#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include "runtime/ps2_input_profile.h"

class PSPadBackend
{
public:
    PSPadBackend();
    ~PSPadBackend();

    // 32-byte packet: byte 1 = 0x73 for sticks/buttons, or 0x79 when
    // bytes 8..19 contain valid analog pressures (including valid zeroes).
    bool readState(int port, int slot, uint8_t *data, size_t size);
    // Called only by the window/event owner thread.
    void poll();
    // Load on the owner thread before polling. Failed loads retain the profile.
    bool loadProfile(const char *path, std::string &error);
    // The same from text: profile rows separated by ';' or new lines (PS2X_INPUT).
    bool loadProfileText(const std::string &rows, std::string &error);
    // Host adapters supply already-polled device states; never polls OS APIs.
    void publishDevices(const ps2_pad::Devices &devices);
    // The emulated controller's motors (any thread); poll() drives the host
    // controller's motors from them.
    void setActuators(bool smallMotor, uint8_t large);
    // Window thread, once the game has stopped (exit skips destructors): the host
    // controller's motors off if this process set them (GT4: player 2's too).
    void stopRumble();
    // GT4 (not the Classic edition), tests: GT4_HUD_ZOOM_SELFTEST=1 logs what GT4 gets from the keyboard and from player 1's
    // mapping with the Race HUD zoom key and button down (HUD hand-off 9: neither reaches GT4's own controls).
    void zoomSelfTest();
private:
    bool m_zoomDown = false; // the Race HUD zoom's controller button was down at the last poll
    void applyRumble();
    std::atomic<uint32_t> m_actuators{0};
    uint32_t m_appliedActuators = 0;
    int m_rumblePad = -1;
    double m_rumbleRetry = 0;
    std::mutex m_mutex;
    std::array<uint8_t, 32> m_snapshot{};
    ps2_pad::InputProfile m_profile = ps2_pad::InputProfile::defaults();
    std::array<std::string, 16> m_deviceInventory{};
};

#endif
