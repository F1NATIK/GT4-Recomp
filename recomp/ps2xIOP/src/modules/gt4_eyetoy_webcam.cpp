#include "gt4_eyetoy_source.h"

// Host webcam for the GT4 EyeToy service, through Media Foundation's source
// reader (RGB32 output with the reader's own colour conversion). The device is
// opened only in start() (GT4 streaming, after the service's open delay) and
// closed in stop(), so the camera light is on only while GT4 shows the camera
// page. The MF DLLs are loaded when first needed (device listing at module load,
// capture at start()) rather than linked, so GT4.exe still starts on Windows
// editions without Media Foundation (N/KN); once loaded they stay loaded. The
// GUIDs come from the SDK headers through <initguid.h> (no mfuuid.lib).

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <initguid.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <objbase.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

namespace ps2x::iop::gt4::eyetoy
{
    namespace
    {
        template <class T>
        void release(T *&p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        // The DLLs are never freed: LoadLibraryEx only adds a reference, and freeing
        // mfplat/mf after MFShutdown while MF's worker threads wind down risks a crash.
        struct MfApi
        {
            HMODULE plat = nullptr, mf = nullptr, readWrite = nullptr;
            HRESULT(WINAPI *startup)(ULONG, DWORD) = nullptr;
            HRESULT(WINAPI *shutdown)() = nullptr;
            HRESULT(WINAPI *createAttributes)(IMFAttributes **, UINT32) = nullptr;
            HRESULT(WINAPI *createMediaType)(IMFMediaType **) = nullptr;
            HRESULT(WINAPI *enumDeviceSources)(IMFAttributes *, IMFActivate ***, UINT32 *) = nullptr;
            HRESULT(WINAPI *createReader)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **) = nullptr;

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
                readWrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!plat || !mf || !readWrite)
                {
                    error = "Media Foundation is not installed (mfplat/mf/mfreadwrite.dll)";
                    return false;
                }
                if (!bind(plat, "MFStartup", startup) || !bind(plat, "MFShutdown", shutdown) ||
                    !bind(plat, "MFCreateAttributes", createAttributes) || !bind(plat, "MFCreateMediaType", createMediaType) ||
                    !bind(mf, "MFEnumDeviceSources", enumDeviceSources) ||
                    !bind(readWrite, "MFCreateSourceReaderFromMediaSource", createReader))
                {
                    error = "Media Foundation entry points missing";
                    return false;
                }
                return true;
            }
        };

        std::string narrow(const wchar_t *text)
        {
            if (!text)
                return {};
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string out(bytes > 0 ? size_t(bytes - 1) : 0, '\0');
            if (bytes > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
            return out;
        }

        class WebcamSource final : public FrameSource
        {
        public:
            WebcamSource(int device, SourceLog log) : device_(device < 0 ? 0 : device), log_(std::move(log)) {}
            ~WebcamSource() override { stop(); }

            std::string describe() const override { return "webcam " + std::to_string(device_) + " (Media Foundation)"; }
            bool live() const override { return true; }

            bool available() override
            {
                if (present_)
                    return true;
                // Lists the capture devices (no device is activated) on a short-lived
                // thread with its own COM apartment: the caller is the EE thread.
                bool found = false;
                std::thread([this, &found] {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    MfApi api;
                    std::string error;
                    bool started = false;
                    IMFAttributes *attributes = nullptr;
                    IMFActivate **devices = nullptr;
                    UINT32 count = 0;
                    if (!api.load(error))
                        say(2, error);
                    else if (FAILED(api.startup(MF_VERSION, MFSTARTUP_LITE)))
                        say(2, "MFStartup failed");
                    else
                    {
                        started = true;
                        if (SUCCEEDED(api.createAttributes(&attributes, 1)) &&
                            SUCCEEDED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) &&
                            SUCCEEDED(api.enumDeviceSources(attributes, &devices, &count)))
                        {
                            std::string names;
                            for (UINT32 i = 0; i < count; ++i)
                            {
                                WCHAR *wideName = nullptr;
                                UINT32 length = 0;
                                if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wideName, &length)))
                                {
                                    names += (i ? ", " : ": ") + std::to_string(i) + " '" + narrow(wideName) + "'";
                                    CoTaskMemFree(wideName);
                                }
                            }
                            found = static_cast<UINT32>(device_) < count;
                            say(found ? 0 : 2, std::to_string(count) + " video capture devices" + names +
                                                   (found ? "" : "; this one is not there"));
                        }
                        else
                        {
                            say(2, "cannot list video capture devices");
                        }
                    }
                    for (UINT32 i = 0; devices && i < count; ++i)
                        release(devices[i]);
                    if (devices)
                        CoTaskMemFree(devices);
                    release(attributes);
                    if (started)
                        api.shutdown();
                    if (SUCCEEDED(com))
                        CoUninitialize();
                }).join();
                present_ = found; // a missing camera is looked for again at the next module load
                return found;
            }

            bool start(uint32_t width, uint32_t height, uint32_t) override
            {
                stop();
                running_ = true;
                thread_ = std::thread([this, width, height] { capture(width, height); });
                return true;
            }

            void stop() override
            {
                running_ = false;
                if (thread_.joinable())
                    thread_.join(); // ReadSample returns within one camera frame
                std::lock_guard lock(mutex_);
                have_ = false;
            }

            bool latest(RgbImage &out, uint64_t &sequence) override
            {
                std::lock_guard lock(mutex_);
                if (!have_)
                    return false;
                out = frame_;
                sequence = sequence_;
                return true;
            }

        private:
            void say(int level, const std::string &text)
            {
                if (log_)
                    log_(level, "[GT4/EYETOY] webcam " + std::to_string(device_) + ": " + text);
            }

            static bool chooseNativeType(IMFSourceReader *reader, uint32_t width, uint32_t height, UINT32 &w, UINT32 &h)
            {
                // The smallest native size that covers the opened size (GT4 opens
                // 160x112, 320x240 or 640x480), else the largest one.
                IMFMediaType *best = nullptr;
                uint64_t bestArea = 0;
                bool bestCovers = false;
                for (DWORD i = 0;; ++i)
                {
                    IMFMediaType *type = nullptr;
                    if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type)))
                        break;
                    UINT32 tw = 0, th = 0;
                    if (SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &tw, &th)) && tw && th)
                    {
                        const uint64_t area = uint64_t(tw) * th;
                        const bool covers = tw >= width && th >= height;
                        const bool better = !best || (covers && (!bestCovers || area < bestArea)) || (!covers && !bestCovers && area > bestArea);
                        if (better)
                        {
                            release(best);
                            best = type;
                            type = nullptr;
                            bestArea = area;
                            bestCovers = covers;
                            w = tw;
                            h = th;
                        }
                    }
                    release(type);
                }
                if (!best)
                    return false;
                const HRESULT hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, best);
                release(best);
                return SUCCEEDED(hr);
            }

            static bool currentLayout(IMFSourceReader *reader, UINT32 &w, UINT32 &h, LONG &stride)
            {
                IMFMediaType *type = nullptr;
                if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)))
                    return false;
                const bool ok = SUCCEEDED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h)) && w && h;
                UINT32 defaultStride = 0;
                // RGB32 without MF_MT_DEFAULT_STRIDE is bottom-up.
                stride = SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &defaultStride)) ? static_cast<LONG>(defaultStride)
                                                                                           : -static_cast<LONG>(w * 4);
                release(type);
                return ok;
            }

            void publish(IMFSample *sample, UINT32 w, UINT32 h, LONG stride, uint32_t width, uint32_t height)
            {
                IMFMediaBuffer *buffer = nullptr;
                if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
                    return;
                IMF2DBuffer *buffer2d = nullptr;
                BYTE *scan0 = nullptr;
                LONG pitch = 0;
                BYTE *data = nullptr;
                DWORD length = 0;
                bool locked2d = false, locked = false;
                if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch)))
                {
                    locked2d = true;
                }
                else if (SUCCEEDED(buffer->Lock(&data, nullptr, &length)))
                {
                    locked = true;
                    pitch = stride;
                    scan0 = stride < 0 ? data + size_t(h - 1) * size_t(-stride) : data;
                    if (size_t(std::abs(stride)) * h > length)
                        scan0 = nullptr;
                }
                if (scan0)
                {
                    RgbImage full;
                    full.width = w;
                    full.height = h;
                    full.rgb.resize(size_t(w) * h * 3);
                    for (UINT32 y = 0; y < h; ++y)
                    {
                        const BYTE *row = scan0 + static_cast<ptrdiff_t>(y) * pitch;
                        uint8_t *d = &full.rgb[size_t(y) * w * 3];
                        for (UINT32 x = 0; x < w; ++x)
                        {
                            d[x * 3 + 0] = row[x * 4 + 2]; // BGRX
                            d[x * 3 + 1] = row[x * 4 + 1];
                            d[x * 3 + 2] = row[x * 4 + 0];
                        }
                    }
                    RgbImage scaled;
                    scaleCover(full, width, height, scaled);
                    std::lock_guard lock(mutex_);
                    frame_ = std::move(scaled);
                    ++sequence_;
                    have_ = true;
                }
                if (locked2d)
                    buffer2d->Unlock2D();
                if (locked)
                    buffer->Unlock();
                release(buffer2d);
                release(buffer);
            }

            void capture(uint32_t width, uint32_t height)
            {
                const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                MfApi api;
                std::string error;
                bool started = false;
                IMFAttributes *attributes = nullptr, *readerAttributes = nullptr;
                IMFActivate **devices = nullptr;
                UINT32 count = 0;
                IMFMediaSource *source = nullptr;
                IMFSourceReader *reader = nullptr;
                IMFMediaType *output = nullptr;
                do
                {
                    if (!api.load(error))
                    {
                        say(2, error);
                        break;
                    }
                    if (FAILED(api.startup(MF_VERSION, MFSTARTUP_LITE)))
                    {
                        say(2, "MFStartup failed");
                        break;
                    }
                    started = true;
                    if (FAILED(api.createAttributes(&attributes, 1)) ||
                        FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)) ||
                        FAILED(api.enumDeviceSources(attributes, &devices, &count)))
                    {
                        say(2, "cannot list video capture devices");
                        break;
                    }
                    if (static_cast<UINT32>(device_) >= count)
                    {
                        say(2, "no such device (" + std::to_string(count) + " video capture devices)");
                        break;
                    }
                    WCHAR *wideName = nullptr;
                    UINT32 nameLength = 0;
                    std::string name = "?";
                    if (SUCCEEDED(devices[device_]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &wideName, &nameLength)))
                    {
                        name = narrow(wideName);
                        CoTaskMemFree(wideName);
                    }
                    if (FAILED(devices[device_]->ActivateObject(IID_PPV_ARGS(&source))))
                    {
                        say(2, "'" + name + "' cannot be opened (in use, or camera access is off in Windows privacy settings)");
                        break;
                    }
                    if (FAILED(api.createAttributes(&readerAttributes, 1)) ||
                        FAILED(readerAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
                        FAILED(api.createReader(source, readerAttributes, &reader)))
                    {
                        say(2, "'" + name + "': no source reader");
                        break;
                    }
                    UINT32 nativeW = 0, nativeH = 0;
                    if (!chooseNativeType(reader, width, height, nativeW, nativeH))
                        say(1, "'" + name + "': keeping the default native format");
                    if (FAILED(api.createMediaType(&output)) || FAILED(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
                        FAILED(output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
                        FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, output)))
                    {
                        say(2, "'" + name + "': no RGB32 output");
                        break;
                    }
                    UINT32 w = 0, h = 0;
                    LONG stride = 0;
                    if (!currentLayout(reader, w, h, stride))
                    {
                        say(2, "'" + name + "': unknown frame size");
                        break;
                    }
                    say(0, "'" + name + "' opened " + std::to_string(w) + "x" + std::to_string(h) + ", delivering " +
                               std::to_string(width) + "x" + std::to_string(height));
                    while (running_)
                    {
                        DWORD stream = 0, flags = 0;
                        LONGLONG time = 0;
                        IMFSample *sample = nullptr;
                        const HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream,
                                                              &flags, &time, &sample);
                        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
                        {
                            release(sample);
                            say(2, "'" + name + "' stopped delivering frames");
                            break;
                        }
                        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !currentLayout(reader, w, h, stride))
                        {
                            release(sample);
                            break;
                        }
                        if (sample)
                            publish(sample, w, h, stride, width, height);
                        release(sample);
                    }
                    say(0, "'" + name + "' closed");
                } while (false);
                release(output);
                release(reader);
                release(readerAttributes);
                if (source)
                    source->Shutdown();
                release(source);
                for (UINT32 i = 0; devices && i < count; ++i)
                    release(devices[i]);
                if (devices)
                    CoTaskMemFree(devices);
                release(attributes);
                if (started)
                    api.shutdown();
                if (SUCCEEDED(com))
                    CoUninitialize();
            }

            const int device_;
            SourceLog log_;
            bool present_ = false;
            std::atomic<bool> running_{false};
            std::thread thread_;
            std::mutex mutex_;
            RgbImage frame_;
            uint64_t sequence_ = 0;
            bool have_ = false;
        };
    }

    std::unique_ptr<FrameSource> createWebcamSource(int device, SourceLog log)
    {
        return std::make_unique<WebcamSource>(device, std::move(log));
    }
}

#else

namespace ps2x::iop::gt4::eyetoy
{
    std::unique_ptr<FrameSource> createWebcamSource(int, SourceLog)
    {
        return nullptr;
    }
}

#endif
