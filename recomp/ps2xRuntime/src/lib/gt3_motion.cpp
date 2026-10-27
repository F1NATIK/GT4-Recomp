#include "gt3_motion.h"

#include "runtime/ps2_vu1.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace gt3motion
{
    namespace
    {
        constexpr uint64_t kImage = 0xa15dc42ffe249fd2ull;

        // Entry stubs that call the loader at 0x0 (matrix, viewport); a kick
        // one instruction later (the XTOP in the delay slot) keeps the
        // registers of an earlier batch but runs the same routine.
        bool loadingEntry(uint32_t pc)
        {
            return pc >= 0x18d0u && pc <= 0x1c68u && (pc - 0x18d0u) % 0x28u == 0u;
        }

        uint64_t mix(uint64_t h, uint64_t v)
        {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h *= 0xff51afd7ed558ccdull;
            return h ^ (h >> 33);
        }

        // Inverse of the 3x3 matrix a[i][k] = m[i][col[k]] (rows 0..2: the
        // local x, y, z; columns: clip x, y, w).
        bool invertXyw(const double m[4][4], double out[3][3])
        {
            static const int col[3] = {0, 1, 3};
            double a[3][3];
            for (int i = 0; i < 3; ++i)
                for (int k = 0; k < 3; ++k)
                    a[i][k] = m[i][col[k]];
            const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
            if (std::fabs(det) < 1e-20)
                return false;
            const double d = 1.0 / det;
            out[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * d;
            out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * d;
            out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * d;
            out[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * d;
            out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * d;
            out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * d;
            out[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * d;
            out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * d;
            out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * d;
            return true;
        }

        uint64_t read64(const uint8_t *p)
        {
            uint64_t v;
            std::memcpy(&v, p, 8);
            return v;
        }
    }

    Tracker &Tracker::instance()
    {
        static Tracker tracker;
        return tracker;
    }

    Tracker::Tracker()
    {
        if (const char *v = std::getenv("GT3_MOTION_DEBUG"); v && *v && *v != '0')
        {
            m_log = std::atoi(v);
            m_enabled.store(true, std::memory_order_relaxed);
        }
    }

    void Tracker::beginKick(uint64_t, uint32_t startPC, bool resume, uint32_t top, const uint8_t *vuData, uint64_t vsyncTick)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        // Frames are grouped by the vsync of their kicks: at 60 fps GT3 draws a
        // frame's display list within one vsync period (checked by the
        // matched-batch statistics).
        if (vsyncTick != m_frameTick)
        {
            if (m_frameTick != ~0ull)
                finishFrame();
            m_frameTick = vsyncTick;
        }
        ++m_stats.kicks;
        if (resume)
            return; // MSCNT: the same batch goes on
        const uint32_t routine = loadingEntry(startPC) ? startPC : startPC - 8u;
        uint64_t h = routine;
        for (uint32_t i = 0; i < 8; ++i)
            h = mix(h, read64(vuData + ((top * 16u + i * 8u) & 0x3FFFu)));
        const uint32_t occurrence = m_occurrences[h]++;
        m_key = mix(h, occurrence);
        m_haveBatch = false;
        m_active = true;
    }

    void Tracker::startBatch(const VU1State &s)
    {
        m_haveBatch = true;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                m_batch.m[r][c] = s.vf[28 + r][c];
        for (int c = 0; c < 3; ++c)
        {
            m_batch.scale[c] = s.vf[25][c];
            m_batch.offset[c] = s.vf[26][c];
        }
        m_current[m_key] = m_batch;
        m_main = s.vf[25][0] == 320.0f && s.vf[25][1] == -224.0f;
        m_matched = false;
        if (!m_main)
            return;
        ++m_stats.mainBatches;
        const auto it = m_previous.find(m_key);
        if (it != m_previous.end() && invertXyw(m_batch.m, m_inverse))
        {
            m_prev = it->second;
            m_matched = true;
            ++m_stats.matched;
        }
    }

    void Tracker::packet(const VU1State &s, uint64_t imageHash, const uint8_t *p, uint32_t bytes)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (!m_active || imageHash != kImage)
            return;
        if (!m_haveBatch)
            startBatch(s);
        if (!m_main)
            return; // mirror, reflection maps: not the picture's motion
        const Batch &b = m_batch, &q = m_prev;
        uint32_t off = 0;
        while (off + 16u <= bytes)
        {
            const uint64_t lo = read64(p + off), hi = read64(p + off + 8);
            off += 16u;
            const uint32_t nloop = static_cast<uint32_t>(lo & 0x7FFFu);
            const bool eop = ((lo >> 15) & 1u) != 0u;
            const uint32_t flg = static_cast<uint32_t>((lo >> 58) & 3u);
            uint32_t nreg = static_cast<uint32_t>((lo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            if (((lo >> 46) & 1u) != 0u) // PRE: the tag sets PRIM (and restarts the primitive)
            {
                m_primType = static_cast<uint32_t>((lo >> 47) & 7u);
                m_queued = 0;
            }
            if (flg == 0u) // PACKED
            {
                float qValue = 0.0f; // the vertex's ST.Q = 1/w
                for (uint32_t i = 0; i < nloop; ++i)
                    for (uint32_t r = 0; r < nreg; ++r, off += 16u)
                    {
                        if (off + 16u > bytes)
                            return;
                        const uint32_t reg = static_cast<uint32_t>((hi >> (4u * r)) & 0xFu);
                        if (reg == 0x2u) // ST: S, T, Q floats
                        {
                            std::memcpy(&qValue, p + off + 8, 4);
                            continue;
                        }
                        // XYZF2 / XYZ2 / XYZF3 / XYZ3 (GT3 sends ST, RGBAQ, XYZF2).
                        if (reg != 0x4u && reg != 0x5u && reg != 0xCu && reg != 0xDu)
                            continue;
                        uint32_t w[4];
                        std::memcpy(w, p + off, 16);
                        const bool adc = reg <= 0x5u && (w[3] & 0x8000u) != 0u; // XYZ2/XYZF2: no drawing kick
                        // GS coordinates: 12.4 fixed point.
                        const double sx = (w[0] & 0xFFFFu) / 16.0, sy = (w[1] & 0xFFFFu) / 16.0;
                        Vertex vx{static_cast<float>(sx - b.offset[0] + 320.0), static_cast<float>(sy - b.offset[1] + 224.0), qValue, 0.0f, 0.0f};
                        vx.px = vx.x;
                        vx.py = vx.y;
                        if (m_matched && qValue > 0.0f)
                        {
                            const double cw = 1.0 / qValue;
                            const double rhs[3] = {(sx - b.offset[0]) / b.scale[0] * cw - b.m[3][0], (sy - b.offset[1]) / b.scale[1] * cw - b.m[3][1],
                                                   cw - b.m[3][3]};
                            double v[3];
                            for (int c = 0; c < 3; ++c)
                                v[c] = rhs[0] * m_inverse[0][c] + rhs[1] * m_inverse[1][c] + rhs[2] * m_inverse[2][c];
                            // Check: the solved position must give the vertex's
                            // own GS Z too (a Q that is not 1/w, e.g. a sky
                            // drawn with Q = 1, gives a wrong depth and a Z far
                            // off; GS Z is too coarse to solve with but fine to
                            // check against).
                            const double zc = v[0] * b.m[0][2] + v[1] * b.m[1][2] + v[2] * b.m[2][2] + b.m[3][2];
                            const double zPred = zc / cw * b.scale[2] + b.offset[2];
                            const double zAct = static_cast<double>(w[2]) / 16.0;
                            const bool depthOk = std::fabs(zPred - zAct) <= 8.0 + 0.25 * std::fabs(zAct);
                            if (!depthOk)
                                ++m_stats.rejected;
                            double clip[4];
                            for (int c = 0; c < 4; ++c)
                                clip[c] = v[0] * q.m[0][c] + v[1] * q.m[1][c] + v[2] * q.m[2][c] + q.m[3][c];
                            if (depthOk && clip[3] > 1e-9)
                            {
                                vx.px = static_cast<float>(clip[0] / clip[3] * q.scale[0] + 320.0);
                                vx.py = static_cast<float>(clip[1] / clip[3] * q.scale[1] + 224.0);
                                ++m_stats.vertices;
                                m_stats.motionSum += std::hypot(vx.px - vx.x, vx.py - vx.y);
                                if (m_samples.size() < 20000u && (m_stats.vertices % 3u) == 0u)
                                    m_samples.push_back(Sample{vx.x, vx.y, vx.px, vx.py});
                            }
                        }
                        addVertex(vx, adc);
                    }
            }
            else if (flg == 1u) // REGLIST: nloop * nreg 64-bit values
                off += ((nloop * nreg + 1u) / 2u) * 16u;
            else // IMAGE
                off += nloop * 16u;
            if (eop)
                break;
        }
    }

    // Primitive assembly as the GS does for the types GT3's microprogram
    // sends: triangle lists (3), strips (4), fans (5); ADC skips the kick.
    void Tracker::addVertex(const Vertex &v, bool adc)
    {
        if (m_primType < 3u || m_primType > 5u)
            return;
        if (m_queued == 3)
        {
            if (m_primType == 4u) // strip: the last two stay
            {
                m_queue[0] = m_queue[1];
                m_queue[1] = m_queue[2];
                m_queued = 2;
            }
            else if (m_primType == 5u) // fan: the first and the last stay
            {
                m_queue[1] = m_queue[2];
                m_queued = 2;
            }
            else // list
                m_queued = 0;
        }
        m_queue[m_queued++] = v;
        if (m_queued == 3 && !adc && m_triangles.size() < kMaxTriangles)
            m_triangles.push_back(Triangle{{m_queue[0], m_queue[1], m_queue[2]}});
    }

    void Tracker::finishFrame()
    {
        if (m_log >= 2 && m_stats.kicks > 0u)
        {
            static uint32_t frames = 0;
            if (frames++ % 60u == 0u)
                std::cerr << "[motion] kicks " << m_stats.kicks << ", main-view batches " << m_stats.mainBatches << ", found in the previous frame "
                          << m_stats.matched << ", vertices " << m_stats.vertices << ", rejected (Q not 1/w) " << m_stats.rejected << std::endl;
        }
        {
            std::lock_guard<std::mutex> lock(m_publishMutex);
            m_published.swap(m_samples);
            m_publishedTriangles.swap(m_triangles);
            m_publishedStats = m_stats;
            m_publishedTick = m_frameTick;
            ++m_publishedSerial;
            Recent &r = m_recent[m_publishedSerial % 3u];
            r.tick = m_frameTick;
            r.triangles = m_publishedTriangles;
            r.stats = m_stats;
        }
        m_samples.clear();
        m_triangles.clear();
        m_queued = 0;
        m_stats = FrameStats{};
        m_previous.swap(m_current);
        m_current.clear();
        m_occurrences.clear();
        m_active = false;
        m_frameTick = ~0ull; // the next kick starts a frame (nothing to finish)
    }

    void Tracker::publishBefore(uint64_t vsyncTick)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_frameTick != ~0ull && m_frameTick < vsyncTick && m_stats.kicks > 0u)
            finishFrame();
    }

    void Tracker::latest(std::vector<Sample> &out, FrameStats &stats)
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        out = m_published;
        stats = m_publishedStats;
    }

    bool Tracker::trianglesOfTick(uint64_t vsyncTick, std::vector<Triangle> &out, FrameStats *stats)
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        for (const Recent &r : m_recent)
            if (r.tick == vsyncTick)
            {
                out = r.triangles;
                if (stats)
                    *stats = r.stats;
                return true;
            }
        return false;
    }

    uint64_t Tracker::latestTriangles(std::vector<Triangle> &out, uint64_t haveSerial, uint64_t *vsyncTick)
    {
        std::lock_guard<std::mutex> lock(m_publishMutex);
        if (m_publishedSerial != haveSerial)
            out = m_publishedTriangles;
        if (vsyncTick)
            *vsyncTick = m_publishedTick;
        return m_publishedSerial;
    }
}
