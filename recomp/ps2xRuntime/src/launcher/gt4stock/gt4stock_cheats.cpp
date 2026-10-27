#include "gt4stock_cheats.h"

#include "gt4stock_util.h"

#include "raylib.h" // CompressData (raw DEFLATE, raylib's sdefl), as the backups' zips use it

#include <algorithm>
#include <cstring>

namespace gt4stock
{
    // ------------------------------------------------------------------ the cards
    CheatStatus cheatStatus(const gt4cheats::Code &code, const CheatSettings &s, long long elapsed, bool player2Ready)
    {
        if (code.controller2)
            return s.secret ? CheatStatus::ReadyController1 : player2Ready ? CheatStatus::ReadyController2 : CheatStatus::NeedsController2;
        if (!code.dayRule)
            return CheatStatus::Ready;
        if (elapsed >= 0 && dayRuleMet(elapsed))
            return CheatStatus::Ready;
        return s.noday ? CheatStatus::ReadySkipped : CheatStatus::NeedsDay;
    }

    const char *cheatStatusText(CheatStatus st)
    {
        switch (st)
        {
        case CheatStatus::NeedsDay: return "Needs Day 367";
        case CheatStatus::ReadySkipped: return "Ready: wait skipped";
        case CheatStatus::Ready: return "Ready";
        case CheatStatus::NeedsController2: return "Needs controller 2";
        case CheatStatus::ReadyController2: return "Ready: on controller 2";
        case CheatStatus::ReadyController1: return "Ready: controller 1 can type it";
        }
        return "";
    }

    bool cheatStatusReady(CheatStatus st) { return st != CheatStatus::NeedsDay && st != CheatStatus::NeedsController2; }

    namespace
    {
        bool isMission(const gt4cheats::Code &c) { return c.id == "mission"; }
    }

    CheatSettings cheatsInEffect(const CheatSettings &s, const ExeFeatures &gt4)
    {
        CheatSettings e = s;
        if (!gt4.cheats)
        {
            e.noday = e.secret = e.missionfix = false;
            e.otherWords.clear();
        }
        if (!gt4.missionfix)
            e.missionfix = false;
        if (!gt4.card)
            e.card = e.cardAuto = false;
        return e;
    }

    std::string cheatEffect(const gt4cheats::Code &code, const CheatSettings &s)
    {
        if (isMission(code) && s.missionfix)
            return "Passes the highlighted mission (our fix). No money.";
        if (isMission(code))
            return "Gives 10,000,000 credits, probably by mistake: we think it was meant to pass a mission.";
        return std::string(code.effect);
    }

    std::string cheatDays(const gt4cheats::Code &code, const CheatSettings &s)
    {
        if (isMission(code) && s.missionfix)
            return "Adds 1 day";
        return std::string(code.days);
    }

    std::string cheatWhere(const gt4cheats::Code &code, const CheatSettings &s)
    {
        if (isMission(code))
            return s.missionfix ? "In the Mission Hall, with the mission highlighted." : "In the Mission Hall.";
        if (code.controller2)
            return s.secret ? "On the Options screen, typed on controller 1 or 2." : "On the Options screen, typed on controller 2.";
        return std::string(code.where);
    }

    std::string cheatScreen(const gt4cheats::Code &code)
    {
        if (code.id == "credits")
            return "GT Mode map screen";
        if (code.id == "licence")
            return "Licence Center, a licence highlighted";
        if (code.id == "test")
            return "A licence's test list, a test highlighted";
        if (code.id == "event")
            return "An event's race list, a race highlighted";
        if (code.id == "mission")
            return "Mission Hall";
        return "Options screen, controller 2";
    }

    std::string cheatController(const gt4cheats::Code &code, const CheatSettings &s)
    {
        if (code.controller2)
            return s.secret ? "Controller 1 or 2" : "Controller 2";
        return "Any controller";
    }

    std::string cheatHistory(const gt4cheats::Code &code, const CheatSettings &)
    {
        if (code.id == "credits")
            return std::string(code.history) + " He tried it on a save moved past 365 days.";
        if (code.id == "options")
            return std::string(code.history) + " On the USA disc it really ignores controller 1.";
        if (code.id == "event")
            return std::string(code.history) + " Each race still needs the code on its own for the game %.";
        return std::string(code.history);
    }

    std::string cheatSheetDoes(const gt4cheats::Code &code, const CheatSettings &s)
    {
        std::string d = "Does: " + (code.controller2 ? std::string(code.effect) : cheatEffect(code, s));
        if (isMission(code) && s.missionfix)
            d += " Its Clear mark shows the next time you open the Mission Hall. Keep a free garage slot: with a full garage a group's prize "
                 "car is lost.";
        return d;
    }

    std::string cheatNote(const gt4cheats::Code &code, const CheatSettings &s)
    {
        if (isMission(code) && s.missionfix)
            return "The fix copies GT4's own race code step for step: a gold Clear mark, game %, and the group's prize car once all its "
                   "missions are passed.";
        return std::string(code.note);
    }

