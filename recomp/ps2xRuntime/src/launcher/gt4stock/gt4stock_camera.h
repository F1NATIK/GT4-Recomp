#pragma once

// GT4 Recompilation launcher (Stock edition): the EyeToy camera's webcam list.
//
// GT4.exe's EyeToy service (analysis/phase5b/eyetoy, in wheel-glue's all-in-one
// patch) reads GT4_EYETOY: absent (no camera), shirt (the built-in GT shirt
// picture, read from the disc), image:<file>, or webcam:<n>, where n is the
// device's place in Media Foundation's list of video capture devices
// (MFEnumDeviceSources, gt4_eyetoy_webcam.cpp). The launcher reads that same list
// in the same way: the devices are listed, never activated, so no camera is opened
// and no camera light comes on. It keeps the chosen camera by name
// (GT4L_EYETOY_CAMERA) and sets n again whenever the list changes order.
//
// Infrared cameras (Windows Hello) are never offered: their grey picture cannot
// show the red reflection GT4 counts. Media Foundation does not list them as
// capture devices on the developer's PC [webcam-check listing]; a name that looks
// infrared is filtered as well.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gt4stock
{
    // The design GT4 looks for: eyetoy/gtshirt.bin from the disc (224x128 grey samples), the
    // same file the EyeToy service's built-in picture is made from.
    struct ShirtDesign
    {
        int width = 0, height = 0;
        std::vector<uint8_t> samples; // width * height, 0 dark .. 255 light
    };
    // Reads it from the GT4 (USA) disc image at its fixed place (ISO byte 0x125c1800, GT4.VOL's
    // packed entry, CRC-32 0x412af037; as analysis/phase5b/eyetoy/gtshirt_target.py). Read only.
    bool readShirtDesign(const std::filesystem::path &iso, ShirtDesign &design, std::string &error);
    // A sheet to print or show on a phone (gtshirt_target.py --print): the pattern patternWidth
    // pixels wide, white on black, with a black border of a fifth of its width. RGB, w x h.
    std::vector<uint8_t> shirtPrintSheet(const ShirtDesign &design, int patternWidth, int &w, int &h);

    struct CameraInfo
    {
        int index = -1;        // its place in Media Foundation's list: the n of GT4_EYETOY=webcam:<n>
        std::string name;      // the friendly name Windows shows
        std::string link;      // the symbolic link (USB ids)
        bool usb = false;
        uint16_t vid = 0, pid = 0;
        bool infrared = false; // looksInfrared(name): never offered, never chosen
        bool demo = false;
        std::string idText() const; // "USB 046D:085E" / "not USB"
    };

    struct CameraList
    {
        bool listed = false;              // a listing finished (or the demo set)
        std::string error;                // why the list could not be read (no Media Foundation...)
        std::vector<CameraInfo> devices;  // the whole list, in Media Foundation's order
        bool demo = false;
    };

    // Windows' camera privacy switches (registry, read only): "Allow", "Deny", or "" (not set).
    struct CameraPrivacy
    {
        std::string device;      // HKLM ...\ConsentStore\webcam Value
        std::string user;        // HKCU ...\ConsentStore\webcam Value
        std::string desktopApps; // HKCU ...\ConsentStore\webcam\NonPackaged Value
        bool blocked() const;    // any of them "Deny": GT4.exe cannot open a webcam
    };

    // ---- pure helpers (unit-tested) ----
    bool looksInfrared(const std::string &name);  // "IR" as a word, "infrared" or "Windows Hello" in the name
    void parseUsbIds(CameraInfo &camera);         // vid_xxxx&pid_xxxx from the symbolic link
    bool isBrio(const CameraInfo &camera);        // Logitech BRIO: USB 046D:085E
    // The camera offered first: the Logitech BRIO (its name, else its USB id), else the
    // first camera that is not infrared. A position in the list, -1 for none.
    int defaultCamera(const std::vector<CameraInfo> &list);
    // The chosen camera by name (case-insensitive), never an infrared one; -1 when it is
    // not connected. The position is the n of webcam:<n>.
    int findCamera(const std::vector<CameraInfo> &list, const std::string &name);
    // The demo set: the developer's list as webcam-check read it (Integrated Webcam, the phone's
    // Windows virtual camera, the Logitech BRIO at #2).
    std::vector<CameraInfo> demoCameras();

    // ---- Windows ----
    // Reads Media Foundation's list on a short-lived thread with its own COM apartment
    // (as the EyeToy service does). Nothing is activated.
    CameraList listCameras();
    CameraPrivacy readCameraPrivacy();

    // The list for the pages: read in the background in a normal session (a listing
    // can take a moment), at once in the test modes, fixed in the demo.
    class Cameras
    {
    public:
        Cameras();
        ~Cameras();
        Cameras(const Cameras &) = delete;
        Cameras &operator=(const Cameras &) = delete;

        void init(bool demo, bool background);
        void refresh();      // starts a new listing (ignored while one runs)
        void refreshNow();   // lists at once (Play, test modes)
        bool update();       // takes a finished background listing; true when the list changed
        const CameraList &list() const { return list_; }
        const CameraPrivacy &privacy() const { return privacy_; }
        bool running() const;
        int listings() const { return listings_; }

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
        CameraList list_;
        CameraPrivacy privacy_;
        bool demo_ = false, background_ = false;
        int listings_ = 0;
    };
}
