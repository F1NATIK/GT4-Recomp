#include "gt4stock_camera.h"

#include "gt4stock_util.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

// raylib's raw-DEFLATE reader (sinfl), declared here: raylib.h and windows.h do not mix.
extern "C" unsigned char *DecompressData(const unsigned char *compData, int compDataSize, int *dataSize);
extern "C" void MemFree(void *ptr);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The Media Foundation GUIDs come from the SDK headers through <initguid.h> (no
// mfuuid.lib), as in the EyeToy service's webcam source.
#include <initguid.h>

#include <mfapi.h>
#include <mfidl.h>
#include <objbase.h>
#endif

namespace gt4stock
{
    // ------------------------------------------------------------------ pure helpers
    std::string CameraInfo::idText() const { return usb ? strf("USB %04X:%04X", vid, pid) : std::string("not USB"); }

    bool CameraPrivacy::blocked() const { return iequals(device, "Deny") || iequals(user, "Deny") || iequals(desktopApps, "Deny"); }

    bool looksInfrared(const std::string &name)
    {
        // webcam-check's rule (analysis/design/webcam-check): "IR" as a word, or "infrared";
        // also a Windows Hello face camera.
        std::string n = " " + lower(name) + " ";
        for (char &c : n)
            if (c == '(' || c == ')' || c == '-' || c == '_' || c == ',')
                c = ' ';
        return n.find(" ir ") != std::string::npos || n.find("infrared") != std::string::npos || n.find("windows hello") != std::string::npos;
    }

    namespace
    {
        int hexValue(const std::string &s, size_t at, size_t digits)
        {
            if (at + digits > s.size())
                return -1;
            int v = 0;
            for (size_t i = 0; i < digits; ++i)
            {
                const char c = s[at + i];
                const int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
                if (d < 0)
                    return -1;
                v = v * 16 + d;
            }
            return v;
        }
    }

    void parseUsbIds(CameraInfo &c)
    {
        const std::string link = lower(c.link);
        const size_t v = link.find("vid_"), p = link.find("pid_");
        const int vid = v == std::string::npos ? -1 : hexValue(link, v + 4, 4);
        const int pid = p == std::string::npos ? -1 : hexValue(link, p + 4, 4);
        c.usb = vid >= 0 && pid >= 0;
        c.vid = c.usb ? static_cast<uint16_t>(vid) : 0;
        c.pid = c.usb ? static_cast<uint16_t>(pid) : 0;
    }

    bool isBrio(const CameraInfo &c) { return c.usb && c.vid == 0x046d && c.pid == 0x085e; }

    int defaultCamera(const std::vector<CameraInfo> &list)
    {
        for (size_t i = 0; i < list.size(); ++i)
            if (!list[i].infrared && iequals(trim(list[i].name), "Logitech BRIO"))
                return static_cast<int>(i);
        for (size_t i = 0; i < list.size(); ++i)
            if (!list[i].infrared && isBrio(list[i]))
                return static_cast<int>(i);
        for (size_t i = 0; i < list.size(); ++i)
            if (!list[i].infrared)
                return static_cast<int>(i);
        return -1;
    }

    int findCamera(const std::vector<CameraInfo> &list, const std::string &name)
    {
        const std::string want = trim(name);
        if (want.empty())
            return -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (!list[i].infrared && iequals(trim(list[i].name), want))
                return static_cast<int>(i);
        return -1;
    }

    bool readShirtDesign(const std::filesystem::path &iso, ShirtDesign &design, std::string &error)
    {
        // GT4.VOL's packed entry: C5 EE F7 FF, the unpacked size negated, then raw DEFLATE.
        constexpr uint64_t kOffset = 0x125c1800ull;
        constexpr int kPacked = 1074, kSize = 28688;
        constexpr uint32_t kCrc = 0x412af037u;
        design = ShirtDesign{};
        std::ifstream f(iso, std::ios::binary);
        if (!f)
            return error = "the disc image cannot be read", false;
        std::vector<uint8_t> packed(kPacked);
        f.seekg(static_cast<std::streamoff>(kOffset));
        if (!f.read(reinterpret_cast<char *>(packed.data()), kPacked))
            return error = "the disc image is too short", false;
        uint32_t magic = 0;
        int32_t negative = 0;
        std::memcpy(&magic, packed.data(), 4);
        std::memcpy(&negative, packed.data() + 4, 4);
        if (magic != 0xfff7eec5u || -negative != kSize)
            return error = "the GT shirt design is not where GT4 (USA) keeps it", false;
        int got = 0;
        unsigned char *raw = DecompressData(packed.data() + 8, kPacked - 8, &got);
        if (!raw)
            return error = "the GT shirt design does not unpack", false;
        std::vector<uint8_t> data(raw, raw + std::max(got, 0));
        MemFree(raw);
        if (static_cast<int>(data.size()) != kSize || crc32(data.data(), data.size()) != kCrc)
            return error = "the GT shirt design fails its check (CRC-32)", false;
        uint32_t w = 0, h = 0;
        std::memcpy(&w, data.data() + 4, 4);
        std::memcpy(&h, data.data() + 8, 4);
        if (std::memcmp(data.data(), "GTSH", 4) != 0 || w == 0 || h == 0 || 16 + uint64_t(w) * h != data.size())
            return error = "not a GT shirt design", false;
        design.width = static_cast<int>(w);
        design.height = static_cast<int>(h);
        design.samples.assign(data.begin() + 16, data.end());
        error.clear();
        return true;
    }

    std::vector<uint8_t> shirtPrintSheet(const ShirtDesign &d, int patternWidth, int &w, int &h)
    {
        w = h = 0;
        if (d.width <= 0 || d.height <= 0 || patternWidth <= 0)
            return {};
        const int pw = patternWidth, ph = static_cast<int>(std::lround(double(pw) * d.height / d.width));
        const int border = pw / 5;
        w = pw + 2 * border;
        h = ph + 2 * border;
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3, 0);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
            {
                const uint8_t s = d.samples[static_cast<size_t>((y * d.height / ph) * d.width + x * d.width / pw)];
                uint8_t *p = &rgb[(static_cast<size_t>(y + border) * w + (x + border)) * 3];
                p[0] = p[1] = p[2] = s;
            }
        return rgb;
    }

    std::vector<CameraInfo> demoCameras()
    {
        struct
        {
            const char *name, *link;
        } const demo[] = {
            {"Integrated Webcam", "\\\\?\\usb#vid_0bda&pid_5591&mi_00#6&00000000&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global"},
            {"Phone (Windows Virtual Camera)", "\\\\?\\swd#vcamdevapi#demo#{e5323777-f976-4f5b-9b55-b94699c46e44}"},
            {"Logitech BRIO", "\\\\?\\usb#vid_046d&pid_085e&mi_00#6&00000000&0&0000#{e5323777-f976-4f5b-9b55-b94699c46e44}\\global"},
        };
        std::vector<CameraInfo> out;
        for (const auto &d : demo)
        {
            CameraInfo c;
            c.index = static_cast<int>(out.size());
            c.name = d.name;
            c.link = d.link;
            parseUsbIds(c);
            c.infrared = looksInfrared(c.name);
            c.demo = true;
            out.push_back(c);
        }
        return out;
    }

    // ------------------------------------------------------------------ Windows
#if defined(_WIN32)
    namespace
    {
        std::string narrow(const wchar_t *text) { return text ? utf8(std::wstring(text)) : std::string(); }

        // Media Foundation, loaded when first needed like the EyeToy service's source, so
        // the launcher also starts where it is missing (Windows N editions). Never freed.
        struct Mf
        {
            HMODULE plat = nullptr, mf = nullptr;
            HRESULT(WINAPI *startup)(ULONG, DWORD) = nullptr;
            HRESULT(WINAPI *shutdown)() = nullptr;
            HRESULT(WINAPI *createAttributes)(IMFAttributes **, UINT32) = nullptr;
            HRESULT(WINAPI *enumDeviceSources)(IMFAttributes *, IMFActivate ***, UINT32 *) = nullptr;

            template <class F>
            static bool bind(HMODULE module, const char *name, F &out)
            {
                out = module ? reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(module, name))) : nullptr;
                return out != nullptr;
            }

            bool load(std::string &error)
            {
                plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                mf = LoadLibraryExW(L"mf.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!plat || !mf)
                    return error = "Media Foundation is not installed on this Windows (an N edition?)", false;
                if (!bind(plat, "MFStartup", startup) || !bind(plat, "MFShutdown", shutdown) || !bind(plat, "MFCreateAttributes", createAttributes) ||
                    !bind(mf, "MFEnumDeviceSources", enumDeviceSources))
                    return error = "Media Foundation's entry points are missing", false;
                return true;
            }
        };

        void listOnThisThread(CameraList &out)
        {
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            Mf api;
            bool started = false;
            IMFAttributes *attributes = nullptr;
            IMFActivate **devices = nullptr;
            UINT32 count = 0;
            do
            {
                if (!api.load(out.error))
                    break;
                if (FAILED(api.startup(MF_VERSION, MFSTARTUP_LITE)))
                {
                    out.error = "Media Foundation did not start";
                    break;
                }
                started = true;
                if (FAILED(api.createAttributes(&attributes, 1)) ||
                    FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
                    FAILED(api.enumDeviceSources(attributes, &devices, &count)))
                {
                    out.error = "Windows could not list the cameras";
                    break;
                }
                // Only the names and links are read: no ActivateObject, so no camera opens.
                for (UINT32 i = 0; i < count; ++i)
                {
                    CameraInfo c;
                    c.index = static_cast<int>(i);
                    WCHAR *text = nullptr;
                    UINT32 length = 0;
                    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &text, &length)))
                    {
                        c.name = narrow(text);
                        CoTaskMemFree(text);
                    }
                    text = nullptr;
                    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &text, &length)))
                    {
                        c.link = narrow(text);
                        CoTaskMemFree(text);
                    }
                    parseUsbIds(c);
                    c.infrared = looksInfrared(c.name);
                    out.devices.push_back(std::move(c));
                }
                out.listed = true;
            } while (false);
            for (UINT32 i = 0; devices && i < count; ++i)
                if (devices[i])
                    devices[i]->Release();
            if (devices)
                CoTaskMemFree(devices);
            if (attributes)
                attributes->Release();
            if (started)
                api.shutdown();
            if (SUCCEEDED(com))
                CoUninitialize();
        }

        std::string regString(HKEY root, const wchar_t *path, const wchar_t *name)
        {
            wchar_t buffer[256];
            DWORD bytes = sizeof(buffer);
            if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS)
                return {};
            return narrow(buffer);
        }
    }

    CameraList listCameras()
    {
        CameraList out;
        // Its own thread: a COM apartment of its own, whatever the caller's is.
        std::thread([&out] { listOnThisThread(out); }).join();
        return out;
    }

    CameraPrivacy readCameraPrivacy()
    {
        static const wchar_t *const store = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
        CameraPrivacy p;
        p.device = regString(HKEY_LOCAL_MACHINE, store, L"Value");
        p.user = regString(HKEY_CURRENT_USER, store, L"Value");
        p.desktopApps = regString(HKEY_CURRENT_USER, (std::wstring(store) + L"\\NonPackaged").c_str(), L"Value");
        return p;
    }