    std::string cheatShortName(const gt4cheats::Code &code, const CheatSettings &s)
    {
        if (isMission(code) && s.missionfix)
            return "Mission Hall (our fix)"; // the shared table's "Mission Hall (10M)" is the code as shipped
        return std::string(code.shortName);
    }

    bool cheatChangesSave(const gt4cheats::Code &) { return true; }

    std::string cheatButtonName(uint16_t button) { return std::string(gt4cheats::buttonName(button)); }

    std::vector<const gt4cheats::Code *> tickedCodes(const CheatSettings &s)
    {
        std::vector<const gt4cheats::Code *> out;
        for (const std::string &id : s.ticked)
            if (const gt4cheats::Code *c = gt4cheats::find(id); c && std::find(out.begin(), out.end(), c) == out.end())
                out.push_back(c);
        return out;
    }

    bool isTicked(const CheatSettings &s, std::string_view id) { return std::find(s.ticked.begin(), s.ticked.end(), id) != s.ticked.end(); }

    void setTicked(CheatSettings &s, std::string_view id, bool on)
    {
        // The order is kept (a hand-written GT4_CHEAT_CARD_LIST's too); a new tick goes last.
        const auto at = std::find(s.ticked.begin(), s.ticked.end(), id);
        if (on && at == s.ticked.end() && gt4cheats::find(id))
            s.ticked.emplace_back(id);
        else if (!on && at != s.ticked.end())
            s.ticked.erase(at);
    }

    std::vector<std::string> cheatSwitchesOn(const CheatSettings &s)
    {
        std::vector<std::string> on;
        if (s.noday)
            on.push_back("365-day wait skipped");
        if (s.secret)
            on.push_back("controller 1 types the hidden code");
        if (s.missionfix)
            on.push_back("Mission Hall fix");
        if (s.card)
            on.push_back(s.key + " cheat card");
        for (const std::string &w : s.otherWords)
            on.push_back(w);
        return on;
    }

    std::string cheatCardFileName(const gt4cheats::Code &code, const CheatSettings &s) { return "GT4 cheat card - " + cheatShortName(code, s) + ".png"; }
    const char *cheatSheetFileName() { return "GT4 cheat sheet.pdf"; }

    fs::path uniqueFile(const fs::path &dir, const std::string &name)
    {
        std::error_code ec;
        fs::path p = dir / pathFrom(name);
        if (!fs::exists(p, ec))
            return p;
        const fs::path stem = pathFrom(name).stem(), ext = pathFrom(name).extension();
        for (int n = 2; n < 1000; ++n)
        {
            p = dir / (stem.wstring() + L" (" + std::to_wstring(n) + L")" + ext.wstring());
            if (!fs::exists(p, ec))
                return p;
        }
        return p;
    }

    Paper paperFor(bool letter)
    {
        if (letter)
            return Paper{2550, 3300, 612.0f, 792.0f, "Letter"};
        return Paper{2480, 3508, 595.28f, 841.89f, "A4"};
    }

    // ------------------------------------------------------------------ PDF
    uint32_t adler32(const uint8_t *data, size_t size)
    {
        uint32_t a = 1, b = 0;
        size_t i = 0;
        while (i < size)
        {
            const size_t n = std::min<size_t>(size - i, 5552);
            for (size_t k = 0; k < n; ++k)
            {
                a += data[i + k];
                b += a;
            }
            a %= 65521u;
            b %= 65521u;
            i += n;
        }
        return (b << 16) | a;
    }

    std::vector<uint8_t> zlibStream(const uint8_t *data, size_t size)
    {
        std::vector<uint8_t> out{0x78, 0x9c};
        int packedSize = 0;
        unsigned char *packed = CompressData(data, static_cast<int>(size), &packedSize);
        if (packed && packedSize > 0)
            out.insert(out.end(), packed, packed + packedSize);
        if (packed)
            MemFree(packed);
        const uint32_t ad = adler32(data, size);
        out.push_back(static_cast<uint8_t>(ad >> 24));
        out.push_back(static_cast<uint8_t>(ad >> 16));
        out.push_back(static_cast<uint8_t>(ad >> 8));
        out.push_back(static_cast<uint8_t>(ad));
        return out;
    }

