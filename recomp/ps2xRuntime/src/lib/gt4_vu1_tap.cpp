// The VU1 tap: see gt4_vu1_tap.h and analysis/design/lighting/research/VU1-TAP-RESEARCH.md (sections 2-3, 5).
#include "gt4_vu1_tap.h"

#include "runtime/ps2_vu1.h"
#include "raylib.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace gt4tap
{
    void noteSun(const Record &r); // (below: relighting's sun, from a car body's light set)
    namespace
    {
        // 0 off, 1 on, 2 verify.
        int mode()
        {
#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)
            static const int m = [] {
                const char *v = std::getenv("GT4_VU1_TAP");
                // (DLSS needs the motion: GT4_UPSCALER=dlss turns it on unless GT4_VU1_TAP says otherwise)
                const char *up = std::getenv("GT4_UPSCALER");
                if ((!v || !*v) && up && (std::strcmp(up, "dlss") == 0 || std::strcmp(up, "fsr") == 0 || std::strcmp(up, "fsr4") == 0))
                    return 1;
                // (per-pixel car lighting needs the normals and light sets, not the motion: 6, tap only)
                const char *lit = std::getenv("GT4_CAR_LIGHTING");
                if ((!v || !*v) && lit && (std::strcmp(lit, "pixel") == 0 || std::strcmp(lit, "split") == 0 || std::strcmp(lit, "show") == 0))
                    return 6;
                const char *ao = std::getenv("GT4_AO"); // (ambient occlusion: the 3D's depth fit and projection)
                if ((!v || !*v) && ao && *ao && std::strcmp(ao, "0") != 0 && std::strcmp(ao, "off") != 0)
                    return 6;
                const char *ssr = std::getenv("GT4_SSR"); // (screen-space reflections: the reflection layer's normals)
                if ((!v || !*v) && ssr && *ssr && std::strcmp(ssr, "0") != 0 && std::strcmp(ssr, "off") != 0)
                    return 6;
                const char *rl = std::getenv("GT4_RELIGHT"); // (relighting: the 3D's distances, the cars, GT4's sun)
                if ((!v || !*v) && rl && *rl && std::strcmp(rl, "0") != 0 && std::strcmp(rl, "off") != 0)
                    return 6;
                const char *sh = std::getenv("GT4_SHADOWS"); // (sun shadows: the same)
                if ((!v || !*v) && sh && *sh && std::strcmp(sh, "0") != 0 && std::strcmp(sh, "off") != 0)
                    return 6;
                for (const char *name : {"GT4_SUN_GLOW", "GT4_BLOOM"}) // (sun glow, bloom: the end of the frame's 3D)
                {
                    const char *e = std::getenv(name);
                    if ((!v || !*v) && e && *e && std::strcmp(e, "0") != 0 && std::strcmp(e, "off") != 0)
                        return 6;
                }
                if (!v || !*v || std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0)
                    return 0;
                return std::strcmp(v, "verify") == 0 ? 2 : std::strcmp(v, "normals") == 0 ? 3 : std::strcmp(v, "motion") == 0 ? 4 : std::strcmp(v, "gbuffer") == 0 ? 5 : 1;
            }();
            return m;
#else
            return 0;
#endif
        }

        enum Kind : uint8_t
        {
            KindNone,   // not tapped (standalone programs, unknown entries)
            KindDirect, // a direct entry: no module, float positions (track and scenery)
            KindModule, // an ITOP module (see module / conversion)
        };
        enum Conv : uint8_t
        {
            ConvFloat, // no conversion: float input
            ConvA,     // cvtA 0x1908: positions itof0, @64 normal / 127
            ConvB,     // cvtB 0x1878: positions itof0, @64 ST / 4096, @128 normal / 127
            ConvC,     // cvtC 0x1810: positions itof0, @64 ST / 4096, @128 colour
            ConvMorph, // morphA / morphB: never seen, not decoded
        };

        constexpr uint32_t kBodies[15] = {0x00b0, 0x0118, 0x0290, 0x0418, 0x05b0, 0x0788, 0x0960, 0x0b48,
                                          0x0ca8, 0x0ea0, 0x10a8, 0x12c8, 0x1508, 0x1608, 0x16f8};

        // The lit modules re-lit per pixel (lighting step 2): M07, and M01-M03 (M07 with powers on lights 3 and 4).
        bool litModule(int m) { return m == 7 || (m >= 1 && m <= 3); }

        struct Program
        {
            bool active = false;
            Kind kind = KindNone;
            int module = -1; // 0..14 (M00..M14)
            Conv conv = ConvFloat;
            bool load = false; // the load entry of a group (matrix from TOP+193..)
            uint32_t top = 0, nloop = 0;
            float m[4][4] = {};   // local->clip rows (clip = x*m0 + y*m1 + z*m2 + m3)
            float vs[4] = {}, vo[4] = {}; // viewport scale / offset (vf25 / vf26)
            float pos[64][3] = {};
            float nrm[64][3] = {};
            // Lighting step 2: M07's light set (TOP+201..209: three rows of 4 light directions in local space, one light
            // per lane; the ambient colour; 4 light colours; the clamp), read with the matrix.
            float light[3][4] = {}, ambient[4] = {}, lightColour[4][4] = {}, lightClamp[4] = {};
            float lightPow = 1.0f; // M01 / M02 / M03: lights 3 and 4 to the 4th / 8th / 16th power (listing 0x0170-0x0180)
            // Lighting step 2c: M10's eye (TOP+204), rotation (201-203), colour factor (205, 206), input colour @128.
            float eye[4] = {}, envRot[3][4] = {}, fresnel[2][4] = {};
            uint8_t inColour[64][4] = {};
            bool hasNormal = false;
            const uint8_t *vuData = nullptr;
            bool hasPrev = false;
            float prevM[4][4] = {};
            int clipSlot = -1; // this program's clipper record (one for all its clipper packets)
            int recordSlot = -1; // this program's record (its kernel packet's)
            bool haveSolve = false;
            double cond = 0.0;
            double solve[3][3] = {};
        };
        Program g_p;

        // Back to local space from clip x, y and w: [local] * A + t = (cx, cy, w), A = the matrix's rows 0-2 in its
        // columns x, y and w, t = row 3 in them; solve = A's inverse (false: singular).
        // cond: |det| over the product of A's row lengths (1: orthogonal rows, 0: singular; flattened shadows).
        bool solveFor(const float m[4][4], double out[3][3], double &cond)
        {
            const int cols[3] = {0, 1, 3};
            double a[3][3];
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    a[i][j] = m[i][cols[j]];
            const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                               a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
            double norms = 1.0;
            for (int i = 0; i < 3; ++i)
                norms *= std::sqrt(a[i][0] * a[i][0] + a[i][1] * a[i][1] + a[i][2] * a[i][2]);
            cond = norms > 0.0 ? std::fabs(det) / norms : 0.0;
            if (!(std::fabs(det) > 1e-30))
                return false;
            const double k = 1.0 / det;
            out[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * k;
            out[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * k;
            out[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * k;
            out[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * k;
            out[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * k;
            out[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * k;
            out[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * k;
            out[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * k;
            out[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * k;
            return true;
        }

        // A screen position (GS pixels) and its Q back to local space: w = 1 / q, cx = (x - vo) / vs * w (likewise
        // y), local = ((cx, cy, w) - t) * A^-1 (row vectors: local_i = sum_j r_j * solve[j][i]).
        bool toLocal(const double solve[3][3], const float m[4][4], const float vs[4], const float vo[4], double x, double y, double q, double out[3])
        {
            if (!(q > 0.0) || vs[0] == 0.0f || vs[1] == 0.0f)
                return false;
            const double w = 1.0 / q;
            const double r[3] = {(x - vo[0]) / vs[0] * w - m[3][0], (y - vo[1]) / vs[1] * w - m[3][1], w - m[3][3]};
            for (int i = 0; i < 3; ++i)
                out[i] = r[0] * solve[0][i] + r[1] * solve[1][i] + r[2] * solve[2][i];
            return true;
        }

        // Clipper records (marks 0x8000 | slot + 1): the program's matrices only.
        struct ClipRecord
        {
            uint64_t frame = 0;
            bool hasPrev = false;      // last frame's matrix known (prevM): the motion; else lighting only
            uint32_t recordSlot = ~0u; // the program's own record (its kernel packet's), once mapped
            double solve[3][3];
            float m[4][4], prevM[4][4];
            float vs[4], vo[4];
        };
        constexpr uint32_t kClipRing = 32767u;
        ClipRecord *g_clipRing = nullptr;
        uint32_t g_clipNext = 0u;

        // Matrix groups (a load entry and the keep entries after it share one matrix), per frame, by key (entry,
        // ITOP, vertex count, the first 4 input qwords): last frame's groups give each group its previous matrix
        // (the candidate with the nearest translation row; keys repeat for identical tyres and road pieces). A new frame
        // starts at the first program after a new VSync that follows a pause of 0.5 ms or more: GT4 sends a frame's
        // VU1 work as one DMA chain, run back to back, and that chain can straddle a VSync (the VSync alone split
        // frames in two, the first key coming again did not work: the first groups change from frame to frame).
        struct Mat
        {
            float m[4][4];
        };
        // Per key: the matrices of its groups in the latest split it was seen in, and in the split before that. A group
        // takes its candidates from the latest split before the current one, so a frame that a pause split in two (its
        // second half after a GS read-back, say) still finds each group's last drawing.
        struct History
        {
            uint64_t latestSplit = 0, olderSplit = 0;
            std::vector<Mat> latest, older;
        };
        std::unordered_map<uint64_t, History> g_hist;
        uint64_t g_split = 1;
        uint64_t g_frameVsync = ~0ull, g_firstKey = 0ull, g_frameLoads = 0ull;
        bool g_newFrame = false;
        std::chrono::steady_clock::time_point g_lastProgram{};
        bool g_groupHasPrev = false;
        Mat g_groupPrev{};
        uint64_t fnv(const void *d, size_t n, uint64_t h = 1469598103934665603ull)
        {
            const uint8_t *b = static_cast<const uint8_t *>(d);
            for (size_t i = 0; i < n; ++i)
                h = (h ^ b[i]) * 1099511628211ull;
            return h;
        }

        // The records the renderer reads (by slot, from the packets' GIF tags): a ring of the programs whose vertex
        // packet was mapped. 16,384 programs is ~10 frames of GT4's (the GS runs at most 6 frames behind).
        constexpr uint32_t kRing = 32767u; // (marks 1..0x7FFF; GT4's GS can run ~7 frames behind its VU1)
        Record *g_ring = nullptr;
        uint32_t g_next = 0u;
        uint64_t g_serial = 0u;

        struct Stats
        {
            uint64_t programs = 0, tapped = 0, normals = 0, packets = 0, mapped = 0, vertices = 0, checked = 0, matched = 0, worst = 0;
            uint64_t frames = 0, loads = 0, loadsWithPrev = 0, lastFrameLoads = 0;
            uint64_t perModule[15] = {}, direct = 0, morph = 0, unknown = 0, normalChecked = 0, normalOff = 0;
            uint64_t litChecked[16] = {}, litMatched[16] = {}, litWorst[16] = {};
            uint64_t envChecked = 0, envUvOff = 0, envColourMatched = 0, envColourWorst = 0, envColourOut = 0;
            double envUvWorst = 0.0;
            uint64_t clipPackets = 0, clipRecords = 0, unprojChecked = 0, unprojOff = 0, qChecked = 0, qOff = 0;
            // (verify: checked / off by cond decade, 1e-0 .. under 1e-6, and by module, direct = 15)
            uint64_t condChecked[7] = {}, condOff[7] = {}, modChecked[16] = {}, modOff[16] = {};
            double unprojWorst = 0.0;
        };
        Stats g_s;
        uint64_t g_lastLogVsync = 0;

        float f32(const uint8_t *d, uint32_t qword, int lane)
        {
            float f;
            std::memcpy(&f, d + (static_cast<size_t>(qword & 1023u) * 16u + static_cast<size_t>(lane) * 4u), 4);
            return f;
        }
        int32_t i32(const uint8_t *d, uint32_t qword, int lane)
        {
            int32_t v;
            std::memcpy(&v, d + (static_cast<size_t>(qword & 1023u) * 16u + static_cast<size_t>(lane) * 4u), 4);
            return v;
        }

        // Entry pc -> (kind, load). Groups at 0x1b08 + 0x28 * g: g 0-11 direct, 12-23 through ITOP; +0 load, +8 keep.
        bool classifyEntry(uint32_t pc, Kind &kind, bool &load)
        {
            if (pc < 0x1b08u || pc >= 0x1b08u + 24u * 0x28u)
                return false;
            const uint32_t off = (pc - 0x1b08u) % 0x28u, g = (pc - 0x1b08u) / 0x28u;
            if (off != 0u && off != 8u)
                return false;
            load = off == 0u;
            kind = g < 12u ? KindDirect : KindModule;
            return true;
        }

        // ITOP (in 8-byte units) -> module and conversion: the body itself (float input) or one of its 5 stubs at
        // body - 0x50 (16 bytes each): cvtA, cvtB, cvtC, morphA, morphB.
        bool classifyModule(uint32_t itop, int &module, Conv &conv)
        {
            const uint32_t pc = (itop & 0x7ffu) * 8u;
            for (int i = 0; i < 15; ++i)
            {
                if (pc == kBodies[i])
                {
                    module = i;
                    conv = ConvFloat;
                    return true;
                }
                if (kBodies[i] >= 0x50u && pc >= kBodies[i] - 0x50u && pc < kBodies[i])
                {
                    module = i;
                    const uint32_t k = (pc - (kBodies[i] - 0x50u)) / 16u; // (each stub: b <conversion> + its delay slot)
                    conv = k == 0 ? ConvA : k == 1 ? ConvB : k == 2 ? ConvC : ConvMorph;
                    return true;
                }
            }
            return false;
        }

        void logStats(uint64_t vsync)
        {
            if (g_lastLogVsync && vsync < g_lastLogVsync + 600u)
                return;
            g_lastLogVsync = vsync;
            std::fprintf(stderr,
                         "[gt4-tap] vsync %llu: programs %llu, tapped %llu (normals %llu; direct %llu, morph skipped %llu, unknown %llu); packets %llu, "
                         "mapped %llu (%llu vertices)",
                         static_cast<unsigned long long>(vsync), static_cast<unsigned long long>(g_s.programs), static_cast<unsigned long long>(g_s.tapped),
                         static_cast<unsigned long long>(g_s.normals), static_cast<unsigned long long>(g_s.direct),
                         static_cast<unsigned long long>(g_s.morph), static_cast<unsigned long long>(g_s.unknown),
                         static_cast<unsigned long long>(g_s.packets), static_cast<unsigned long long>(g_s.mapped),
                         static_cast<unsigned long long>(g_s.vertices));
            if (mode() == 2)
                std::fprintf(stderr, "; verify: %llu on-screen vertices, %llu within 1/16 px, worst %.2f px",
                             static_cast<unsigned long long>(g_s.checked), static_cast<unsigned long long>(g_s.matched),
                             static_cast<double>(g_s.worst) / 16.0);
            if (mode() == 2)
                std::fprintf(stderr, "; normals %llu, %llu not of unit length (+-5%%)", static_cast<unsigned long long>(g_s.normalChecked),
                             static_cast<unsigned long long>(g_s.normalOff));
            if (mode() == 2)
                for (int m : {1, 2, 3, 7})
                    std::fprintf(stderr, "\n[gt4-tap]   lighting step 2: M%02d colours re-lit from the tapped normals and lights: %llu vertices, %llu "
                                         "within 1 of GT4's (worst %llu)",
                                 m, static_cast<unsigned long long>(g_s.litChecked[m]), static_cast<unsigned long long>(g_s.litMatched[m]),
                                 static_cast<unsigned long long>(g_s.litWorst[m]));
            if (mode() == 2)
                std::fprintf(stderr, "\n[gt4-tap]   lighting step 2c: M10 reflections re-made from the tapped data: %llu vertices, UV off over 1/8 "
                                     "texel (256) %llu (worst %.4f texels); colour within 1 %llu (worst %llu); input colours out of 0..255 %llu",
                             static_cast<unsigned long long>(g_s.envChecked), static_cast<unsigned long long>(g_s.envUvOff), g_s.envUvWorst,
                             static_cast<unsigned long long>(g_s.envColourMatched), static_cast<unsigned long long>(g_s.envColourWorst),
                             static_cast<unsigned long long>(g_s.envColourOut));
            std::fprintf(stderr, "\n[gt4-tap]   frames %llu (last %llu groups); groups %llu, with last frame's matrix %llu",
                         static_cast<unsigned long long>(g_s.frames), static_cast<unsigned long long>(g_s.lastFrameLoads),
                         static_cast<unsigned long long>(g_s.loads), static_cast<unsigned long long>(g_s.loadsWithPrev));
            std::fprintf(stderr, "\n[gt4-tap]   clipper packets %llu (records %llu); unprojected %llu, off %llu (worst %.4f); Q = 1/w %llu of %llu",
                         static_cast<unsigned long long>(g_s.clipPackets), static_cast<unsigned long long>(g_s.clipRecords),
                         static_cast<unsigned long long>(g_s.unprojChecked), static_cast<unsigned long long>(g_s.unprojOff), g_s.unprojWorst,
                         static_cast<unsigned long long>(g_s.qChecked - g_s.qOff), static_cast<unsigned long long>(g_s.qChecked));
            std::fprintf(stderr, "\n[gt4-tap]   unprojection off / checked by cond decade:");
            for (int i = 0; i < 7; ++i)
                std::fprintf(stderr, " %llu/%llu", static_cast<unsigned long long>(g_s.condOff[i]), static_cast<unsigned long long>(g_s.condChecked[i]));
            std::fprintf(stderr, "; by module M00..M14, direct:");
            for (int i = 0; i < 16; ++i)
                std::fprintf(stderr, " %llu/%llu", static_cast<unsigned long long>(g_s.modOff[i]), static_cast<unsigned long long>(g_s.modChecked[i]));
            std::fprintf(stderr, "\n[gt4-tap]   modules M00..M14:");
            for (int i = 0; i < 15; ++i)
                std::fprintf(stderr, " %llu", static_cast<unsigned long long>(g_s.perModule[i]));
            std::fprintf(stderr, "\n");
        }
    } // namespace

    bool enabled() { return mode() != 0; }

    void viewNormal(const Record &r, const float n[3], float out[3])
    {
        const int cols[3] = {0, 1, 3};
        float v[3];
        for (int a = 0; a < 3; ++a)
        {
            const int c = cols[a];
            const float len = std::sqrt(r.m[0][c] * r.m[0][c] + r.m[1][c] * r.m[1][c] + r.m[2][c] * r.m[2][c]);
            const float k = len > 0.0f ? (a == 2 ? -1.0f : 1.0f) / len : 0.0f;
            v[a] = (r.m[0][c] * n[0] + r.m[1][c] * n[1] + r.m[2][c] * n[2]) * k;
        }
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        for (int a = 0; a < 3; ++a)
            out[a] = len > 0.0f ? v[a] / len : 0.0f;
    }

    bool zMapping(uint16_t mark, double &A, double &B)
    {
        const float(*m)[4] = nullptr;
        const float *vs = nullptr, *vo = nullptr;
        if (!mark)
            return false;
        if (mark & kClipMark)
        {
            const uint32_t slot = (mark & 0x7FFFu) - 1u;
            if (!g_clipRing || slot >= kClipRing)
                return false;
            m = g_clipRing[slot].m;
            vs = g_clipRing[slot].vs;
            vo = g_clipRing[slot].vo;
        }
        else
        {
            const Record *r = record(mark - 1u);
            if (!r)
                return false;
            m = r->m;
            vs = r->vs;
            vo = r->vo;
        }
        // clip z = k clip w + c (rigid model-view, a perspective without z in w's constant): Z = vs.z (k + c Q) + vo.z
        const double ww = static_cast<double>(m[0][3]) * m[0][3] + static_cast<double>(m[1][3]) * m[1][3] + static_cast<double>(m[2][3]) * m[2][3];
        if (!(ww > 0.0))
            return false;
        const double k = (static_cast<double>(m[0][2]) * m[0][3] + static_cast<double>(m[1][2]) * m[1][3] + static_cast<double>(m[2][2]) * m[2][3]) / ww;
        A = (vs[2] * k + vo[2]) / 16777216.0;
        B = vs[2] * (m[3][2] - k * m[3][3]) / 16777216.0;
        return true;
    }

    bool corner(uint16_t mark, uint32_t k, float x, float y, float q, Corner &c)
    {
        if (!mark)
            return false;
        if (!(mark & kClipMark))
        {
            const Record *r = record(mark - 1u);
            if (!r || k >= r->count || !r->hasNormal)
                return false;
            c.rec = r;
            for (int a = 0; a < 3; ++a)
            {
                c.pos[a] = r->pos[k][a];
                c.nrm[a] = r->nrm[k][a];
            }
            for (int l = 0; l < 4; ++l)
                c.colour[l] = r->env ? static_cast<float>(r->inColour[k][l]) : 0.0f;
            return true;
        }
        const uint32_t slot = (mark & 0x7FFFu) - 1u;
        if (!g_clipRing || slot >= kClipRing)
            return false;
        const ClipRecord &cr = g_clipRing[slot];
        const Record *r = cr.recordSlot != ~0u ? record(cr.recordSlot) : nullptr;
        if (!r || !r->hasNormal || r->frame != cr.frame || r->count < 3u)
            return false;
        double l[3];
        if (!toLocal(cr.solve, cr.m, cr.vs, cr.vo, x, y, q, l))
            return false;
        // the strip's triangle (i-2, i-1, i) it lies on: the nearest to its plane, inside it
        double bestScore = 1e30, bu = 0.0, bv = 0.0, bw = 0.0;
        uint32_t best = 0;
        for (uint32_t i = 2; i < r->count; ++i)
        {
            const float *A = r->pos[i - 2], *B = r->pos[i - 1], *C = r->pos[i];
            const double e1[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, e2[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
            const double vp[3] = {l[0] - A[0], l[1] - A[1], l[2] - A[2]};
            const double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const double area = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (!(area > 1e-9))
                continue;
            const double d00 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2], d01 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
            const double d11 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
            const double d20 = vp[0] * e1[0] + vp[1] * e1[1] + vp[2] * e1[2], d21 = vp[0] * e2[0] + vp[1] * e2[1] + vp[2] * e2[2];
            const double den = d00 * d11 - d01 * d01;
            if (!(std::fabs(den) > 1e-18))
                continue;
            const double v = (d11 * d20 - d01 * d21) / den, w = (d00 * d21 - d01 * d20) / den, u = 1.0 - v - w;
            const double plane = std::fabs(vp[0] * n[0] + vp[1] * n[1] + vp[2] * n[2]) / area / std::sqrt(area);
            const double outside = std::max(0.0, -u) + std::max(0.0, -v) + std::max(0.0, -w);
            const double score = plane + outside * 10.0;
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
                bu = std::max(0.0, u);
                bv = std::max(0.0, v);
                bw = std::max(0.0, w);
            }
        }
        const double sum = bu + bv + bw;
        if (best < 2u || !(sum > 0.0) || bestScore > 0.05) // (on the plane and inside, to a twentieth of the edge)
            return false;
        bu /= sum;
        bv /= sum;
        bw /= sum;
        c.rec = r;
        for (int a = 0; a < 3; ++a)
        {
            c.pos[a] = static_cast<float>(l[a]);
            c.nrm[a] = static_cast<float>(bu * r->nrm[best - 2][a] + bv * r->nrm[best - 1][a] + bw * r->nrm[best][a]);
        }
        for (int i = 0; i < 4; ++i)
            c.colour[i] = r->env ? static_cast<float>(bu * r->inColour[best - 2][i] + bv * r->inColour[best - 1][i] + bw * r->inColour[best][i]) : 0.0f;
        return true;
    }

    void envVertex(const Record &r, uint32_t k, float &u, float &v, float colour[4])
    {
        float V[3], R[3], rv[3];
        for (int a = 0; a < 3; ++a)
            V[a] = r.pos[k][a] - r.eye[a];
        const float d = V[0] * r.nrm[k][0] + V[1] * r.nrm[k][1] + V[2] * r.nrm[k][2];
        const float len2 = V[0] * V[0] + V[1] * V[1] + V[2] * V[2];
        const float inv = len2 > 0.0f ? 1.0f / std::sqrt(len2) : 0.0f;
        for (int a = 0; a < 3; ++a)
            R[a] = V[a] - 2.0f * d * r.nrm[k][a];
        for (int a = 0; a < 3; ++a)
            rv[a] = (r.envRot[0][a] * R[0] + r.envRot[1][a] * R[1] + r.envRot[2][a] * R[2]) * inv;
        rv[2] += 1.0f;
        const float m2 = rv[0] * rv[0] + rv[1] * rv[1] + rv[2] * rv[2];
        const float q = m2 > 0.0f ? 0.5f / std::sqrt(m2) : 0.0f;
        u = 0.5f + rv[0] * q;
        v = 0.5f - rv[1] * q;
        const float f = std::min(d, 0.0f) * inv;
        for (int l = 0; l < 4; ++l)
            colour[l] = static_cast<float>(r.inColour[k][l]) * (r.fresnel[0][l] + r.fresnel[1][l] * f);
    }
    bool motion() { return mode() != 0 && mode() != 6; }

    void beginProgram(const VU1State &vu, uint32_t startPc, uint32_t top, uint32_t itop, const uint8_t *vuData, uint64_t vsync)
    {
        Program &p = g_p;
        p.active = false;
        ++g_s.programs;
        logStats(vsync);
        Kind kind = KindNone;
        bool load = false;
        if (!classifyEntry(startPc, kind, load))
        {
            ++g_s.unknown;
            return;
        }
        int module = -1;
        Conv conv = ConvFloat;
        if (kind == KindModule && !classifyModule(itop, module, conv))
        {
            ++g_s.unknown;
            return;
        }
        if (conv == ConvMorph)
        {
            ++g_s.morph;
            return;
        }
        p.kind = kind;
        p.module = module;
        p.conv = conv;
        p.load = load;
        p.top = top & 1023u;
        p.vuData = vuData;
        p.nloop = static_cast<uint32_t>(i32(vuData, p.top + 192u, 0)) & 0x7fffu;
        if (p.nloop == 0u || p.nloop > 64u)
        {
            ++g_s.unknown;
            return;
        }
        // The matrix and viewport: a load entry reads them from TOP+193..198 (into vf28..31, vf25 / vf26), a keep
        // entry keeps the registers of the program before.
        for (int r = 0; r < 4; ++r)
            for (int l = 0; l < 4; ++l)
                p.m[r][l] = load ? f32(vuData, p.top + 193u + r, l) : vu.vf[28 + r][l];
        for (int l = 0; l < 4; ++l)
        {
            p.vs[l] = load ? f32(vuData, p.top + 197u, l) : vu.vf[25][l];
            p.vo[l] = load ? f32(vuData, p.top + 198u, l) : vu.vf[26][l];
        }
        {
            const auto now = std::chrono::steady_clock::now();
            const bool paused = now - g_lastProgram > std::chrono::microseconds(500);
            g_lastProgram = now;
            if (g_frameVsync == ~0ull || (vsync != g_frameVsync && paused) || vsync > g_frameVsync + 3u)
            {
                g_newFrame = true;
                g_frameVsync = vsync;
            }
        }
        if (load)
        {
            const uint32_t keyHead[3] = {startPc, itop, p.nloop};
            const uint64_t key = fnv(vuData + static_cast<size_t>(p.top) * 16u, 64u, fnv(keyHead, sizeof(keyHead)));
            (void)g_firstKey;
            if (g_newFrame)
            {
                g_newFrame = false;
                ++g_split;
                g_firstKey = key;
                g_s.lastFrameLoads = g_frameLoads;
                ++g_s.frames;
                g_frameLoads = 0u;
                // Keys not seen for 600 splits (~10 s) go.
                if ((g_split % 600u) == 0u)
                    for (auto it = g_hist.begin(); it != g_hist.end();)
                        it = it->second.latestSplit + 600u < g_split ? g_hist.erase(it) : std::next(it);
            }
            ++g_frameLoads;
            Mat now;
            std::memcpy(now.m, p.m, sizeof(now.m));
            History &h = g_hist[key];
            const std::vector<Mat> *cands = h.latestSplit < g_split ? &h.latest : (h.olderSplit ? &h.older : nullptr);
            g_groupHasPrev = false;
            if (cands && !cands->empty())
            {
                float best = 0.0f;
                for (const Mat &c : *cands)
                {
                    const float dx = c.m[3][0] - now.m[3][0], dy = c.m[3][1] - now.m[3][1], dz = c.m[3][2] - now.m[3][2], dw = c.m[3][3] - now.m[3][3];
                    const float d = dx * dx + dy * dy + dz * dz + dw * dw;
                    if (!g_groupHasPrev || d < best)
                    {
                        best = d;
                        g_groupPrev = c;
                        g_groupHasPrev = true;
                    }
                }
            }
            if (h.latestSplit != g_split)
            {
                h.older.swap(h.latest);
                h.olderSplit = h.latestSplit;
                h.latest.clear();
                h.latestSplit = g_split;
            }
            h.latest.push_back(now);
        }
        if (load)
        {
            ++g_s.loads;
            g_s.loadsWithPrev += g_groupHasPrev ? 1u : 0u;
            // (verify: which groups found no match, by entry and module; and whether the same entry / ITOP / count
            // existed last frame at all)
            if (mode() == 2 && !g_groupHasPrev)
            {
                static std::unordered_map<uint64_t, uint64_t> missed;
                const uint64_t k = (static_cast<uint64_t>(startPc) << 32) | (static_cast<uint64_t>(itop & 0xffffu) << 8) | (p.nloop & 0xffu);
                if (++missed[k] == 2000u)
                    std::fprintf(stderr, "[gt4-tap] unmatched group x2000: entry %04x itop %04x (module %d) %u vertices, first qword %08x %08x %08x %08x\n",
                                 startPc, itop * 8u, module, p.nloop, static_cast<uint32_t>(i32(vuData, p.top, 0)), static_cast<uint32_t>(i32(vuData, p.top, 1)),
                                 static_cast<uint32_t>(i32(vuData, p.top, 2)), static_cast<uint32_t>(i32(vuData, p.top, 3)));
            }
        }
        p.hasPrev = g_groupHasPrev;
        std::memcpy(p.prevM, g_groupPrev.m, sizeof(p.prevM));
        p.clipSlot = -1;
        p.recordSlot = -1;
        // (not for a near-singular matrix: GT4's flattened shadows, M12, cond under 1e-6, where the way back is off by
        // up to 10^6 px; checked with verify: at cond 0.1 and above 74,311 of 187 million vertices over 1/4 px)
        p.haveSolve = solveFor(p.m, p.solve, p.cond) && p.cond >= 1e-3;
        const bool intPos = conv == ConvA || conv == ConvB || conv == ConvC;
        // Normals: lit modules M01-M09 @128, the env-map modules M10 / M11 @64.
        const bool lit = module >= 1 && module <= 9, env = module == 10 || module == 11;
        p.hasNormal = (lit && (conv == ConvB || conv == ConvFloat)) || (env && (conv == ConvA || conv == ConvFloat));
        const uint32_t nslot = env ? 64u : 128u;
        for (uint32_t i = 0; i < p.nloop; ++i)
        {
            for (int l = 0; l < 3; ++l)
            {
                p.pos[i][l] = intPos ? static_cast<float>(i32(vuData, p.top + i, l)) : f32(vuData, p.top + i, l);
                if (p.hasNormal)
                    p.nrm[i][l] = conv == ConvFloat ? f32(vuData, p.top + nslot + i, l)
                                                    : static_cast<float>(i32(vuData, p.top + nslot + i, l)) * (1.0f / 127.0f);
            }
        }
        if (module == 10)
        {
            for (int l = 0; l < 4; ++l)
            {
                p.eye[l] = f32(vuData, p.top + 204u, l);
                for (int r = 0; r < 3; ++r)
                    p.envRot[r][l] = f32(vuData, p.top + 201u + r, l);
                p.fresnel[0][l] = f32(vuData, p.top + 205u, l);
                p.fresnel[1][l] = f32(vuData, p.top + 206u, l);
            }
            for (uint32_t i = 0; i < p.nloop; ++i)
                for (int l = 0; l < 4; ++l)
                {
                    const int32_t c = i32(vuData, p.top + 128u + i, l);
                    if ((c < 0 || c > 255) && mode() == 2)
                        ++g_s.envColourOut;
                    p.inColour[i][l] = static_cast<uint8_t>(std::clamp(c, 0, 255));
                }
        }
        if (litModule(module))
        {
            p.lightPow = module == 1 ? 4.0f : module == 2 ? 8.0f : module == 3 ? 16.0f : 1.0f;
            for (int r = 0; r < 3; ++r)
                for (int l = 0; l < 4; ++l)
                    p.light[r][l] = f32(vuData, p.top + 201u + r, l);
            for (int l = 0; l < 4; ++l)
            {
                p.ambient[l] = f32(vuData, p.top + 204u, l);
                p.lightClamp[l] = f32(vuData, p.top + 209u, l);
                for (int c = 0; c < 4; ++c)
                    p.lightColour[c][l] = f32(vuData, p.top + 205u + c, l);
            }
        }
        if (mode() == 2 && p.hasNormal)
            for (uint32_t i = 0; i < p.nloop; ++i)
            {
                const float len = std::sqrt(p.nrm[i][0] * p.nrm[i][0] + p.nrm[i][1] * p.nrm[i][1] + p.nrm[i][2] * p.nrm[i][2]);
                ++g_s.normalChecked;
                if (std::fabs(len - 1.0f) > 0.05f)
                    ++g_s.normalOff;
            }
        p.active = true;
        ++g_s.tapped;
        if (p.hasNormal)
            ++g_s.normals;
        if (kind == KindDirect)
            ++g_s.direct;
        else
            ++g_s.perModule[module];
    }

    void packet(const uint8_t *data, uint32_t size)
    {
        Program &p = g_p;
        if (!p.active)
            return;
        ++g_s.packets;
        // The kernel's vertex packet is built at TOP+242 (GIF tag) and kicked from there: the same bytes. The register
        // packet (TOP+217) and the clipper's packets (workspace 870+) do not match and get no mapping.
        const uint32_t bytes = 16u + p.nloop * 48u;
        if (size != bytes || std::memcmp(data, p.vuData + static_cast<size_t>((p.top + 242u) & 1023u) * 16u, bytes) != 0)
        {
            // Not the kernel's packet: the clipper's (or the register packet, which has no vertices). With history
            // and an invertible matrix, one clipper record per program, its slot in each such packet's tag.
            if (!p.haveSolve || size < 32u)
                return;
            ++g_s.clipPackets;
            if (p.clipSlot < 0)
            {
                if (!g_clipRing)
                    g_clipRing = new ClipRecord[kClipRing];
                const uint32_t slot = g_clipNext;
                g_clipNext = (g_clipNext + 1u) % kClipRing;
                ClipRecord &r = g_clipRing[slot];
                r.frame = g_split;
                r.hasPrev = p.hasPrev;
                r.recordSlot = p.recordSlot >= 0 ? static_cast<uint32_t>(p.recordSlot) : ~0u;
                std::memcpy(r.solve, p.solve, sizeof(r.solve));
                std::memcpy(r.m, p.m, sizeof(r.m));
                std::memcpy(r.prevM, p.prevM, sizeof(r.prevM));
                std::memcpy(r.vs, p.vs, sizeof(r.vs));
                std::memcpy(r.vo, p.vo, sizeof(r.vo));
                p.clipSlot = static_cast<int>(slot);
                ++g_s.clipRecords;
            }
            const uint16_t mark = static_cast<uint16_t>(kClipMark | (p.clipSlot + 1));
            std::memcpy(const_cast<uint8_t *>(data) + 2, &mark, 2);
            return;
        }
        ++g_s.mapped;
        g_s.vertices += p.nloop;
        // The record for the renderer, and its slot in the packet's GIF tag bits 16-31 (no meaning to the GS: the
        // frontend reads them only with the tap on). The packet is the kick's own copy, not VU memory.
        if (!g_ring)
            g_ring = new Record[kRing];
        const uint32_t slot = g_next;
        g_next = (g_next + 1u) % kRing;
        Record &r = g_ring[slot];
        p.recordSlot = static_cast<int>(slot);
        if (p.clipSlot >= 0 && g_clipRing)
            g_clipRing[p.clipSlot].recordSlot = slot; // (its clipper packets came first)
        r.serial = ++g_serial;
        r.frame = g_split;
        r.count = p.nloop;
        r.lit = litModule(p.module) && p.hasNormal;
        if (r.lit)
        {
            r.lightPow = p.lightPow;
            std::memcpy(r.light, p.light, sizeof(r.light));
            std::memcpy(r.ambient, p.ambient, sizeof(r.ambient));
            std::memcpy(r.lightColour, p.lightColour, sizeof(r.lightColour));
            std::memcpy(r.lightClamp, p.lightClamp, sizeof(r.lightClamp));
        }
        r.env = p.module == 10 && p.hasNormal;
        if (r.env)
        {
            std::memcpy(r.eye, p.eye, sizeof(r.eye));
            std::memcpy(r.envRot, p.envRot, sizeof(r.envRot));
            std::memcpy(r.fresnel, p.fresnel, sizeof(r.fresnel));
            std::memcpy(r.inColour, p.inColour, sizeof(r.inColour[0]) * p.nloop);
        }
        r.hasNormal = p.hasNormal;
        r.module = static_cast<int8_t>(p.module);
        std::memcpy(r.m, p.m, sizeof(r.m));
        {
            // (tests, GT4_TAP_LIGHTLOG=<n>[,<from vsync>]: a lit program's light set every 600th one, n times: each light's local direction,
            // its view-space direction (viewNormal), its colour; the ambient; the local up (0, 1, 0) in view space. Relighting:
            // which light is the sun.)
            static const int lightLog = [] {
                const char *v = std::getenv("GT4_TAP_LIGHTLOG");
                return v && *v ? std::atoi(v) : 0;
            }();
            static const uint64_t lightFrom = [] {
                const char *v = std::getenv("GT4_TAP_LIGHTLOG");
                const char *c = v ? std::strchr(v, ',') : nullptr;
                return c ? std::strtoull(c + 1, nullptr, 10) : 0ull;
            }();
            static int logged = 0;
            static uint64_t seen = 0;
            if (r.lit && logged < lightLog && g_frameVsync != ~0ull && g_frameVsync >= lightFrom && (seen++ % 600u) == 0u)
            {
                ++logged;
                std::fprintf(stderr, "[gt4-tap] light set (M%02d, vsync %llu): ambient %.3f %.3f %.3f", p.module, static_cast<unsigned long long>(g_frameVsync),
                             r.ambient[0], r.ambient[1], r.ambient[2]);
                for (int l = 0; l < 4; ++l)
                {
                    const float d[3] = {r.light[0][l], r.light[1][l], r.light[2][l]};
                    float v[3];
                    viewNormal(r, d, v);
                    std::fprintf(stderr, " | L%d local %.3f %.3f %.3f view %.3f %.3f %.3f colour %.3f %.3f %.3f", l, d[0], d[1], d[2], v[0], v[1], v[2],
                                 r.lightColour[0][l], r.lightColour[1][l], r.lightColour[2][l]);
                }
                const float up[3] = {0.0f, 1.0f, 0.0f};
                float uv[3];
                viewNormal(r, up, uv);
                std::fprintf(stderr, " | up view %.3f %.3f %.3f\n", uv[0], uv[1], uv[2]);
            }
        }
        std::memcpy(r.vs, p.vs, sizeof(r.vs));
        std::memcpy(r.vo, p.vo, sizeof(r.vo));
        std::memcpy(r.pos, p.pos, sizeof(float) * 3u * p.nloop);
        if (r.lit && p.module == 7)
            noteSun(r);
        if (p.hasNormal)
        {
            std::memcpy(r.nrm, p.nrm, sizeof(float) * 3u * p.nloop);
            // View-space normals: rows = the 3x3 part's x, y and -w columns, normalised.
            float R[3][3];
            const int cols[3] = {0, 1, 3};
            for (int a = 0; a < 3; ++a)
            {
                const int c = cols[a];
                const float len = std::sqrt(p.m[0][c] * p.m[0][c] + p.m[1][c] * p.m[1][c] + p.m[2][c] * p.m[2][c]);
                const float k = len > 0.0f ? (a == 2 ? -1.0f : 1.0f) / len : 0.0f;
                for (int i = 0; i < 3; ++i)
                    R[a][i] = p.m[i][c] * k;
            }
            for (uint32_t v = 0; v < p.nloop; ++v)
            {
                float n[3];
                for (int a = 0; a < 3; ++a)
                    n[a] = R[a][0] * p.nrm[v][0] + R[a][1] * p.nrm[v][1] + R[a][2] * p.nrm[v][2];
                const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int a = 0; a < 3; ++a)
                    r.viewN[v][a] = len > 0.0f ? n[a] / len : 0.0f;
            }
        }
        r.hasPrev = p.hasPrev;
        if (p.hasPrev)
            for (uint32_t v = 0; v < p.nloop; ++v)
            {
                float c[4];
                for (int l = 0; l < 4; ++l)
                    c[l] = p.pos[v][0] * p.prevM[0][l] + p.pos[v][1] * p.prevM[1][l] + p.pos[v][2] * p.prevM[2][l] + p.prevM[3][l];
                const bool ok = c[3] > 1e-3f;
                r.prev[v][0] = ok ? c[0] / c[3] * p.vs[0] + p.vo[0] : -1.0e9f;
                r.prev[v][1] = ok ? c[1] / c[3] * p.vs[1] + p.vo[1] : -1.0e9f;
            }
        uint8_t *tag = const_cast<uint8_t *>(data);
        const uint16_t mark = static_cast<uint16_t>(slot + 1u);
        std::memcpy(tag + 2, &mark, 2); // tag bits 16-31
        if (mode() != 2)
            return;
        // Verify: screen = (clip.xy / clip.w) * vf25 + vf26 against the packet's XYZF2 (12.4 fixed point).
        for (uint32_t k = 0; k < p.nloop; ++k)
        {
            const uint8_t *xyz = data + 16u + k * 48u + 32u;
            // Lighting step 2: M07's colour from the tapped normal and light set (listing 0x0b48-0x0c40: d = max(L . n, 0)
            // per light, colour = ambient + sum colour_i * d_i, kept within 0..clamp, truncated) against the packet's RGBA.
            if (litModule(p.module) && p.hasNormal)
            {
                float d[4];
                for (int i = 0; i < 4; ++i)
                    d[i] = std::max(0.0f, p.light[0][i] * p.nrm[k][0] + p.light[1][i] * p.nrm[k][1] + p.light[2][i] * p.nrm[k][2]);
                for (int i = 2; i < 4; ++i)
                    for (float e = 1.0f; e < p.lightPow; e *= 2.0f)
                        d[i] *= d[i];
                uint64_t worst = 0;
                for (int l = 0; l < 4; ++l)
                {
                    float c = p.ambient[l];
                    for (int i = 0; i < 4; ++i)
                        c += p.lightColour[i][l] * d[i];
                    c = std::min(std::max(c, 0.0f), p.lightClamp[l]);
                    uint32_t got;
                    std::memcpy(&got, data + 16u + k * 48u + 16u + l * 4u, 4);
                    const int64_t diff = std::llabs(static_cast<int64_t>(c) - static_cast<int64_t>(got & 0xffu));
                    worst = std::max<uint64_t>(worst, static_cast<uint64_t>(diff));
                }
                ++g_s.litChecked[p.module];
                if (worst <= 1u)
                    ++g_s.litMatched[p.module];
                g_s.litWorst[p.module] = std::max(g_s.litWorst[p.module], worst);
            }
            if (p.module == 10 && p.hasNormal)
            {
                Record e;
                std::memcpy(e.pos[0], p.pos[k], sizeof(e.pos[0]));
                std::memcpy(e.nrm[0], p.nrm[k], sizeof(e.nrm[0]));
                std::memcpy(e.eye, p.eye, sizeof(e.eye));
                std::memcpy(e.envRot, p.envRot, sizeof(e.envRot));
                std::memcpy(e.fresnel, p.fresnel, sizeof(e.fresnel));
                std::memcpy(e.inColour[0], p.inColour[k], sizeof(e.inColour[0]));
                float u, v, col[4];
                envVertex(e, 0u, u, v, col);
                float stq[3];
                std::memcpy(stq, data + 16u + k * 48u, 12);
                ++g_s.envChecked;
                if (stq[2] > 0.0f)
                {
                    const double du = std::fabs(stq[0] / stq[2] - u) * 256.0, dv = std::fabs(stq[1] / stq[2] - v) * 256.0;
                    const double worst = std::max(du, dv);
                    if (worst > 0.125)
                        ++g_s.envUvOff;
                    g_s.envUvWorst = std::max(g_s.envUvWorst, worst);
                }
                uint64_t worst = 0;
                for (int l = 0; l < 4; ++l)
                {
                    uint32_t got;
                    std::memcpy(&got, data + 16u + k * 48u + 16u + l * 4u, 4);
                    const int64_t diff = std::llabs(static_cast<int64_t>(col[l]) - static_cast<int64_t>(got & 0xffu));
                    worst = std::max<uint64_t>(worst, static_cast<uint64_t>(diff));
                }
                if (worst <= 1u)
                    ++g_s.envColourMatched;
                g_s.envColourWorst = std::max(g_s.envColourWorst, worst);
            }
            uint32_t w0, w1, w3;
            std::memcpy(&w0, xyz, 4);
            std::memcpy(&w1, xyz + 4, 4);
            std::memcpy(&w3, xyz + 12, 4);
            const float x = p.pos[k][0], y = p.pos[k][1], z = p.pos[k][2];
            float c[4];
            for (int l = 0; l < 4; ++l)
                c[l] = x * p.m[0][l] + y * p.m[1][l] + z * p.m[2][l] + p.m[3][l];
            if (!(c[3] > 1e-3f))
                continue; // behind the camera: the clipper's business
            const float sx = c[0] / c[3] * p.vs[0] + p.vo[0], sy = c[1] / c[3] * p.vs[1] + p.vo[1];
            if (sx < 0.0f || sy < 0.0f || sx >= 4096.0f || sy >= 4096.0f)
                continue;
            ++g_s.checked;
            // DLSS step 1: the vertex's Q (ST qword, bits 64-95) against 1 / clip w (what a clipper vertex's
            // position could be recovered from without Z).
            {
                float q;
                std::memcpy(&q, data + 16u + k * 48u + 8u, 4);
                ++g_s.qChecked;
                if (!(q > 0.0f) || std::fabs(1.0f / q - c[3]) > 1e-3f * std::fabs(c[3]))
                    ++g_s.qOff;
            }
            // DLSS step 1: the packet's screen position and Q back to local space should give the input position
            // (what the clipper records rely on). Off: more than 1% of the vertex's distance from its group's origin
            // (and 0.05 units).
            if (p.haveSolve)
            {
                float q;
                std::memcpy(&q, data + 16u + k * 48u + 8u, 4);
                double l[3];
                // (measured where it matters: both through last frame's matrix, in GS pixels; off: over 1/4 px)
                double a[4], b[4];
                if (p.hasPrev && toLocal(p.solve, p.m, p.vs, p.vo, (w0 & 0xffffu) / 16.0, (w1 & 0xffffu) / 16.0, q, l))
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        a[i] = l[0] * p.prevM[0][i] + l[1] * p.prevM[1][i] + l[2] * p.prevM[2][i] + p.prevM[3][i];
                        b[i] = x * p.prevM[0][i] + y * p.prevM[1][i] + z * p.prevM[2][i] + p.prevM[3][i];
                    }
                    if (a[3] > 1e-3 && b[3] > 1e-3)
                    {
                        const double ex = (a[0] / a[3] - b[0] / b[3]) * p.vs[0], ey = (a[1] / a[3] - b[1] / b[3]) * p.vs[1];
                        const double e = std::sqrt(ex * ex + ey * ey);
                        ++g_s.unprojChecked;
                        const int cb = p.cond > 0.0 ? std::min(6, static_cast<int>(-std::log10(p.cond))) : 6;
                        const int mb = p.module >= 0 && p.module < 15 ? p.module : 15;
                        ++g_s.condChecked[cb];
                        ++g_s.modChecked[mb];
                        if (e > 0.25)
                        {
                            ++g_s.condOff[cb];
                            ++g_s.modOff[mb];
                            ++g_s.unprojOff;
                            if (e > g_s.unprojWorst && e < 1e6)
                                g_s.unprojWorst = e;
                        }
                    }
                }
            }
            const int64_t px = static_cast<int64_t>(w0 & 0xffffu), py = static_cast<int64_t>(w1 & 0xffffu);
            const int64_t dx = std::llabs(static_cast<int64_t>(std::lround(sx * 16.0f)) - px);
            const int64_t dy = std::llabs(static_cast<int64_t>(std::lround(sy * 16.0f)) - py);
            const uint64_t d = static_cast<uint64_t>(dx > dy ? dx : dy);
            if (d <= 1u)
                ++g_s.matched;
            else if (d > g_s.worst && d < 65536u)
                g_s.worst = d;
            (void)w3;
        }
    }

    void endProgram() { g_p.active = false; }

    const Record *record(uint32_t slot) { return (g_ring && slot < kRing) ? &g_ring[slot] : nullptr; }

    bool vertexPrev(uint16_t mark, uint32_t k, float x, float y, float q, float &px, float &py)
    {
        if (!mark)
            return false;
        if (mark & kClipMark)
        {
            const uint32_t slot = (mark & 0x7FFFu) - 1u;
            if (!g_clipRing || slot >= kClipRing)
                return false;
            const ClipRecord &r = g_clipRing[slot];
            double l[3];
            if (!r.hasPrev || !toLocal(r.solve, r.m, r.vs, r.vo, x, y, q, l))
                return false;
            double c[4];
            for (int i = 0; i < 4; ++i)
                c[i] = l[0] * r.prevM[0][i] + l[1] * r.prevM[1][i] + l[2] * r.prevM[2][i] + r.prevM[3][i];
            if (!(c[3] > 1e-3))
                return false;
            px = static_cast<float>(c[0] / c[3] * r.vs[0] + r.vo[0]);
            py = static_cast<float>(c[1] / c[3] * r.vs[1] + r.vo[1]);
            return true;
        }
        const Record *r = record(mark - 1u);
        if (!r || k >= r->count || !r->hasPrev || r->prev[k][0] < -1.0e8f)
            return false;
        px = r->prev[k][0];
        py = r->prev[k][1];
        return true;
    }

    bool markFrame(uint16_t mark, uint64_t &frame)
    {
        if (!mark)
            return false;
        if (mark & kClipMark)
        {
            const uint32_t slot = (mark & 0x7FFFu) - 1u;
            if (!g_clipRing || slot >= kClipRing)
                return false;
            frame = g_clipRing[slot].frame;
            return true;
        }
        const Record *r = record(mark - 1u);
        if (!r)
            return false;
        frame = r->frame;
        return true;
    }

    bool carMark(uint16_t mark)
    {
        if (!mark)
            return false;
        uint32_t slot = mark - 1u;
        if (mark & kClipMark)
        {
            const uint32_t c = (mark & 0x7FFFu) - 1u;
            if (!g_clipRing || c >= kClipRing || g_clipRing[c].recordSlot == ~0u)
                return false;
            slot = g_clipRing[c].recordSlot;
        }
        const Record *r = record(slot);
        return r && r->hasNormal;
    }

    namespace
    {
        std::mutex g_sunMutex;
        float g_sun[3] = {}, g_up[3] = {};
        uint64_t g_sunFrame = 0;
        bool g_sunOk = false;
    } // namespace
    // (endProgram: a car body's light set, light 0 = GT4's sun; its direction and the car's up (local +y) in the renderer's
    // view space: x and y as GS pixels grow (the viewport scale's signs), z = clip w)
    void noteSun(const Record &r)
    {
        const float l0[3] = {r.light[0][0], r.light[1][0], r.light[2][0]}, up[3] = {0.0f, 1.0f, 0.0f};
        float a[3], b[3];
        viewNormal(r, l0, a);
        viewNormal(r, up, b);
        const float sx = r.vs[0] < 0.0f ? -1.0f : 1.0f, sy = r.vs[1] < 0.0f ? -1.0f : 1.0f;
        std::lock_guard<std::mutex> lock(g_sunMutex);
        g_sun[0] = sx * a[0], g_sun[1] = sy * a[1], g_sun[2] = -a[2];
        g_up[0] = sx * b[0], g_up[1] = sy * b[1], g_up[2] = -b[2];
        g_sunFrame = r.frame;
        g_sunOk = std::isfinite(g_sun[0]) && std::isfinite(g_up[0]);
    }
    bool sun(float L[3], float up[3], uint64_t &frame)
    {
        std::lock_guard<std::mutex> lock(g_sunMutex);
        if (!g_sunOk)
            return false;
        std::memcpy(L, g_sun, sizeof(g_sun));
        std::memcpy(up, g_up, sizeof(g_up));
        frame = g_sunFrame;
        return true;
    }

    // GT4_VU1_TAP=verify on the renderer's side: a tapped vertex as the GS got it against its record.
    namespace
    {
        std::atomic<uint64_t> g_gsChecked{0}, g_gsMatched{0}, g_gsLogged{0};
    }
    void checkVertex(uint32_t slot, uint32_t k, float gsX, float gsY)
    {
        const Record *r = record(slot);
        if (!r || k >= r->count)
            return;
        const float x = r->pos[k][0], y = r->pos[k][1], z = r->pos[k][2];
        float c[4];
        for (int l = 0; l < 4; ++l)
            c[l] = x * r->m[0][l] + y * r->m[1][l] + z * r->m[2][l] + r->m[3][l];
        if (!(c[3] > 1e-3f))
            return;
        const float sx = c[0] / c[3] * r->vs[0] + r->vo[0], sy = c[1] / c[3] * r->vs[1] + r->vo[1];
        const uint64_t n = ++g_gsChecked;
        if (std::fabs(sx - gsX) <= 0.07f && std::fabs(sy - gsY) <= 0.07f) // (the kernel truncates to 1/16 px)
            ++g_gsMatched;
        else
        {
            static std::atomic<int> shown{0};
            if (shown++ < 12)
                std::fprintf(stderr, "[gt4-tap] renderer side mismatch: slot %u vertex %u of %u (module %d, serial %llu of %llu): record %.3f,%.3f gs %.3f,%.3f\n",
                             slot, k, r->count, r->module, static_cast<unsigned long long>(r->serial), static_cast<unsigned long long>(g_serial), sx, sy, gsX,
                             gsY);
        }
        if ((n & ((1u << 24) - 1u)) == 0u && g_gsLogged++ < 64u)
            std::fprintf(stderr, "[gt4-tap] renderer side: %llu tapped vertices checked, %llu at their record's position\n",
                         static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_gsMatched.load()));
    }
    bool verifying() { return mode() == 2; }
    bool savePng(const char *path, int width, int height, const uint8_t *rgba)
    {
        Image img{};
        img.data = const_cast<uint8_t *>(rgba);
        img.width = width;
        img.height = height;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        return ExportImage(img, path);
    }
    int debugView() { return mode() == 3 ? 1 : mode() == 4 ? 2 : mode() == 5 ? 3 : 0; }
} // namespace gt4tap
