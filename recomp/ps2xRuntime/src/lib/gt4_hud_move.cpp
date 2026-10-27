#include "gt4_hud_move.h"

#if defined(PS2X_GAME_GT4) && !defined(GT4_STOCK_BUILD)

#include "gt4_hud_pictures.h"
#include "gt4_mirror.h"
#include "runtime/gs/gs_types.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// HUD hand-off 7 B (gt4_hud_move.h).

namespace gt4hudmove
{
    namespace
    {
        enum Part
        {
            Position,
            Lap,
            Clock,
            Besttimes,
            Map,
            Panel,
            Fuel,
            Warnings,
            Gbar,
            Gearspeed,
            Mirror,
            kPartCount
        };
        constexpr const char *kPartNames[kPartCount] = {"position", "lap",      "clock", "besttimes", "map",   "panel",
                                                        "fuel",     "warnings", "gbar",  "gearspeed", "mirror"};
        constexpr int kPieces = 4; // the compact panel's pieces, left to right: fuel, warnings, gbar, gearspeed

        // RaceDisplay's members that draw the parts (HUD hand-off 7 A1).
        struct Mem
        {
            uint32_t off;
            int part;
        };
        constexpr Mem kMem[] = {{0x1398u, Position}, {0x1668u, Position}, {0x1300u, Lap},       {0x1684u, Lap},       {0x13f0u, Besttimes},
                                {0x1438u, Besttimes}, {0x14a8u, Besttimes}, {0x1518u, Besttimes}, {0x1588u, Besttimes}, {0x16a0u, Besttimes},
                                {0x16bcu, Besttimes}, {0x15f8u, Clock},     {0x0108u, Panel},     {0x0d58u, Panel},     {0x1108u, Map}};
        constexpr int kMemCount = static_cast<int>(sizeof(kMem) / sizeof(kMem[0]));
        constexpr uint32_t kCompact = 0x0d58u;

        struct Move
        {
            bool on = false;
            float x = 50.0f, y = 50.0f; // percent of the HUD area
            float size = 100.0f;        // percent
        };

        struct Box
        {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            bool empty() const { return x1 < x0; }
            void add(float a0, float b0, float a1, float b1)
            {
                x0 = std::min(x0, a0), y0 = std::min(y0, b0), x1 = std::max(x1, a1), y1 = std::max(y1, b1);
            }
            void add(const Box &b)
            {
                if (!b.empty())
                    add(b.x0, b.y0, b.x1, b.y1);
            }
            bool near(const Box &b, float tol) const
            {
                return std::fabs(x0 - b.x0) <= tol && std::fabs(y0 - b.y0) <= tol && std::fabs(x1 - b.x1) <= tol && std::fabs(y1 - b.y1) <= tol;
            }
            float cx() const { return (x0 + x1) * 0.5f; }
            float cy() const { return (y0 + y1) * 0.5f; }
        };

