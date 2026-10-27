#include "external/glad.h"

#include "gs_gl_present.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

namespace
{
    struct Slot
    {
        GLuint tex = 0;
        int scale = 0, scaleY = 0; // the scale tex was made for
        uint32_t rows = 512;       // and its native rows (512, or 1024 for 1080i)
        GLsync ready = nullptr;    // worker: composition done
        GLsync consumed = nullptr; // host: copy done
        uint32_t width = 0, height = 0;
        uint64_t serial = 0;
        uint64_t tick = 0; // the latch tick of its frame (notePresentTick)
        gsgl::FieldInfo field; // its fields (notePresentField)
    };

    std::mutex g_mutex;
    Slot g_slots[3];
    int g_latest = -1;     // newest published slot
    int g_hostHeld = -1;   // slot the host is copying from
    int g_composing = -1;  // slot the worker is composing into
    uint64_t g_serial = 0;
    uint64_t g_hostSerial = 0;
    std::atomic<bool> g_active{false};
    std::atomic<uint64_t> g_composeTick{0}; // worker: the frame being composed
    std::atomic<int> g_composeParity{-1}, g_composeMode{0}; // worker: its fields
    std::atomic<uint32_t> g_composeCrt{0};                   // worker: its video mode
    std::atomic<int> g_composeLines{0};                      // worker: its display lines
    std::atomic<uint32_t> g_presentRows{512};                // worker: rows the slots must hold (native)
    gsgl::FieldInfo g_hostField; // host thread: the frame it copied last

    // The stock builds (GT3_STOCK_BUILD, GT4_STOCK_BUILD): the PS2's own
    // resolution and interlaced fields, always.
    int initialScale()
    {
#if defined(GT3_STOCK_BUILD) || defined(GT4_STOCK_BUILD)
        return 1;
#else
        const char *v = std::getenv("GT3_GS_GL_SCALE");
        return (v && *v) ? std::clamp(std::atoi(v), 1, 8) : 1;
#endif
    }
    bool initialProgressive()
    {
#if defined(GT3_STOCK_BUILD) || defined(GT4_STOCK_BUILD)
        return false;
#else
        const char *v = std::getenv("GT3_GS_GL_PROGRESSIVE");
        return v && *v && *v != '0';
#endif
    }
    int initialDeinterlace()
    {
        // GT4 builds: GT4_DEINTERLACE (the Classic edition's setting) is read by
        // the host loop once gt4-start.env is in the environment, which then
        // calls requestDeinterlace (ps2_runtime.cpp); this is the start value.
#if defined(GT4_STOCK_BUILD)
        return 4; // blend: woven fields, the lines blended by the picture pass
#else
        const char *v = std::getenv("GT3_DEINTERLACE");
        const std::string_view s = v ? v : "";
        return s == "weave" ? 1 : s == "bob" ? 2 : s == "double" ? 3 : 0;
#endif
    }
    std::atomic<int> g_scale{initialScale()};
    std::atomic<bool> g_progressive{initialProgressive()};
    std::atomic<int> g_deinterlace{initialDeinterlace()};
    std::atomic<int> g_presentScale{0};
    std::atomic<int> g_presentScaleY{0};
}

int gsgl::renderScale() { return g_scale.load(std::memory_order_relaxed); }
bool gsgl::progressiveScan() { return g_progressive.load(std::memory_order_relaxed); }

void gsgl::requestRenderScale(int scale, bool progressive)
{
    g_scale.store(std::clamp(scale, 1, 8), std::memory_order_relaxed);
    g_progressive.store(progressive, std::memory_order_relaxed);
}

int gsgl::deinterlaceMode() { return g_deinterlace.load(std::memory_order_relaxed); }
void gsgl::requestDeinterlace(int mode) { g_deinterlace.store(std::clamp(mode, 0, 5), std::memory_order_relaxed); }