#else
    CameraList listCameras()
    {
        CameraList out;
        out.error = "cameras are listed on Windows only";
        return out;
    }
    CameraPrivacy readCameraPrivacy() { return {}; }
#endif

    // ------------------------------------------------------------------ Cameras
    struct Cameras::Impl
    {
        std::thread worker;
        std::atomic<bool> running{false};
        std::mutex mutex;
        bool ready = false;
        CameraList result;
        CameraPrivacy privacy;

        void join()
        {
            if (worker.joinable())
                worker.join();
        }
    };

    Cameras::Cameras() : impl_(std::make_unique<Impl>()) {}

    Cameras::~Cameras() { impl_->join(); }

    void Cameras::init(bool demo, bool background)
    {
        impl_->join();
        demo_ = demo;
        background_ = background;
        list_ = CameraList{};
        privacy_ = CameraPrivacy{};
        if (demo_)
        {
            list_.listed = true;
            list_.demo = true;
            list_.devices = demoCameras();
            privacy_.device = privacy_.user = privacy_.desktopApps = "Allow";
            return;
        }
        if (background_)
            refresh();
        else
            refreshNow();
    }

    bool Cameras::running() const { return impl_->running; }

    void Cameras::refresh()
    {
        if (demo_)
            return;
        if (!background_)
        {
            refreshNow();
            return;
        }
        if (impl_->running)
            return;
        impl_->join();
        impl_->running = true;
        impl_->worker = std::thread([impl = impl_.get()] {
            CameraList l = listCameras();
            CameraPrivacy p = readCameraPrivacy();
            {
                std::lock_guard lock(impl->mutex);
                impl->result = std::move(l);
                impl->privacy = std::move(p);
                impl->ready = true;
            }
            impl->running = false;
        });
    }

    void Cameras::refreshNow()
    {
        if (demo_)
            return;
        impl_->join();
        list_ = listCameras();
        privacy_ = readCameraPrivacy();
        ++listings_;
    }

    bool Cameras::update()
    {
        if (demo_)
            return false;
        CameraList l;
        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->ready)
                return false;
            impl_->ready = false;
            l = std::move(impl_->result);
            privacy_ = impl_->privacy;
        }
        ++listings_;
        bool changed = l.listed != list_.listed || l.devices.size() != list_.devices.size();
        for (size_t i = 0; !changed && i < l.devices.size(); ++i)
            changed = l.devices[i].name != list_.devices[i].name || l.devices[i].link != list_.devices[i].link;
        list_ = std::move(l);
        return changed;
    }
}
