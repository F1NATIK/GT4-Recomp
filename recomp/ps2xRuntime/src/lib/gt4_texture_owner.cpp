#include "gt4_texture_owner.h"

#if !defined(GT4_STOCK_BUILD)
#include "ps2_runtime.h"
#include "ps2x/iop/gt4_disc_reads.h"
#include "../launcher/gt4stock/gt4stock_names.h"
#include "gt4save.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr size_t kWindow = 32, kStep = 16;
    constexpr uint32_t kNoOwner = 0xFFFFFFFFu, kShared = 0xFFFFFFFEu;
    constexpr uint32_t kGsPages = 512; // 4 MB of GS memory in 8 KB pages

    uint64_t windowHash(const uint8_t *p)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < kWindow; ++i)
            h = (h ^ p[i]) * 0x100000001B3ull;
        h ^= h >> 33;
        h *= 0xFF51AFD7ED558CCDull;
        return h ^ (h >> 33);
    }

    // Says nothing about where it came from: one byte throughout, or the same 16 bytes twice (fills, padding, runs
    // of one colour).
    bool uninformative(const uint8_t *p)
    {
        if (std::memcmp(p, p + 16, 16) == 0)
            return true;
        for (size_t i = 1; i < kWindow; ++i)
            if (p[i] != p[0])
                return false;
        return true;
    }

    struct VolFile
    {
        uint64_t begin = 0, end = 0; // bytes of the disc image
        std::string path;
    };

    struct OwnerRec
    {
        gt4texowner::Owner::Kind kind = gt4texowner::Owner::None;
        std::string id; // the model code (cars) or the course label
        gt4texowner::Owner named;
        bool hasName = false;
    };

    struct Job
    {
        std::string path;
        uint32_t owner = kNoOwner;
    };

    std::mutex g_mutex;
    std::condition_variable g_wake, g_idle;
    std::vector<VolFile> g_files;            // car/, wheel/, tire/ and crs/ files, by disc position (read only once built)
    std::unordered_set<std::string> g_queued; // files queued or indexed
    std::deque<Job> g_jobs;
    bool g_busy = false, g_stop = false, g_ready = false;
    std::vector<OwnerRec> g_owners;
    std::map<std::string, uint32_t> g_ownerIds; // "car:<model>" / "track:<label>" -> g_owners index
    std::unordered_map<uint64_t, uint32_t> g_index; // window hash -> owner (kShared: in two owners' files)
    std::thread g_thread;
    uint64_t g_indexedFiles = 0, g_indexedWindows = 0, g_resolved[4] = {0, 0, 0, 0};

    // The names (the indexing thread only, once loaded).
    std::shared_ptr<gt4stock::GameNames> g_names;
    std::map<std::string, uint32_t> g_modelCar; // model code ("toyt0032") -> car code

    // The GS renderer thread's state.
    std::array<gt4texowner::Sample, kGsPages> g_pageSamples{};
    std::vector<uint32_t> g_curPages;
    gt4texowner::Sample g_cur;
    uint64_t g_curPos = 0, g_curNext = 0, g_curStep = kStep, g_uploads = 0;
    int g_curTries = 0;

    uint32_t ownerFor(gt4texowner::Owner::Kind kind, const std::string &id) // g_mutex held
    {
        const std::string key = (kind == gt4texowner::Owner::Car ? "car:" : kind == gt4texowner::Owner::Track ? "track:" : "wheels:") + id;
        const auto it = g_ownerIds.find(key);
        if (it != g_ownerIds.end())
            return it->second;
        OwnerRec r;
        r.kind = kind;
        r.id = id;
        g_owners.push_back(std::move(r));
        return g_ownerIds[key] = static_cast<uint32_t>(g_owners.size() - 1);
    }

    // The IOP thread (the disc's mutex held): queue the car and course files a read touched.
    void onDiscRead(uint64_t offset, uint64_t bytes)
    {
        if (!bytes)
            return;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_ready || g_stop)
            return;
        auto it = std::upper_bound(g_files.begin(), g_files.end(), offset, [](uint64_t o, const VolFile &f) { return o < f.begin; });
        if (it != g_files.begin())
            --it;
        bool queued = false;
        for (; it != g_files.end() && it->begin < offset + bytes; ++it)
        {
            if (it->end <= offset)
                continue;
            const std::string &p = it->path;
            const size_t slash = p.find('/');
            const std::string top = p.substr(0, slash);
            uint32_t owner = kNoOwner;
            if (top == "car")
            {
                std::string model = p.substr(p.rfind('/') + 1);
                if (const size_t dot = model.find('.'); dot != std::string::npos)
                    model.resize(dot);
                owner = ownerFor(gt4texowner::Owner::Car, model);
            }
            else if (top == "crs")
            {
                std::string course = p.substr(4);
                if (course.find('/') != std::string::npos)
                    continue; // crs/bestline/...
                if (const size_t dot = course.find('.'); dot != std::string::npos)
                    course.resize(dot);
                owner = ownerFor(gt4texowner::Owner::Track, course);
            }
            else // wheel/, tire/: shared (GT4 reads them after all of a race's cars)
                owner = ownerFor(gt4texowner::Owner::Wheels, "wheels");
            if (owner == kNoOwner || !g_queued.insert(p).second)
                continue;
            g_jobs.push_back(Job{p, owner});
            queued = true;
        }
        if (queued)
            g_wake.notify_one();
    }

    // A folder name Windows takes, ASCII: GT4's Latin letters without their accents, the characters a file name
    // cannot hold as spaces.
    std::string folderSafe(const std::string &utf8)
    {
        static const char *const kLatin1 = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        std::string s;
        for (size_t i = 0; i < utf8.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            if (c < 0x80)
            {
                s += (c < 0x20 || std::strchr("<>:\"/\\|?*", c)) ? ' ' : static_cast<char>(c);
                continue;
            }
            uint32_t cp = 0;
            if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size())
                cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(utf8[++i]) & 0x3Fu);
            else
                while (i + 1 < utf8.size() && (static_cast<unsigned char>(utf8[i + 1]) & 0xC0) == 0x80)
                    ++i;
            s += cp >= 0xC0 && cp <= 0xFF && kLatin1[cp - 0xC0] != '/' && kLatin1[cp - 0xC0] != 'x' ? kLatin1[cp - 0xC0] : ' ';
        }
        std::string t; // spaces closed up, none at the ends, no trailing dot
        for (char c : s)
            if (!(c == ' ' && (t.empty() || t.back() == ' ')))
                t += c;
        while (!t.empty() && (t.back() == ' ' || t.back() == '.'))
            t.pop_back();
        return t;
    }

    // The indexing thread: the car's / course's names, once.
    void nameOwner(OwnerRec &r)
    {
        if (r.hasName)
            return;
        r.hasName = true;
        r.named.kind = r.kind;
        std::string label = r.id, name;
        if (r.kind == gt4texowner::Owner::Car)
        {
            const auto it = g_modelCar.find(r.id);
            if (it != g_modelCar.end() && g_names && g_names->spec)
            {
                const auto &cars = g_names->spec->labels;
                const auto t = cars.find(0);
                if (t != cars.end())
                    if (const auto l = t->second.find(it->second); l != t->second.end())
                        label = l->second;
                name = g_names->carFullName(it->second);
            }
            r.named.key = "car:" + label;
        }
        else if (r.kind == gt4texowner::Owner::Track)
        {
            if (g_names)
                name = g_names->text("CourseName", r.id);
            r.named.key = "track:" + label;
        }
        else
        {
            name = "wheels";
            r.named.key = "wheels";
        }
        r.named.folder = folderSafe(name);
        if (r.named.folder.empty())
            r.named.folder = folderSafe(label);
        if (r.named.folder.empty())
            r.named.folder = "unnamed";
    }

    uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

    // The model code -> car table: VARIATIONamerican rows (word 0: the model's string id in american_StrDB) and
    // CAR_VARIATION_american (car -> its VARIATION code). The lowest car code wins when several share a model.
    void loadModels(const gt4save::Gt4Disc &disc)
    {
        const std::string dir = "specdb/GT4_US2560/";
        std::vector<uint8_t> file;
        std::string err;
        std::vector<std::string> strings;
        gt4stock::GtdbTable var, carVar;
        if (!disc.read(dir + "american_StrDB.sdb", file, err) || !gt4stock::decodeStrDb(file, strings, err) ||
            !disc.read(dir + "VARIATIONamerican.dbt", file, err) || !gt4stock::decodeGtdb(file, var, err) ||
            !disc.read(dir + "CAR_VARIATION_american.dbt", file, err) || !gt4stock::decodeGtdb(file, carVar, err))
        {
            std::fprintf(stderr, "[texture-dump] car names: %s (model codes used)\n", err.c_str());
            return;
        }
        std::map<uint32_t, std::string> varModel;
        for (size_t i = 0; i < var.codes.size(); ++i)
            if (var.rows[i].size() >= 4 && !varModel.count(var.codes[i]))
            {
                const uint32_t id = le32(var.rows[i].data());
                if (id < strings.size() && !strings[id].empty())
                    varModel[var.codes[i]] = strings[id];
            }
        for (size_t i = 0; i < carVar.codes.size(); ++i)
            if (carVar.rows[i].size() >= 8 && le32(carVar.rows[i].data() + 4) == 42)
                if (const auto m = varModel.find(le32(carVar.rows[i].data())); m != varModel.end())
                {
                    auto [it, added] = g_modelCar.emplace(m->second, carVar.codes[i]);
                    if (!added && carVar.codes[i] < it->second)
                        it->second = carVar.codes[i];
                }
    }

    void indexLoop()
    {
        std::string iso = PS2Runtime::getIoPaths().cdImage.string();
        std::string err;
        std::unique_ptr<gt4save::Gt4Disc> disc = iso.empty() ? nullptr : gt4save::Gt4Disc::open(PS2Runtime::getIoPaths().cdImage, err);
        if (!disc)
        {
            std::fprintf(stderr, "[texture-dump] no car / course folders: the disc image %s: %s\n", iso.c_str(), err.c_str());
            return;
        }
        g_names = gt4stock::GameNames::load(PS2Runtime::getIoPaths().cdImage, err);
        if (!g_names)
            std::fprintf(stderr, "[texture-dump] names: %s (labels used)\n", err.c_str());
        loadModels(*disc);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &[path, f] : disc->files())
            {
                const std::string top = path.substr(0, path.find('/'));
                if (top == "car" || top == "wheel" || top == "tire" || top == "crs")
                    g_files.push_back(VolFile{f.lba * 2048u, f.lba * 2048u + std::max<uint64_t>(f.stored, 1), path});
            }
            std::sort(g_files.begin(), g_files.end(), [](const VolFile &a, const VolFile &b) { return a.begin < b.begin; });
            g_ready = true;
        }
        ps2x::iop::gt4::setDiscReadObserver(onDiscRead);
        std::fprintf(stderr, "[texture-dump] car / course folders: watching %zu files of %s (%zu car models named)\n", g_files.size(),
                     iso.c_str(), g_modelCar.size());
        for (;;)
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_busy = false;
                g_idle.notify_all();
                g_wake.wait(lock, [] { return g_stop || !g_jobs.empty(); });
                if (g_stop)
                    return;
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
                g_busy = true;
            }
            std::vector<uint8_t> data;
            std::string why;
            if (!disc->read(job.path, data, why))
            {
                std::fprintf(stderr, "[texture-dump] %s: %s\n", job.path.c_str(), why.c_str());
                continue;
            }
            std::vector<uint64_t> hashes;
            hashes.reserve(data.size() / kStep);
            for (size_t o = 0; o + kWindow <= data.size(); o += kStep)
                if (!uninformative(data.data() + o))
                    hashes.push_back(windowHash(data.data() + o));
            std::lock_guard<std::mutex> lock(g_mutex);
            OwnerRec &r = g_owners[job.owner];
            nameOwner(r);
            for (const uint64_t h : hashes)
            {
                auto [it, added] = g_index.emplace(h, job.owner);
                if (!added && it->second != job.owner)
                    it->second = kShared;
            }
            ++g_indexedFiles;
            g_indexedWindows += hashes.size();
            std::fprintf(stderr, "[texture-dump] owner %s (%s): %s, %zu bytes, %zu windows\n", r.named.key.c_str(), r.named.folder.c_str(),
                         job.path.c_str(), data.size(), hashes.size());
        }
    }
}