void gsgl::wideScaleSplit(int scale, int &scaleX, int &scaleY)
{
#if defined(GT3_STOCK_BUILD) || defined(GT4_STOCK_BUILD)
    scaleX = scaleY = scale; // the stock build: no wide pictures of its own
#else
    // Across / down samples that suit the picture: a frame A:1 wide shown on
    // square pixels wants 640 sx : 448 sy = A, so sx / sy = 0.7 A.
    static const float ratio = [] {
        const char *off = std::getenv("GT3_GS_GL_WIDE_SPLIT");
        const char *v = std::getenv("GT3_ASPECT");
        if ((off && std::strcmp(off, "0") == 0) || !v || !*v)
            return 1.0f;
        const char *colon = std::strchr(v, ':');
        const float h = colon ? static_cast<float>(std::atof(colon + 1)) : 1.0f;
        const float a = h > 0.0f ? static_cast<float>(std::atof(v)) / h : 0.0f;
        return a > 0.0f && a < 16.0f ? 0.7f * a : 1.0f;
    }();
    scaleX = scaleY = scale;
    if (scale <= 1 || ratio < 1.35f)
        return;
    scaleY = std::max(1, static_cast<int>(std::lround(static_cast<float>(scale) / std::sqrt(ratio))));
    scaleX = std::clamp(static_cast<int>(std::lround(static_cast<float>(scale * scale) / static_cast<float>(scaleY))), scaleY, 8);
#endif
}

int gsgl::presentScale()
{
    const int s = g_presentScale.load(std::memory_order_relaxed);
    if (s > 0)
        return s;
    int sx = 1, sy = 1;
    wideScaleSplit(renderScale(), sx, sy);
    return sx;
}

int gsgl::presentScaleY()
{
    const int s = g_presentScaleY.load(std::memory_order_relaxed);
    if (s > 0)
        return s;
    int sx = 1, sy = 1;
    wideScaleSplit(renderScale(), sx, sy);
    return sy;
}

void gsgl::setPresentScale(int scaleX, int scaleY)
{
    g_presentScale.store(scaleX, std::memory_order_relaxed);
    g_presentScaleY.store(scaleY, std::memory_order_relaxed);
}

unsigned gsgl::beginPresentSlot()
{
    GLsync waitFor = nullptr;
    int chosen = -1;
    const int scale = presentScale(), scaleY = presentScaleY();
    const uint32_t rows = presentRows();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 1; i <= 3; ++i)
        {
            const int s = ((g_latest < 0 ? 0 : g_latest) + i) % 3;
            if (s != g_hostHeld && s != g_latest)
            {
                chosen = s;
                break;
            }
        }
        if (chosen < 0)
            chosen = (g_latest + 1) % 3;
        g_composing = chosen;
        Slot &slot = g_slots[chosen];
        waitFor = slot.consumed;
        slot.consumed = nullptr;
        if (slot.ready)
        {
            glDeleteSync(slot.ready);
            slot.ready = nullptr;
        }
    }
    Slot &slot = g_slots[chosen];
    if (waitFor)
    {
        // The host's copy out of this slot must be done before it is overwritten
        // (or, at a new scale, deleted: then on the CPU, as the other context used it).
        if (slot.tex && (slot.scale != scale || slot.scaleY != scaleY || slot.rows < rows))
            glClientWaitSync(waitFor, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        else
            glWaitSync(waitFor, 0, GL_TIMEOUT_IGNORED);
        glDeleteSync(waitFor);
    }
    if (slot.tex && (slot.scale != scale || slot.scaleY != scaleY || slot.rows < rows))
    {
        glDeleteTextures(1, &slot.tex);
        slot.tex = 0;
    }
    if (!slot.tex)
    {
        glGenTextures(1, &slot.tex);
        glBindTexture(GL_TEXTURE_2D, slot.tex);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 640 * scale, static_cast<GLsizei>(rows) * scaleY);
        slot.rows = rows;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);
        slot.scale = scale;
        slot.scaleY = scaleY;
    }
    return slot.tex;
}