    std::vector<uint8_t> makePdf(const std::vector<PdfPage> &pages, const std::string &title)
    {
        // Objects: 1 catalog, 2 pages, 3 info, then per page: page, its content stream, its image.
        std::vector<uint8_t> out;
        std::vector<size_t> offsets;
        auto put = [&](const std::string &s) { out.insert(out.end(), s.begin(), s.end()); };
        auto putBytes = [&](const std::vector<uint8_t> &b) { out.insert(out.end(), b.begin(), b.end()); };
        auto begin = [&](size_t id) {
            if (offsets.size() < id + 1)
                offsets.resize(id + 1, 0);
            offsets[id] = out.size();
            put(strf("%zu 0 obj\n", id));
        };
        auto pdfText = [](const std::string &t) {
            std::string r = "(";
            for (char c : t)
            {
                if (c == '(' || c == ')' || c == '\\')
                    r += '\\';
                r += (static_cast<unsigned char>(c) < 0x80) ? c : '?';
            }
            return r + ")";
        };
        put("%PDF-1.4\n%\xe2\xe3\xcf\xd3\n");
        const size_t n = pages.size();
        begin(1);
        put("<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
        begin(2);
        std::string kids;
        for (size_t i = 0; i < n; ++i)
            kids += strf("%zu 0 R ", 4 + 3 * i);
        put("<< /Type /Pages /Kids [ " + kids + "] /Count " + std::to_string(n) + " >>\nendobj\n");
        begin(3);
        put("<< /Title " + pdfText(title) + " /Producer (GT4 Recompilation launcher) >>\nendobj\n");
        for (size_t i = 0; i < n; ++i)
        {
            const PdfPage &p = pages[i];
            const size_t pageId = 4 + 3 * i, contentId = pageId + 1, imageId = pageId + 2;
            begin(pageId);
            put(strf("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /XObject << /Im%zu %zu 0 R >> >> /Contents %zu 0 R >>\nendobj\n",
                     p.widthPt, p.heightPt, i + 1, imageId, contentId));
            const std::string draw = strf("q %.2f 0 0 %.2f 0 0 cm /Im%zu Do Q\n", p.widthPt, p.heightPt, i + 1);
            begin(contentId);
            put(strf("<< /Length %zu >>\nstream\n", draw.size()));
            put(draw);
            put("endstream\nendobj\n");
            const std::vector<uint8_t> z = zlibStream(p.rgb.data(), p.rgb.size());
            begin(imageId);
            put(strf("<< /Type /XObject /Subtype /Image /Width %d /Height %d /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length %zu >>\nstream\n",
                     p.width, p.height, z.size()));
            putBytes(z);
            put("\nendstream\nendobj\n");
        }
        const size_t xref = out.size();
        put(strf("xref\n0 %zu\n", offsets.size()));
        put("0000000000 65535 f \n");
        for (size_t id = 1; id < offsets.size(); ++id)
            put(strf("%010zu 00000 n \n", offsets[id]));
        put(strf("trailer\n<< /Size %zu /Root 1 0 R /Info 3 0 R >>\nstartxref\n%zu\n%%%%EOF\n", offsets.size(), xref));
        return out;
    }

    // ------------------------------------------------------------------ the player's buttons
    uint16_t keyboardWord(const KeyboardKeys &k)
    {
        uint16_t w = 0;
        if (k.up)
            w |= gt4cheats::Up;
        if (k.down)
            w |= gt4cheats::Down;
        if (k.left)
            w |= gt4cheats::Left;
        if (k.right)
            w |= gt4cheats::Right;
        if (k.tab)
            w |= gt4cheats::Select;
        if (k.enter)
            w |= gt4cheats::Start;
        if (k.q)
            w |= gt4cheats::L1;
        if (k.e)
            w |= gt4cheats::R1;
        if (k.lshift)
            w |= gt4cheats::L2;
        if (k.rshift)
            w |= gt4cheats::R2;
        if (k.x)
            w |= gt4cheats::Cross;
        if (k.c)
            w |= gt4cheats::Circle;
        if (k.z)
            w |= gt4cheats::Square;
        if (k.v)
            w |= gt4cheats::Triangle;
        return w;
    }

    std::string padNamesLine(const PadSettings &pad, bool ps)
    {
        // The buttons the codes use: Select, Start, L1, R1, L2, R2 (the d-pad is the d-pad).
        auto name = [&](int button) {
            const int b = pad.buttons[static_cast<size_t>(button)];
            const int p = pressureOfButton(button);
            const int a = p >= 0 ? pad.pressure[static_cast<size_t>(p)] : -1;
            if (b > 0)
                return std::string(gamepadButtonName(b, ps));
            if (a >= 0)
                return std::string(gamepadAxisName(a, ps));
            return std::string("(none)");
        };
        // kPadButtonKeys order: 0 select, 3 start, 8 l2, 9 r2, 10 l1, 11 r1.
        std::string l2 = name(8), r2 = name(9);
        const auto trig = [](std::string s) {
            // "Left trigger (LT)" -> "LT"
            const size_t a = s.find('('), b = s.find(')');
            return a != std::string::npos && b != std::string::npos && b > a ? s.substr(a + 1, b - a - 1) : s;
        };
        return name(0) + " = Select, " + name(3) + " = Start, " + name(10) + " / " + name(11) + " = L1 / R1, " + trig(l2) + " / " + trig(r2) + " = L2 / R2";
    }
}