void gt4texowner::start()
{
    static const bool started = [] {
        g_thread = std::thread(indexLoop);
        return true;
    }();
    (void)started;
}

void gt4texowner::beginUpload(const std::vector<uint32_t> &pages, uint64_t bytes)
{
    g_curPages = pages;
    g_cur = Sample{};
    g_curPos = 0;
    g_curNext = 0;
    g_curTries = 0;
    g_curStep = std::max<uint64_t>(kStep, (bytes / kSamples) & ~uint64_t(kStep - 1));
    ++g_uploads;
    for (const uint32_t page : g_curPages)
        g_pageSamples[page % kGsPages] = g_cur;
}

void gt4texowner::uploadData(const uint8_t *data, uint32_t bytes)
{
    if (!data || g_curPages.empty())
        return;
    const uint64_t end = g_curPos + bytes;
    const uint8_t before = g_cur.count;
    while (g_cur.count < kSamples && g_curNext + kWindow <= end)
    {
        if (g_curNext < g_curPos)
            g_curNext = g_curPos; // (never: steps are 16-byte multiples and data comes in whole qwords)
        const uint8_t *w = data + (g_curNext - g_curPos);
        if (!uninformative(w))
        {
            g_cur.hash[g_cur.count++] = windowHash(w);
            g_curNext = uint64_t(g_cur.count) * g_curStep;
            g_curTries = 0;
        }
        else if (++g_curTries < 8)
            g_curNext += kStep;
        else
        {
            g_curTries = 0;
            g_curNext = (g_curNext / g_curStep + 1) * g_curStep;
        }
    }
    if (g_curNext < end && g_curNext + kWindow > end)
        g_curNext = end; // a window across two pieces of the data: the next piece's start
    g_curPos = end;
    if (g_cur.count != before)
        for (const uint32_t page : g_curPages)
            g_pageSamples[page % kGsPages] = g_cur;
}