void gsgl::endPresentSlot(uint32_t width, uint32_t height)
{
    GLsync ready = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush(); // the fence must reach the GPU before another context waits on it
    std::lock_guard<std::mutex> lock(g_mutex);
    Slot &slot = g_slots[g_composing];
    slot.ready = ready;
    slot.width = width;
    slot.height = height;
    slot.serial = ++g_serial;
    slot.tick = g_composeTick.load(std::memory_order_relaxed);
    slot.field = FieldInfo{g_composeParity.load(std::memory_order_relaxed), g_composeMode.load(std::memory_order_relaxed),
                           g_composeCrt.load(std::memory_order_relaxed), g_composeLines.load(std::memory_order_relaxed)};
    g_latest = g_composing;
    g_composing = -1;
    g_active.store(true, std::memory_order_release);
}

int gsgl::hostCopyLatestFrame(unsigned dstTexture, uint32_t dstWidth, uint32_t dstHeight, uint32_t &width, uint32_t &height, int &frameScale,
                              int &frameScaleY, uint64_t maxTick)
{
    int s = -1;
    GLsync ready = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_latest < 0 || g_slots[g_latest].serial == g_hostSerial)
            return 0;
        s = g_latest;
        if (maxTick != ~0ull && g_slots[s].tick > maxTick)
        {
            // Published early (grid pacing): an older frame not copied yet
            // whose latch tick is due, if one is still held (not the slot
            // being composed into).
            int best = -1;
            for (int i = 0; i < 3; ++i)
            {
                const Slot &c = g_slots[i];
                if (i == g_composing || c.serial == 0 || c.serial <= g_hostSerial || c.tick > maxTick)
                    continue;
                if (best < 0 || c.serial > g_slots[best].serial)
                    best = i;
            }
            if (best >= 0)
                s = best;
        }
        frameScale = g_slots[s].scale;
        frameScaleY = g_slots[s].scaleY;
        width = g_slots[s].width;
        height = g_slots[s].height;
        if (width > dstWidth || height > dstHeight)
            return 2; // not taken: the host makes room first
        g_hostHeld = s;
        ready = g_slots[s].ready;
        g_hostSerial = g_slots[s].serial;
        g_hostField = g_slots[s].field;
    }
    if (ready)
        glWaitSync(ready, 0, GL_TIMEOUT_IGNORED);
    glCopyImageSubData(g_slots[s].tex, GL_TEXTURE_2D, 0, 0, 0, 0, dstTexture, GL_TEXTURE_2D, 0, 0, 0, 0, static_cast<GLsizei>(width),
                       static_cast<GLsizei>(height), 1);
    GLsync consumed = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_slots[s].consumed)
        glDeleteSync(g_slots[s].consumed);
    g_slots[s].consumed = consumed;
    g_hostHeld = -1;
    return 1;
}

bool gsgl::gpuPresentationActive()
{
    return g_active.load(std::memory_order_acquire);
}

void gsgl::notePresentTick(uint64_t tick)
{
    g_composeTick.store(tick, std::memory_order_relaxed);
}

void gsgl::notePresentField(int parity, int mode, uint32_t crt, int lines)
{
    g_composeParity.store(parity == 0 || parity == 1 ? parity : -1, std::memory_order_relaxed);
    g_composeMode.store(mode, std::memory_order_relaxed);
    g_composeCrt.store(crt, std::memory_order_relaxed);
    g_composeLines.store(lines, std::memory_order_relaxed);
}

void gsgl::setPresentRows(uint32_t rows)
{
    g_presentRows.store(rows <= 512u ? 512u : 1024u, std::memory_order_relaxed);
}

uint32_t gsgl::presentRows()
{
    return g_presentRows.load(std::memory_order_relaxed);
}

gsgl::FieldInfo gsgl::copiedFrameField()
{
    return g_hostField; // written and read on the host thread only
}

uint64_t gsgl::composingTick()
{
    return g_composeTick.load(std::memory_order_relaxed);
}

gsgl::FieldInfo gsgl::composingField()
{
    return FieldInfo{g_composeParity.load(std::memory_order_relaxed), g_composeMode.load(std::memory_order_relaxed),
                     g_composeCrt.load(std::memory_order_relaxed), g_composeLines.load(std::memory_order_relaxed)};
}
