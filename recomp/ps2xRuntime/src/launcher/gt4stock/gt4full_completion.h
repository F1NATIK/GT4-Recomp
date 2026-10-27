#pragma once

// GT4's GT Mode "game completion rate" (Home "Complete", Game Status "Achievement"), worked out from a save the way
// GT4 does (analysis\design\game-completion\COMPLETION-RATE.md; gt4_completion.py is the Python reference).
//
// GT4 stores no percentage: gtmode.adc EventData::getRaceEventRecordStatistics (0x012d81) counts, over every race of
// every event in EventData::AllEvents (each event's sub-races: the code without its last two digits + 01, 02, ...
// while RACE has that label; a championship's own code once more) and the missions l0m0001.. (while getUnit finds
// them), the entries whose best result is 1st, and keeps win * 1000 / total (whole numbers: the remainder dropped).
// It shows that per-mille figure with one decimal ("%.1t" + "%": 6 -> "0.6%"). The prize table's "achievement" rows
// (25 / 50 / 100) give their car at perMille >= detail * 10; the Endurance hall opens at 250 (canEntryEndurance).
//
// The event list is read from the player's disc (projects/gtmode/gtmode.adc) at run time; nothing is compiled in.
// Header only (inline), so no build file needs it listed.

#include "gt4full_model.h"
#include "gt4save.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace gt4stock
{
    struct CompletionList
    {
        std::vector<std::string> labels; // every entry GT4 counts, in its order (589 on the US disc)
        std::vector<int> rows;           // the RaceRecord row of each (getUnit's lookup), -1 when none
    };

    namespace completion_detail
    {
        // The launcher's scriptTokens (gt4full_model.cpp), repeated here so this header stands alone.
        inline std::vector<std::string> tokens(const std::vector<uint8_t> &d)
        {
            std::vector<std::string> out;
            size_t i = 0;
            const size_t n = d.size();
            while (i + 2 < n)
            {
                const size_t len = d[i] | (d[i + 1] << 8);
                if (len >= 1 && len <= 200 && i + 2 + len <= n)
                {
                    bool printable = true;
                    for (size_t k = 0; k < len && printable; ++k)
                        printable = d[i + 2 + k] >= 32 && d[i + 2 + k] < 127;
                    if (printable)
                    {
                        out.emplace_back(reinterpret_cast<const char *>(d.data() + i + 2), len);
                        i += 2 + len;
                        continue;
                    }
                }
                ++i;
            }
            return out;
        }
        inline bool endsWith(const std::string &s, const char *e)
        {
            const size_t n = std::char_traits<char>::length(e);
            return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
        }
        inline int twoDigits(const std::string &s, size_t at)
        {
            if (at + 2 > s.size() || s[at] < '0' || s[at] > '9' || s[at + 1] < '0' || s[at + 1] > '9')
                return -1;
            return (s[at] - '0') * 10 + (s[at + 1] - '0');
        }
    }

    // The race codes of EventData::AllEvents, in its order, from gtmode.adc's string tokens: from the first
    // "BeginnerHallEvents" to the first "AllEvents" after it, a "...Events" token starts a list and a race code is the
    // token after an "EventInfo" token (a symbol repeats: STATIC_DEFINE name, VARIABLE_PUSH name list); the list order is
    // the run of "...Events" tokens from that "AllEvents" (lists only, repeats once). US disc: 38 lists, 143 codes
    // (checked against an instruction walk: gt4_completion.py tokens). Empty when the file does not look like that.
    inline std::vector<std::string> completionEventCodes(const std::vector<uint8_t> &gtmodeAdc)
    {
        using namespace completion_detail;
        const std::vector<std::string> t = tokens(gtmodeAdc);
        size_t start = 0;
        while (start < t.size() && t[start] != "BeginnerHallEvents")
            ++start;
        size_t end = start;
        while (end < t.size() && t[end] != "AllEvents")
            ++end;
        if (start >= t.size() || end >= t.size())
            return {};
        std::map<std::string, std::vector<std::string>> lists;
        std::string cur;
        for (size_t k = start; k < end; ++k)
        {
            const std::string &x = t[k];
            if (endsWith(x, "Events"))
            {
                cur = x;
                lists[cur];
            }
            else if (x != "EventInfo" && k > 0 && t[k - 1] == "EventInfo" && !cur.empty())
                lists[cur].push_back(x);
        }
        std::vector<std::string> out;
        std::string last;
        for (size_t k = end; k < t.size() && endsWith(t[k], "Events") && (t[k] == "AllEvents" || lists.count(t[k])); ++k)
        {
            if (t[k] == "AllEvents" || t[k] == last)
                continue;
            last = t[k];
            for (const std::string &c : lists[t[k]])
                out.push_back(c);
        }
        return out;
    }

    // Every entry GT4 counts: each code's sub-races (DealerData::extractSubRaceCode 0x01e269, MRaceData::available =
    // the exact RACE label), plus a championship's own code (isChampionship 0x01dc10: series count > 0, index 00),
    // then the missions l0m0001.. while getUnit (label, "us"+label, "jp"+label) finds them.
    inline CompletionList completionList(const std::vector<std::string> &eventCodes, const gt4save::SpecDb &db)
    {
        using namespace completion_detail;
        CompletionList l;
        if (eventCodes.empty())
            return l;
        std::set<std::string> race;
        const auto t = db.labels.find(35);
        if (t != db.labels.end())
            for (const auto &kv : t->second)
                race.insert(kv.second);
        // getUnit's row lookup, once per label (SpecDb::raceRow searches the rows one by one).
        std::map<uint32_t, int> rowOfCode;
        for (size_t i = 0; i < db.raceRows.size(); ++i)
            rowOfCode.emplace(db.raceRows[i], static_cast<int>(i));
        std::map<std::string, uint32_t> codeOf;
        if (t != db.labels.end())
            for (const auto &kv : t->second)
                codeOf[kv.second] = kv.first; // (a later duplicate replaces, as SpecDb does)
        auto rowOf = [&](const std::string &label) {
            for (const std::string &cand : {label, "us" + label, "jp" + label})
            {
                const auto c = codeOf.find(cand);
                if (c == codeOf.end())
                    continue;
                const auto r = rowOfCode.find(c->second);
                if (r != rowOfCode.end())
                    return r->second;
            }
            return -1;
        };
        auto add = [&](const std::string &label) {
            l.labels.push_back(label);
            l.rows.push_back(rowOf(label));
        };
        for (const std::string &code : eventCodes)
        {
            std::vector<std::string> sub;
            if (code.size() >= 2)
            {
                const std::string base = code.substr(0, code.size() - 2);
                for (int i = 1; i < 99; ++i)
                {
                    const std::string lab = base + static_cast<char>('0' + i / 10) + static_cast<char>('0' + i % 10);
                    if (!race.count(lab))
                        break;
                    sub.push_back(lab);
                }
                if (sub.empty() && race.count(code))
                    sub.push_back(code);
            }
            const int count = code.size() >= 6 ? twoDigits(code, code.size() - 4) : -1, index = code.size() >= 6 ? twoDigits(code, code.size() - 2) : -1;
            if (count > 0 && index == 0)
                sub.push_back(code);
            for (const std::string &s : sub)
                add(s);
        }
        for (int i = 1; i < 99; ++i)
        {
            char lab[16];
            std::snprintf(lab, sizeof lab, "l0m%04d", i);
            if (rowOf(lab) < 0)
                break;
            add(lab);
        }
        return l;
    }

    // The list for a disc image, read once per image and kept (thread-safe). nullptr when gtmode.adc or its event lists
    // cannot be read: the launcher then hides the figure.
    // isoUtf8: the image's path as UTF-8 (GameNames::source).
    inline std::shared_ptr<const CompletionList> completionListForDisc(const std::string &isoUtf8, const gt4save::SpecDb &db)
    {
        static std::mutex m;
        static std::map<std::string, std::shared_ptr<const CompletionList>> cache;
        const fs::path iso(std::u8string(isoUtf8.begin(), isoUtf8.end()));
        const std::string key = isoUtf8 + "|" + std::to_string(reinterpret_cast<uintptr_t>(&db));
        std::lock_guard<std::mutex> lock(m);
        const auto it = cache.find(key);
        if (it != cache.end())
            return it->second;
        std::shared_ptr<const CompletionList> out;
        std::string err;
        if (std::unique_ptr<gt4save::Gt4Disc> disc = gt4save::Gt4Disc::open(iso, err))
        {
            std::vector<uint8_t> f;
            if (disc->read("projects/gtmode/gtmode.adc", f, err))
            {
                CompletionList l = completionList(completionEventCodes(f), db);
                if (!l.labels.empty())
                    out = std::make_shared<const CompletionList>(std::move(l));
            }
        }
        cache[key] = out;
        return out;
    }

    // A save's completion (GT4's sum; "unit.result == '1'" is the best result, bits 4-7, at 0 = 1st).
    inline Completion completionOf(const gt4save::Save &save, const CompletionList &list)
    {
        Completion c;
        if (!save.loaded || list.labels.empty())
            return c;
        std::map<int, uint32_t> raw; // rows not listed are untouched (best 15: none)
        for (const gt4save::RaceUnit &u : save.fields.raceRecord)
            raw[u.row] = u.raw;
        for (int row : list.rows)
        {
            ++c.total;
            const auto it = row >= 0 ? raw.find(row) : raw.end();
            if (it != raw.end() && ((it->second >> 4) & 15u) == 0)
                ++c.won;
        }
        c.perMille = c.won * 1000 / c.total;
        c.known = true;
        return c;
    }
}
