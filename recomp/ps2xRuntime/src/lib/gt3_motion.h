#pragma once

// GT3 motion data: groundwork for temporal upscalers (FSR 2, DLSS), which need
// each pixel's position in the previous frame.
//
// GT3's VU1 microprogram (code image a15dc42ffe249fd2) transforms every batch
// of vertices with one 4x4 matrix held in vf28..vf31 (loaded from TOP+193 by
// the entry stubs that call 0x0; the others keep the previous batch's) and a
// viewport scale/offset in vf25/vf26:
//   clip = (x, y, z, 1) . M;   screen = clip.xyz / clip.w * vf25 + vf26.
// A batch's vertex data is the same from frame to frame, so a batch is found
// again in the next frame by its routine, its first four vertex qwords and
// the occurrence of that pair in the frame. For each vertex the microprogram
// sends to the GS, the position in the previous frame is then found from the
// output, which works for every preprocessing routine: clip.w = 1/Q (the ST
// register's Q), clip.xy = screen.xy made NDC times w; the local position
// solves those three columns of M (the z column is useless: GT3's projection
// makes clip.z = w - tiny, so GS Z carries almost no depth), and
//   previous = project(local . M_prev).
// See gt3-analysis/FSR2-motion-vectors.md.
//
// Now: a debug view (GT3_MOTION_DEBUG=1, or F6 in game) drawing each vertex's
// motion over the picture, and statistics (GT3_MOTION_DEBUG=2 also logs them).

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

struct VU1State;

namespace gt3motion
{
    // One vertex: where it is now and where it was a frame ago, in frame
    // pixels of the main view (0..640 x 0..448).
    struct Sample
    {
        float x, y, px, py;
    };

    // A main-view vertex as the GS draws it (frame pixels), its 1/w (larger is
    // nearer) and where it was a frame ago (the same place when unknown).
    struct Vertex
    {
        float x, y, q, px, py;
    };
    struct Triangle
    {
        Vertex v[3];
    };

    struct FrameStats
    {
        uint32_t kicks = 0;       // VU1 programs this frame
        uint32_t mainBatches = 0; // of them, main-view batches
        uint32_t matched = 0;     // main-view batches found in the previous frame
        uint32_t vertices = 0;    // main-view vertices with a previous position
        uint32_t rejected = 0;    // vertices whose Q is not 1/w (their Z disagrees): motion 0
        double motionSum = 0.0;   // summed length of those vertices' motion (frame pixels)
    };

    class Tracker
    {
    public:
        static Tracker &instance();

        bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
        void setEnabled(bool on) { m_enabled.store(on, std::memory_order_relaxed); }
        int logLevel() const { return m_log; }

        // VU1 program hooks, in kick order (VIF1 worker or EE thread). A kick
        // resumed with MSCNT continues the batch it belongs to.
        void beginKick(uint64_t imageHash, uint32_t startPC, bool resume, uint32_t top, const uint8_t *vuData, uint64_t vsyncTick);
        // Each XGKICK packet; imageHash is the running code image (only
        // a15dc42f's layout is known).
        void packet(const VU1State &state, uint64_t imageHash, const uint8_t *packet, uint32_t bytes);

        // Present thread: the samples and statistics of the last complete frame.
        void latest(std::vector<Sample> &out, FrameStats &stats);
        // The last complete frame's main-view triangles (copied only when its
        // serial differs from haveSerial); returns that serial.
        uint64_t latestTriangles(std::vector<Triangle> &out, uint64_t haveSerial, uint64_t *vsyncTick = nullptr);
        // The triangles of the frame drawn during that vsync, if among the last
        // three published (a picture shown at vsync V was drawn during V-1).
        bool trianglesOfTick(uint64_t vsyncTick, std::vector<Triangle> &out, FrameStats *stats = nullptr);
        // Present thread, at a vsync: publishes a frame drawn before it at once
        // (otherwise that happens at the next frame's first kick).
        void publishBefore(uint64_t vsyncTick);

    private:
        Tracker();
        struct Batch
        {
            double m[4][4];
            double scale[3], offset[3];
        };
        void finishFrame();
        void startBatch(const VU1State &state);
        void addVertex(const Vertex &v, bool adc);
        static constexpr size_t kMaxTriangles = 400000;
        uint32_t m_primType = 4; // GS PRIM type of the current packet (4: strip)
        Vertex m_queue[3]{};
        int m_queued = 0;
        std::vector<Triangle> m_triangles;

        std::atomic<bool> m_enabled{false};
        std::mutex m_stateMutex; // the kick-side state (VIF1 worker vs publishBefore)
        int m_log = 0;
        bool m_active = false;   // the current kick belongs to image a15dc42f
        bool m_haveBatch = false; // the current batch's matrix is known
        uint64_t m_key = 0;
        uint64_t m_frameTick = ~0ull;
        std::unordered_map<uint64_t, uint32_t> m_occurrences;
        std::unordered_map<uint64_t, Batch> m_current, m_previous;
        // The current batch: M, the inverse of its x/y/w columns (rows 0..2),
        // the previous frame's M (if found).
        Batch m_batch{};
        double m_inverse[3][3]{};
        Batch m_prev{};
        bool m_main = false, m_matched = false;
        std::vector<Sample> m_samples;
        FrameStats m_stats;

        std::mutex m_publishMutex;
        std::vector<Sample> m_published;
        std::vector<Triangle> m_publishedTriangles;
        FrameStats m_publishedStats;
        uint64_t m_publishedTick = 0, m_publishedSerial = 0;
        struct Recent
        {
            uint64_t tick = ~0ull;
            std::vector<Triangle> triangles;
            FrameStats stats;
        };
        Recent m_recent[3];
    };
}