gt4texowner::Sample gt4texowner::sampleAt(uint32_t tbp) { return g_pageSamples[(tbp >> 5) % kGsPages]; }

gt4texowner::Owner gt4texowner::resolve(const Sample &sample)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_idle.wait_for(lock, std::chrono::seconds(2), [] { return g_stop || (!g_busy && g_jobs.empty()); });
    std::map<uint32_t, int> votes;
    for (int i = 0; i < sample.count; ++i)
    {
        const auto it = g_index.find(sample.hash[i]);
        if (it != g_index.end() && it->second < g_owners.size())
            ++votes[it->second];
    }
    uint32_t best = kNoOwner;
    int most = 0;
    for (const auto &[owner, n] : votes)
        if (n > most)
        {
            best = owner;
            most = n;
        }
    if (best == kNoOwner || !g_owners[best].hasName)
    {
        ++g_resolved[0];
        return Owner{};
    }
    ++g_resolved[g_owners[best].kind];
    return g_owners[best].named;
}

void gt4texowner::shutdown()
{
    ps2x::iop::gt4::setDiscReadObserver(nullptr);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    g_idle.notify_all();
    if (g_thread.joinable())
    {
        g_thread.join();
        std::fprintf(stderr, "[texture-dump] owners: %zu cars / courses, %llu files indexed (%llu windows); %llu uploads sampled; pictures: %llu car, %llu course, %llu wheels and tyres, %llu other\n",
                     g_owners.size(), static_cast<unsigned long long>(g_indexedFiles), static_cast<unsigned long long>(g_indexedWindows),
                     static_cast<unsigned long long>(g_uploads), static_cast<unsigned long long>(g_resolved[Owner::Car]),
                     static_cast<unsigned long long>(g_resolved[Owner::Track]), static_cast<unsigned long long>(g_resolved[Owner::Wheels]),
                     static_cast<unsigned long long>(g_resolved[0]));
    }
}
#else
void gt4texowner::start() {}
void gt4texowner::beginUpload(const std::vector<uint32_t> &, uint64_t) {}
void gt4texowner::uploadData(const uint8_t *, uint32_t) {}
gt4texowner::Sample gt4texowner::sampleAt(uint32_t) { return Sample{}; }
gt4texowner::Owner gt4texowner::resolve(const Sample &) { return Owner{}; }
void gt4texowner::shutdown() {}
#endif