        // ---- settings (read once)
        struct Settings
        {
            Move m[kPartCount];
            bool any = false;
            bool log = false; // GT4_HUD_MOVE_LOG=1 (tests): what was learned, the packet cursor's use
        };
        const Settings &settings()
        {
            static const Settings s = [] {
                Settings r;
                const char *v = std::getenv("GT4_HUD_MOVE");
                const char *lg = std::getenv("GT4_HUD_MOVE_LOG");
                r.log = lg && *lg == '1';
                if (!v || !*v)
                    return r;
                std::string all = v, said;
                size_t at = 0;
                while (at <= all.size())
                {
                    size_t end = all.find(';', at);
                    if (end == std::string::npos)
                        end = all.size();
                    std::string item = all.substr(at, end - at);
                    at = end + 1;
                    item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }), item.end());
                    if (item.empty())
                        continue;
                    const size_t colon = item.find(':');
                    int part = -1;
                    for (int p = 0; p < kPartCount && colon != std::string::npos; ++p)
                        if (item.compare(0, colon, kPartNames[p]) == 0 && std::strlen(kPartNames[p]) == colon)
                            part = p;
                    float x = 0, y = 0, size = 0;
                    char tail = 0;
                    if (part < 0 || std::sscanf(item.c_str() + colon + 1, "%f,%f,%f%c", &x, &y, &size, &tail) != 3 || !(x >= 0.0f && x <= 100.0f) ||
                        !(y >= 0.0f && y <= 100.0f) || !(size >= 40.0f && size <= 250.0f))
                    {
                        std::fprintf(stderr, "[gt4-hud-move] GT4_HUD_MOVE: '%s' not understood (<part>:<x 0-100>,<y 0-100>,<size 40-250>; parts: position, "
                                             "lap, clock, besttimes, map, panel, fuel, warnings, gbar, gearspeed, mirror): left where GT4 puts it\n",
                                     item.c_str());
                        continue;
                    }
                    r.m[part] = Move{true, x, y, size};
                    r.any = true;
                    char t[96];
                    std::snprintf(t, sizeof(t), "%s%s at %.1f,%.1f %.0f%%", said.empty() ? "" : ", ", kPartNames[part], x, y, size);
                    said += t;
                }
                if (r.any)
                    std::fprintf(stderr, "[gt4-hud-move] GT4's own HUD parts moved: %s\n", said.c_str());
                return r;
            }();
            return s;
        }

        bool piecesMoved()
        {
            const Settings &s = settings();
            return s.m[Fuel].on || s.m[Warnings].on || s.m[Gbar].on || s.m[Gearspeed].on;
        }

        // ---- the GS frontend's own (one thread at a time: the GS lock)
        struct MemState
        {
            Box frame;      // this frame's box (raw: where GT4 draws it)
            uint64_t seen = 0; // the last frame it drew in (frame counter)
            bool frozen = false;
            Box ref;        // GT4's place, once kept
            Box seed;       // GT4's place in GT4's 16:9 frame (640 x 448) before it is kept (seedPlaces)
            Box anchor, win; // the run of frames within 2 pixels of anchor, and their union
            int run = 0, tries = 0;
        };
        MemState g_mem[kMemCount];
        int g_cur = -1;      // the bracketed member (kMem index), -1 none
        bool g_inFrame = false;
        uint64_t g_frame = 0;
        // The frame's copy of GT4's 3D buffer (TBP 0x1a40) into the shown buffer, before its HUD: that buffer and the HUD area.
        uint32_t g_copyFbp = ~0u, g_shownFbp = ~0u;
        // The buffers GT4 copied its 3D into lately (its shown buffers, double buffered): the HUD is drawn into one of them. (A
        // frame where GT4 changes the view may draw its HUD without a copy of its own: the last one's buffer is then not it.)
        uint32_t g_hudFbps[4] = {~0u, ~0u, ~0u, ~0u};
        // (GT4_HUD_MOVE_LOG: this frame's counts, for a frame where nothing was moved)
        int g_fMembers = 0, g_fMoved = 0, g_fSkipped = 0, g_fOutside = 0;
        uint32_t g_fSkipFbp = 0;
        bool g_movedSeen = false; // (a frame moved something: the race's HUD is up; frames before it are not logged)
        void frameDiag(const char *why)
        {
            static int said = 0;
            if (g_fMoved > 0)
                g_movedSeen = true;
            if (!settings().log || !g_movedSeen || g_fMoved > 0 || said >= 40)
                return;
            ++said;
            std::fprintf(stderr, "[gt4-hud-move] frame %llu (%s): nothing moved; %d member markers, %d draws into another buffer (FBP %03x), %d draws into "
                                 "the shown buffers outside a member; shown %03x, buffers %03x %03x\n",
                         static_cast<unsigned long long>(g_frame), why, g_fMembers, g_fSkipped, g_fSkipFbp, g_fOutside, g_shownFbp, g_hudFbps[0], g_hudFbps[1]);
        }
        bool hudFbp(uint32_t fbp)
        {
            for (uint32_t f : g_hudFbps)
                if (f == fbp)
                    return true;
            return false;
        }
        Box g_copyBox, g_area;
        float g_areaW = 640.0f, g_areaH = 448.0f, g_areaX = 0.0f, g_areaY = 0.0f;
        // The compact panel's pieces (0 not split yet, 1 split into kPieces, -1 could not be).
        int g_pieceState = 0;
        Box g_piece[kPieces];
        std::vector<Box> g_prims; // the compact panel's primitives this frame (raw)
        int g_pieceTries = 0;

        // Published for the other threads.
        std::mutex g_pubMutex;
        std::vector<PartPlace> g_pub, g_capPub; // (g_capPub: as on the last race picture's frame, its markers without 0x100)
        bool g_pubPieces = false, g_capPieces = false;
        std::atomic<float> g_pubAreaX{0.0f}, g_pubAreaY{0.0f}, g_pubAreaW{640.0f}, g_pubAreaH{448.0f};

        int memIndex(uint32_t off)
        {
            for (int i = 0; i < kMemCount; ++i)
                if (kMem[i].off == off)
                    return i;
            return -1;
        }

        // A member's place: kept, else the seed (none: empty).
        const Box &memPlace(int i) { return g_mem[i].frozen ? g_mem[i].ref : g_mem[i].seed; }

        // GT4's place of a part: the union of its members' places (the panel: the member drawing now; its members never draw
        // in the same view).
        bool partRef(int part, int cur, Box &ref)
        {
            ref = Box{};
            if (part == Panel)
            {
                if (cur >= 0)
                    ref = memPlace(cur);
                return !ref.empty();
            }
            for (int i = 0; i < kMemCount; ++i)
                if (kMem[i].part == part)
                    ref.add(memPlace(i));
            return !ref.empty();
        }

        // The places GT4 draws its parts at in its 16:9 frame (GT4_ASPECT 16:9 or unset, one screen; 640 x 448 at 0, 0), as
        // learned on Autumn Ring Mini (hud7b-all, 1920x1080 3x): used until each member's own place is kept, so a moved part
        // is at its place from the first frame. Other shapes learn only.
        bool g_seeded = false;
        Box g_pieceSeed[kPieces];
        void seedPlaces()
        {
            static const bool shape169 = [] {
                const char *a = std::getenv("GT4_ASPECT");
                const char *sc = std::getenv("GT4_SCREENS");
                const std::string as = a ? a : "";
                return (as.empty() || as == "16:9") && !(sc && std::strcmp(sc, "3") == 0);
            }();
            g_seeded = shape169 && g_areaX == 0.0f && g_areaY == 0.0f && g_areaW == 640.0f && g_areaH == 448.0f;
            if (!g_seeded)
                return;
            struct Seed
            {
                uint32_t off;
                float x0, y0, x1, y1;
            };
            static constexpr Seed kSeeds[] = {{0x1108u, 25.5f, 89.1f, 160.5f, 257.1f},  {0x1398u, 33.8f, 37.8f, 65.2f, 79.8f},
                                              {0x1668u, 21.8f, 19.1f, 77.2f, 37.8f},    {0x1300u, 89.5f, 48.3f, 128.5f, 71.5f},
                                              {0x1684u, 93.0f, 19.1f, 124.5f, 37.8f},   {0x14a8u, 531.5f, 42.5f, 596.0f, 56.5f},
                                              {0x1588u, 531.5f, 90.5f, 596.0f, 104.5f}, {0x16a0u, 534.0f, 19.1f, 594.0f, 37.8f},
                                              {0x16bcu, 534.0f, 68.6f, 594.0f, 87.2f},  {0x15f8u, 274.1f, 83.5f, 364.2f, 105.9f},
                                              {0x0108u, 152.2f, 309.5f, 462.0f, 432.6f}, {0x0d58u, 27.0f, 370.9f, 612.0f, 432.6f}};
            for (const Seed &sd : kSeeds)
                if (const int i = memIndex(sd.off); i >= 0)
                {
                    g_mem[i].seed = Box{};
                    g_mem[i].seed.add(sd.x0, sd.y0, sd.x1, sd.y1);
                }
            // (the compact panel's pieces: fuel, warnings, g bar, gear and speed)
            static constexpr float kPieceSeeds[kPieces][4] = {
                {27.0f, 371.3f, 43.5f, 433.0f}, {87.8f, 394.7f, 152.4f, 433.0f}, {239.4f, 410.6f, 400.0f, 433.0f}, {492.4f, 391.9f, 612.5f, 433.0f}};
            for (int k = 0; k < kPieces; ++k)
            {
                g_pieceSeed[k] = Box{};
                g_pieceSeed[k].add(kPieceSeeds[k][0], kPieceSeeds[k][1], kPieceSeeds[k][2], kPieceSeeds[k][3]);
            }
        }

        // Where a part's box goes: the move's centre (kept inside the HUD area) and scale; whole pixels at 100 %.
        void target(const Box &ref, const Move &mv, float &tx, float &ty, float &s)
        {
            s = mv.size / 100.0f;
            const float hw = (ref.x1 - ref.x0) * s * 0.5f, hh = (ref.y1 - ref.y0) * s * 0.5f;
            tx = g_areaX + mv.x / 100.0f * g_areaW;
            ty = g_areaY + mv.y / 100.0f * g_areaH;
            tx = std::clamp(tx, g_areaX + std::min(hw, g_areaW * 0.5f), g_areaX + std::max(g_areaW - hw, g_areaW * 0.5f));
            ty = std::clamp(ty, g_areaY + std::min(hh, g_areaH * 0.5f), g_areaY + std::max(g_areaH - hh, g_areaH * 0.5f));
            if (mv.size == 100.0f)
                tx = ref.cx() + std::round(tx - ref.cx()), ty = ref.cy() + std::round(ty - ref.cy());
        }
        Box moved(const Box &ref, const Move &mv)
        {
            float tx = 0, ty = 0, s = 1;
            target(ref, mv, tx, ty, s);
            Box b;
            b.add(tx + (ref.x0 - ref.cx()) * s, ty + (ref.y0 - ref.cy()) * s, tx + (ref.x1 - ref.cx()) * s, ty + (ref.y1 - ref.cy()) * s);
            return b;
        }

        void transform(GSPrimitiveBatch &batch, const Box &ref, const Move &mv)
        {
            float tx = 0, ty = 0, s = 1;
            target(ref, mv, tx, ty, s);
            GSContext &ctx = batch.state.context;
            const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
            const float cx = ref.cx(), cy = ref.cy();
            for (uint32_t i = 0; i < batch.vertexCount && i < 3u; ++i)
            {
                GSVertex &v = batch.vertices[i];
                const float fx = tx + (v.x - ofx - cx) * s, fy = ty + (v.y - ofy - cy) * s;
                v.x = std::round((ofx + fx) * 16.0f) / 16.0f;
                v.y = std::round((ofy + fy) * 16.0f) / 16.0f;
            }
            // The scissor moved the same way: what GT4 clipped stays clipped, what it showed stays inside.
            GSScissorReg &sc = ctx.scissor;
            const float sx0 = tx + (static_cast<float>(sc.x0) - cx) * s, sx1 = tx + (static_cast<float>(sc.x1) + 1.0f - cx) * s;
            const float sy0 = ty + (static_cast<float>(sc.y0) - cy) * s, sy1 = ty + (static_cast<float>(sc.y1) + 1.0f - cy) * s;
            sc.x0 = static_cast<uint16_t>(std::clamp(std::floor(sx0), 0.0f, 2047.0f));
            sc.x1 = static_cast<uint16_t>(std::clamp(std::ceil(sx1) - 1.0f, 0.0f, 2047.0f));
            sc.y0 = static_cast<uint16_t>(std::clamp(std::floor(sy0), 0.0f, 2047.0f));
            sc.y1 = static_cast<uint16_t>(std::clamp(std::ceil(sy1) - 1.0f, 0.0f, 2047.0f));
        }

        // The compact panel's primitives merged while they touch (within 6 frame pixels), left to right.
        std::vector<Box> clusters(const std::vector<Box> &prims)
        {
            std::vector<Box> c = prims;
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (size_t i = 0; i < c.size() && !merged; ++i)
                    for (size_t j = i + 1; j < c.size(); ++j)
                        if (c[i].x0 - 6.0f <= c[j].x1 && c[j].x0 - 6.0f <= c[i].x1 && c[i].y0 - 6.0f <= c[j].y1 && c[j].y0 - 6.0f <= c[i].y1)
                        {
                            c[i].add(c[j]);
                            c.erase(c.begin() + static_cast<long>(j));
                            merged = true;
                            break;
                        }
            }
            c.erase(std::remove_if(c.begin(), c.end(), [](const Box &b) { return b.x1 - b.x0 < 2.0f && b.y1 - b.y0 < 2.0f; }), c.end());
            std::sort(c.begin(), c.end(), [](const Box &a, const Box &b) { return a.x0 < b.x0; });
            return c;
        }

        int pieceOf(const Box &b, const Box *pieces)
        {
            const float px = b.cx(), py = b.cy();
            int best = 0;
            float bestD = 1e30f;
            for (int k = 0; k < kPieces; ++k)
            {
                const Box &p = pieces[k];
                const float dx = px < p.x0 ? p.x0 - px : px > p.x1 ? px - p.x1 : 0.0f;
                const float dy = py < p.y0 ? p.y0 - py : py > p.y1 ? py - p.y1 : 0.0f;
                const float d = dx * dx + dy * dy;
                if (d < bestD)
                    bestD = d, best = k;
            }
            return best;
        }

        void resetLearning(const char *why)
        {
            for (MemState &m : g_mem)
                m = MemState{};
            g_pieceState = 0;
            g_pieceTries = 0;
            seedPlaces();
            if (settings().log)
                std::fprintf(stderr, "[gt4-hud-move] GT4's places learned again (%s)\n", why);
        }

        // The end of RaceDisplay's draw: each member's box this frame kept towards GT4's place; the places published.
        void frameEnd()
        {
            const Settings &st = settings();
            for (int i = 0; i < kMemCount; ++i)
            {
                MemState &m = g_mem[i];
                if (m.frame.empty())
                    continue;
                m.seen = g_frame;
                if (!m.frozen)
                {
                    ++m.tries;
                    if (m.run > 0 && m.frame.near(m.anchor, 2.0f))
                        ++m.run, m.win.add(m.frame);
                    else
                        m.anchor = m.frame, m.win = m.frame, m.run = 1;
                    if (m.run >= 20 || m.tries >= 300)
                    {
                        m.frozen = true;
                        m.ref = m.win;
                        if (st.log)
                            std::fprintf(stderr, "[gt4-hud-move] GT4's place of member +0x%x (%s): %.1f,%.1f - %.1f,%.1f (%s, frame %llu, drawn in %d frames)\n", kMem[i].off,
                                         kPartNames[kMem[i].part], m.ref.x0, m.ref.y0, m.ref.x1, m.ref.y1, m.run >= 20 ? "20 frames the same" : "after 300 frames",
                                         static_cast<unsigned long long>(g_frame), m.tries);
                    }
                }
                // The compact panel's pieces: from a frame where its box is GT4's place.
                if (kMem[i].off == kCompact && m.frozen && g_pieceState == 0 && m.frame.near(m.ref, 2.0f) && !g_prims.empty())
                {
                    const std::vector<Box> c = clusters(g_prims);
                    if (c.size() == static_cast<size_t>(kPieces))
                    {
                        for (int k = 0; k < kPieces; ++k)
                            g_piece[k] = c[static_cast<size_t>(k)];
                        g_pieceState = 1;
                        std::fprintf(stderr, "[gt4-hud-move] the chase views' compact panel split into its %d pieces (fuel %.0f-%.0f, warnings %.0f-%.0f, g bar %.0f-%.0f, gear and speed %.0f-%.0f)\n",
                                     kPieces, g_piece[0].x0, g_piece[0].x1, g_piece[1].x0, g_piece[1].x1, g_piece[2].x0, g_piece[2].x1, g_piece[3].x0, g_piece[3].x1);
                    }
                    else if (++g_pieceTries >= 120)
                    {
                        g_pieceState = -1;
                        std::fprintf(stderr, "[gt4-hud-move] the chase views' compact panel did not split into %d pieces (%zu found): it moves as one part "
                                             "(panel)\n",
                                     kPieces, c.size());
                    }
                }
            }
            // Published: GT4's place of each part and where it is drawn.
            std::vector<PartPlace> pub;
            for (int p = 0; p < kPartCount; ++p)
            {
                if (p == Mirror || p == Fuel || p == Warnings || p == Gbar || p == Gearspeed)
                    continue;
                Box ref;
                if (p == Panel)
                {
                    for (int i = 0; i < kMemCount; ++i)
                        if (kMem[i].part == Panel && g_mem[i].seen == g_frame)
                            ref = memPlace(i);
                }
                else
                    partRef(p, -1, ref);
                if (ref.empty())
                    continue;
                PartPlace pp;
                pp.part = kPartNames[p];
                pp.x0 = ref.x0, pp.y0 = ref.y0, pp.x1 = ref.x1, pp.y1 = ref.y1;
                const bool compactNow = p == Panel && g_mem[memIndex(kCompact)].seen == g_frame;
                pp.moved = st.m[p].on && !(compactNow && (g_pieceState == 1 || (g_pieceState == 0 && g_seeded)));
                if (pp.moved)
                {
                    const Box b = moved(ref, st.m[p]);
                    pp.mx0 = b.x0, pp.my0 = b.y0, pp.mx1 = b.x1, pp.my1 = b.y1;
                }
                pub.push_back(pp);
            }
            const bool pieces = (g_pieceState == 1 || (g_pieceState == 0 && g_seeded)) && g_mem[memIndex(kCompact)].seen == g_frame;
            if (pieces)
                for (int k = 0; k < kPieces; ++k)
                {
                    PartPlace pp;
                    pp.part = kPartNames[Fuel + k];
                    const Box &ref = g_pieceState == 1 ? g_piece[k] : g_pieceSeed[k];
                    pp.x0 = ref.x0, pp.y0 = ref.y0, pp.x1 = ref.x1, pp.y1 = ref.y1;
                    pp.moved = st.m[Fuel + k].on;
                    if (pp.moved)
                    {
                        const Box b = moved(ref, st.m[Fuel + k]);
                        pp.mx0 = b.x0, pp.my0 = b.y0, pp.mx1 = b.x1, pp.my1 = b.y1;
                    }
                    pub.push_back(pp);
                }
            {
                std::lock_guard<std::mutex> lock(g_pubMutex);
                g_pub = std::move(pub);
                g_pubPieces = pieces;
            }
            g_pubAreaX.store(g_areaX, std::memory_order_relaxed), g_pubAreaY.store(g_areaY, std::memory_order_relaxed);
            g_pubAreaW.store(g_areaW, std::memory_order_relaxed), g_pubAreaH.store(g_areaH, std::memory_order_relaxed);
        }
    } // namespace

    bool on() { return settings().any; }

    bool wantsMember(uint32_t off)
    {
        const Settings &s = settings();
        const int i = memIndex(off);
        if (i < 0)
            return false;
        if (off == kCompact)
            return s.m[Panel].on || piecesMoved();
        return s.m[kMem[i].part].on;
    }

    void gsMarker(uint32_t code)
    {
        if (!settings().any)
            return;
        const uint32_t c = code & 0xffu;
        if (c == 0xfeu)
        {
            // RaceDisplay's draw begins: the HUD area is the copy of the 3D buffer just made (GT4's frame).
            if (g_inFrame)
                frameDiag("no end marker");
            g_fMembers = g_fMoved = g_fSkipped = g_fOutside = 0;
            static int saidOpen = 0;
            if (g_inFrame && settings().log && saidOpen < 12)
                ++saidOpen, std::fprintf(stderr, "[gt4-hud-move] frame %llu: no end marker before the next RaceDisplay draw\n",
                                         static_cast<unsigned long long>(g_frame));
            ++g_frame;
            g_inFrame = true;
            g_cur = -1;
            g_shownFbp = g_copyFbp;
            if (g_copyFbp != ~0u && !hudFbp(g_copyFbp))
            {
                for (int i = 3; i > 0; --i)
                    g_hudFbps[i] = g_hudFbps[i - 1];
                g_hudFbps[0] = g_copyFbp;
            }
            if (!g_copyBox.empty())
            {
                const float w = g_copyBox.x1 - g_copyBox.x0, h = g_copyBox.y1 - g_copyBox.y0;
                if (w >= 64.0f && h >= 64.0f)
                {
                    if (std::fabs(w - g_areaW) > 1.0f || std::fabs(h - g_areaH) > 1.0f || std::fabs(g_copyBox.x0 - g_areaX) > 1.0f ||
                        std::fabs(g_copyBox.y0 - g_areaY) > 1.0f)
                    {
                        if (settings().log || g_frame > 1u)
                            std::fprintf(stderr, "[gt4-hud-move] the HUD area: %.0f,%.0f %.0fx%.0f\n", g_copyBox.x0, g_copyBox.y0, w, h);
                        if (g_frame > 1u)
                            resetLearning("the frame's shape changed");
                    }
                    g_areaX = g_copyBox.x0, g_areaY = g_copyBox.y0, g_areaW = w, g_areaH = h;
                }
            }
            static bool seedTried = false;
            if (!seedTried)
                seedTried = true, seedPlaces();
            g_copyBox = Box{};
            for (MemState &m : g_mem)
                m.frame = Box{};
            g_prims.clear();
            return;
        }
        if (c == 0xfdu)
        {
            if (g_inFrame)
            {
                frameDiag("end");
                frameEnd();
                if (!(code & 0x100u))
                {
                    // (a race picture's frame: its places kept for its .json, written later on another thread)
                    std::lock_guard<std::mutex> lock(g_pubMutex);
                    g_capPub = g_pub;
                    g_capPieces = g_pubPieces;
                }
            }
            g_inFrame = false;
            g_cur = -1;
            return;
        }
        if (c == 0xffu)
        {
            g_cur = -1;
            return;
        }
        g_cur = g_inFrame ? memIndex(gt4hudpic::memberOffset(static_cast<int>(c))) : -1;
        g_fMembers += g_cur >= 0;
    }

    void apply(GSPrimitiveBatch &batch)
    {
        if (batch.vertexCount == 0u)
            return;
        const GSContext &ctx = batch.state.context;
        if (!g_inFrame)
        {
            // (before RaceDisplay's draw: the frame's copy of the 3D buffer into the shown buffer)
            if (batch.state.prim.tme && ctx.tex0.tbp0 == 0x1a40u && ctx.frame.fbp != 0xd2u)
            {
                if (ctx.frame.fbp != g_copyFbp)
                    g_copyBox = Box{};
                g_copyFbp = ctx.frame.fbp;
                const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
                for (uint32_t i = 0; i < batch.vertexCount && i < 3u; ++i)
                    g_copyBox.add(batch.vertices[i].x - ofx, batch.vertices[i].y - ofy, batch.vertices[i].x - ofx, batch.vertices[i].y - ofy);
            }
            return;
        }
        if (g_cur < 0)
        {
            g_fOutside += ctx.frame.fbp == g_shownFbp || hudFbp(ctx.frame.fbp);
            return;
        }
        if (ctx.frame.fbp != g_shownFbp && !hudFbp(ctx.frame.fbp))
        {
            ++g_fSkipped, g_fSkipFbp = ctx.frame.fbp;
            // (GT4_HUD_MOVE_LOG: a moved part's member drawing into a buffer that is not a shown one, left alone)
            static int said = 0;
            if (settings().log && said < 12)
                ++said, std::fprintf(stderr, "[gt4-hud-move] frame %llu: member +0x%x draws into FBP %03x (shown %03x): not moved\n",
                                     static_cast<unsigned long long>(g_frame), kMem[g_cur].off, ctx.frame.fbp, g_shownFbp);
            return;
        }
        const float ofx = static_cast<float>(ctx.xyoffset.ofx) / 16.0f, ofy = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
        Box raw;
        for (uint32_t i = 0; i < batch.vertexCount && i < 3u; ++i)
            raw.add(batch.vertices[i].x - ofx, batch.vertices[i].y - ofy, batch.vertices[i].x - ofx, batch.vertices[i].y - ofy);
        MemState &m = g_mem[g_cur];
        m.frame.add(raw);
        const Settings &st = settings();
        int part = kMem[g_cur].part;
        Box ref;
        if (kMem[g_cur].off == kCompact)
        {
            if (g_prims.size() < 512u)
                g_prims.push_back(raw);
            // Its pieces move on their own; "panel" moves it whole only when it could not be split (until it is split, it stays
            // where GT4 puts it).
            if (g_pieceState == 1 || (g_pieceState == 0 && g_seeded))
            {
                const Box *pieces = g_pieceState == 1 ? g_piece : g_pieceSeed;
                const int k = pieceOf(raw, pieces);
                part = Fuel + k;
                if (!st.m[part].on)
                    return;
                transform(batch, pieces[k], st.m[part]);
                ++g_fMoved;
                return;
            }
            if (g_pieceState == 0)
                return;
        }
        if (!st.m[part].on || !partRef(part, g_cur, ref))
            return;
        transform(batch, ref, st.m[part]);
        ++g_fMoved;
    }

    bool mirrorMoved()
    {
        static const bool m = settings().m[Mirror].on && gt4mirror::mode() != 2;
        return m;
    }

    bool mirrorPlace(float &cx, float &cy, float &scale, float &frameW, float &frameH)
    {
        if (!mirrorMoved())
            return false;
        const Move &mv = settings().m[Mirror];
        frameW = g_pubAreaW.load(std::memory_order_relaxed), frameH = g_pubAreaH.load(std::memory_order_relaxed);
        cx = g_pubAreaX.load(std::memory_order_relaxed) + mv.x / 100.0f * frameW;
        cy = g_pubAreaY.load(std::memory_order_relaxed) + mv.y / 100.0f * frameH;
        scale = mv.size / 100.0f;
        return true;
    }

    void places(std::vector<PartPlace> &out, bool &pieces)
    {
        std::lock_guard<std::mutex> lock(g_pubMutex);
        out = g_capPub;
        pieces = g_capPieces;
    }

    bool movedRect(const char *part, float &x0, float &y0, float &x1, float &y1)
    {
        int p = -1;
        for (int i = 0; i < kPartCount; ++i)
            if (std::strcmp(kPartNames[i], part) == 0)
                p = i;
        if (p < 0 || !settings().m[p].on)
            return false;
        const Move &mv = settings().m[p];
        const float aw = g_pubAreaW.load(std::memory_order_relaxed), ah = g_pubAreaH.load(std::memory_order_relaxed);
        const float ax = g_pubAreaX.load(std::memory_order_relaxed), ay = g_pubAreaY.load(std::memory_order_relaxed);
        const float s = mv.size / 100.0f, hw = (x1 - x0) * s * 0.5f, hh = (y1 - y0) * s * 0.5f;
        float tx = ax + mv.x / 100.0f * aw, ty = ay + mv.y / 100.0f * ah;
        tx = std::clamp(tx, ax + std::min(hw, aw * 0.5f), ax + std::max(aw - hw, aw * 0.5f));
        ty = std::clamp(ty, ay + std::min(hh, ah * 0.5f), ay + std::max(ah - hh, ah * 0.5f));
        x0 = tx - hw, x1 = tx + hw, y0 = ty - hh, y1 = ty + hh;
        return true;
    }
} // namespace gt4hudmove

#else

namespace gt4hudmove
{
    bool on() { return false; }
    bool wantsMember(uint32_t) { return false; }
    void gsMarker(uint32_t) {}
    void apply(GSPrimitiveBatch &) {}
    bool mirrorMoved() { return false; }
    bool mirrorPlace(float &, float &, float &, float &, float &) { return false; }
    void places(std::vector<PartPlace> &out, bool &pieces)
    {
        out.clear();
        pieces = false;
    }
    bool movedRect(const char *, float &, float &, float &, float &) { return false; }
} // namespace gt4hudmove

#endif
