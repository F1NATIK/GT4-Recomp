// GT4 Recompilation launcher: the "How it works" text: its parser, About's "Is it a full
// recompilation?" answer and the page's layout (gt4stock_doc.h).

#include "gt4stock_doc.h"

#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace gt4stock
{
    // ------------------------------------------------------------------ inline marks
    std::vector<DocSpan> parseInline(const std::string &text)
    {
        std::vector<DocSpan> out;
        std::string cur;
        bool bold = false, code = false;
        auto flush = [&]() {
            if (!cur.empty())
                out.push_back(DocSpan{cur, bold, code});
            cur.clear();
        };
        for (size_t i = 0; i < text.size(); ++i)
        {
            // A mark counts only when it is closed later (or closes an open one).
            if (!code && text.compare(i, 2, "**") == 0 && (bold || text.find("**", i + 2) != std::string::npos))
            {
                flush();
                bold = !bold;
                ++i;
                continue;
            }
            if (text[i] == '`' && (code || text.find('`', i + 1) != std::string::npos))
            {
                flush();
                code = !code;
                continue;
            }
            cur += text[i];
        }
        flush();
        return out;
    }

    namespace
    {
        std::string spansText(const std::vector<DocSpan> &spans)
        {
            std::string s;
            for (const DocSpan &x : spans)
                s += x.text;
            return s;
        }

        std::string rtrim(const std::string &s)
        {
            size_t n = s.size();
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
                --n;
            return s.substr(0, n);
        }

        bool tableSeparator(const std::string &t)
        {
            bool dash = false;
            for (char c : t)
            {
                if (c == '-')
                    dash = true;
                else if (c != '|' && c != ':' && c != ' ')
                    return false;
            }
            return dash;
        }

        std::vector<std::string> tableCells(const std::string &t)
        {
            std::string s = trim(t);
            if (startsWith(s, "|"))
                s.erase(0, 1);
            if (endsWith(s, "|"))
                s.pop_back();
            std::vector<std::string> cells;
            for (const std::string &c : split(s, '|'))
                cells.push_back(trim(c));
            return cells;
        }

        // "3. text": a number of up to 3 digits, a dot and a space.
        bool numbered(const std::string &t, int &n, std::string &rest)
        {
            size_t i = 0;
            while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])))
                ++i;
            if (i == 0 || i > 3 || i + 1 >= t.size() || t[i] != '.' || t[i + 1] != ' ')
                return false;
            n = std::stoi(t.substr(0, i));
            rest = trim(t.substr(i + 2));
            return true;
        }

        // The "What's inside a PS2" text diagram: drawn as boxes on the page.
        bool isPs2Diagram(const std::vector<std::string> &lines)
        {
            const std::string all = lower(join(lines, "\n"));
            return all.find("emotion engine") != std::string::npos && all.find("graphics synthesizer") != std::string::npos &&
                   all.find("(iop)") != std::string::npos;
        }
    }

    // ------------------------------------------------------------------ the parser
    const DocSection *Doc::find(const std::string &heading) const
    {
        for (const DocSection &s : sections)
            if (iequals(s.heading, heading) || iequals(s.title, heading))
                return &s;
        return nullptr;
    }

    bool Doc::isLead(const DocSection &s) const { return iequals(s.title, "About") || iequals(s.title, "Short version"); }

    std::string Doc::shortVersion() const
    {
        for (const DocSection &s : sections)
            if (isLead(s))
            {
                std::vector<std::string> parts;
                for (const DocBlock &b : s.blocks)
                    if (b.kind == DocKind::Paragraph)
                        parts.push_back(b.text);
                return join(parts, " ");
            }
        return std::string();
    }

    std::string Doc::plainText() const
    {
        std::string out = title + "\n";
        for (const DocSection &s : sections)
        {
            out += "\n" + s.heading + "\n";
            for (const DocBlock &b : s.blocks)
            {
                switch (b.kind)
                {
                case DocKind::Bullet: out += "- " + b.text + "\n"; break;
                case DocKind::Numbered: out += std::to_string(b.number) + ". " + b.text + "\n"; break;
                case DocKind::Table:
                    for (const auto &r : b.rows)
                        out += join(r, " | ") + "\n";
                    break;
                case DocKind::Code: out += join(b.lines, "\n") + "\n"; break;
                default: out += b.text + "\n"; break;
                }
            }
        }
        return out;
    }

    Doc parseDoc(const std::string &md)
    {
        Doc doc;
        DocSection *sec = nullptr;
        DocBlock cur;
        bool open = false, fence = false;
        auto flush = [&]() {
            if (open && (cur.kind == DocKind::Table ? !cur.rows.empty() : cur.kind == DocKind::Code ? !cur.lines.empty() : !trim(cur.text).empty()))
            {
                if (cur.kind != DocKind::Table && cur.kind != DocKind::Code)
                {
                    cur.spans = parseInline(trim(cur.text));
                    cur.text = spansText(cur.spans);
                }
                if (!sec)
                {
                    doc.sections.push_back(DocSection{});
                    sec = &doc.sections.back();
                }
                sec->blocks.push_back(cur);
            }
            cur = DocBlock{};
            open = false;
        };
        auto start = [&](DocKind k, const std::string &text) {
            flush();
            cur.kind = k;
            cur.text = text;
            open = true;
        };
        for (std::string line : split(md, '\n'))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            const std::string t = trim(line);
            if (fence)
            {
                if (startsWith(t, "```"))
                {
                    fence = false;
                    cur.diagram = isPs2Diagram(cur.lines);
                    flush();
                }
                else
                    cur.lines.push_back(rtrim(line));
                continue;
            }
            if (startsWith(t, "```"))
            {
                start(DocKind::Code, std::string());
                fence = true;
                continue;
            }
            if (t.empty())
            {
                flush();
                continue;
            }
            if (startsWith(t, "### "))
            {
                start(DocKind::Heading, trim(t.substr(4)));
                flush();
                continue;
            }
            if (startsWith(t, "## "))
            {
                flush();
                DocSection s;
                s.heading = trim(t.substr(3));
                int n = 0;
                std::string rest;
                if (numbered(s.heading, n, rest))
                {
                    s.number = std::to_string(n);
                    s.title = rest;
                }
                else
                    s.title = s.heading;
                doc.sections.push_back(s);
                sec = &doc.sections.back();
                continue;
            }
            if (startsWith(t, "# "))
            {
                flush();
                doc.title = trim(t.substr(2));
                continue;
            }
            if (startsWith(t, "|"))
            {
                if (!(open && cur.kind == DocKind::Table))
                    start(DocKind::Table, std::string());
                if (!tableSeparator(t))
                    cur.rows.push_back(tableCells(t));
                continue;
            }
            if (startsWith(t, ">"))
            {
                const std::string q = trim(t.substr(1));
                if (!(open && cur.kind == DocKind::Quote))
                    start(DocKind::Quote, q);
                else if (!q.empty())
                    cur.text += " " + q;
                continue;
            }
            int n = 0;
            std::string rest;
            if (startsWith(t, "- ") || startsWith(t, "* "))
                start(DocKind::Bullet, trim(t.substr(2)));
            else if (numbered(t, n, rest))
            {
                start(DocKind::Numbered, rest);
                cur.number = n;
            }
            else if (open && (cur.kind == DocKind::Paragraph || cur.kind == DocKind::Bullet || cur.kind == DocKind::Numbered || cur.kind == DocKind::Quote))
                cur.text += " " + t; // a wrapped line continues the block
            else
                start(DocKind::Paragraph, t);
        }
        flush();
        return doc;
    }

    // ------------------------------------------------------------------ About's answer card
    RecompAnswer recompAnswer(const Doc &doc)
    {
        RecompAnswer a;
        const DocSection *s = doc.find("Is it a full recompilation?");
        if (!s)
            return a;
        a.question = s->title;
        const DocBlock *b = nullptr;
        for (const DocBlock &x : s->blocks)
            if (x.kind == DocKind::Quote || x.kind == DocKind::Paragraph)
            {
                b = &x;
                break;
            }
        if (!b)
            return a;
        a.answer = b->text;
        if (!b->spans.empty() && b->spans[0].bold)
            a.headline = trim(b->spans[0].text);
        // "All 27,350 functions ...: 27,295 were statically recompiled ..., and the other 55, ..."
        static const std::regex total(R"(All ([0-9]+(?:,[0-9]{3})*) functions)"), translated(R"(([0-9]+(?:,[0-9]{3})*) were (?:statically )?(?:recompiled|translated))"),
            replaced(R"(the other ([0-9]+(?:,[0-9]{3})*))");
        std::smatch m;
        if (std::regex_search(a.answer, m, total))
            a.total = m[1];
        if (std::regex_search(a.answer, m, translated))
            a.translated = m[1];
        if (std::regex_search(a.answer, m, replaced))
            a.replaced = m[1];
        // The key points, each shown only when the answer says it (its source is in the text).
        static const RecompAnswer::Point points[] = {
            {0, "GT4's programs for the PS2's 3D and maths chips are translated too.", "3D and maths chips are translated too"},
            {0, "The PS2's sound, disc, controller and memory-card modules are replaced by native code.",
             "memory-card modules are replaced by native code"},
            {0, "No emulator or interpreter runs the game's code.", "No emulator or interpreter runs the game's code"},
            {1,
             "One leftover: a slow step-by-step version of the 3D and maths chips, kept only in development builds to check the "
             "translations. No test run has ever needed it, and it will be removed from the final launch build.",
             "it will be removed from the final launch build"},
            {2, "The game's data still comes from your disc image, which is normal for a recompilation.", "The game's data still comes from your disc image"},
        };
        for (const RecompAnswer::Point &p : points)
            if (a.answer.find(p.source) != std::string::npos)
                a.points.push_back(p);
        a.found = !a.headline.empty() && !a.total.empty() && !a.translated.empty() && !a.replaced.empty();
        return a;
    }

    // ------------------------------------------------------------------ the layout
    float DocItem::top() const
    {
        switch (kind)
        {
        case Kind::Line: return std::min(y, y + h) - 8.0f;
        case Kind::Circle:
        case Kind::Check: return y - r;
        default: return y;
        }
    }

    float DocItem::bottom() const
    {
        switch (kind)
        {
        case Kind::Text: return y + size * 1.4f;
        case Kind::Line: return std::max(y, y + h) + 8.0f;
        case Kind::Circle:
        case Kind::Check: return y + r;
        default: return y + h;
        }
    }

    float DocLayout::anchor(const std::string &name, float fallback) const
    {
        for (const auto &[n, y] : anchors)
            if (n == name)
                return y;
        return fallback;
    }

    namespace
    {
        constexpr float kBody = 14.5f, kBodyGap = 1.5f;
        constexpr float kLead = 16.5f, kLeadGap = 1.45f;
        constexpr float kCell = 13.5f, kCellGap = 1.4f;
        constexpr float kTextH = 1.33f; // a line box's height per text size (the launcher's centring rule)

        struct Out
        {
            std::vector<DocItem> *items = nullptr; // null: measure only
            bool diagram = false;

            void add(DocItem it)
            {
                if (items)
                {
                    it.diagram = it.diagram || (diagram && it.kind == DocItem::Kind::Text);
                    items->push_back(std::move(it));
                }
            }
            void text(float x, float y, const std::string &t, float size, DocFont f, DocColor c, bool glued = false)
            {
                DocItem it;
                it.glued = glued;
                it.kind = DocItem::Kind::Text;
                it.x = x;
                it.y = y;
                it.text = t;
                it.size = size;
                it.font = f;
                it.color = c;
                add(std::move(it));
            }
            void box(DocItem::Kind k, float x, float y, float w, float h, float r, DocColor c, float a = 1.0f, float thick = 1.0f)
            {
                DocItem it;
                it.kind = k;
                it.x = x;
                it.y = y;
                it.w = w;
                it.h = h;
                it.r = r;
                it.color = c;
                it.alpha = a;
                it.thick = thick;
                add(std::move(it));
            }
            void fill(float x, float y, float w, float h, DocColor c, float a = 1.0f) { box(DocItem::Kind::Fill, x, y, w, h, 0, c, a); }
            void round(float x, float y, float w, float h, float r, DocColor c, float a = 1.0f) { box(DocItem::Kind::Round, x, y, w, h, r, c, a); }
            void outline(float x, float y, float w, float h, float r, DocColor c, float thick = 1.0f, float a = 1.0f)
            {
                box(DocItem::Kind::RoundLine, x, y, w, h, r, c, a, thick);
            }
            void line(float x1, float y1, float x2, float y2, float thick, DocColor c, int heads = 0, float dash = 0.0f, float a = 1.0f)
            {
                DocItem it;
                it.kind = DocItem::Kind::Line;
                it.x = x1;
                it.y = y1;
                it.w = x2 - x1;
                it.h = y2 - y1;
                it.thick = thick;
                it.color = c;
                it.heads = heads;
                it.dash = dash;
                it.alpha = a;
                add(std::move(it));
            }
            void circle(float cx, float cy, float r, DocColor c, float a = 1.0f) { box(DocItem::Kind::Circle, cx, cy, 0, 0, r, c, a); }
            void check(float cx, float cy, float r, DocColor c) { box(DocItem::Kind::Check, cx, cy, 0, 0, r, c); }
        };

        struct Style
        {
            float size = kBody;
            float gap = kBodyGap;
            DocFont font = DocFont::Regular;
            DocFont bold = DocFont::Bold;
            DocColor color = DocColor::Body;
            DocColor boldColor = DocColor::Text;
            bool centre = false;
        };

        // Word-wraps spans (with their bold runs) into [x, x + width] from y; returns the height.
        // A word glued to the one before it ("modules" + ":") stays on its line.
        float flow(Out &o, const std::vector<DocSpan> &spans, float x, float y, float width, const Style &st, const DocMeasure &m)
        {
            struct Word
            {
                std::string t;
                DocFont f;
                DocColor c;
                bool space;
                float w;
            };
            std::vector<Word> words;
            bool pending = false;
            for (const DocSpan &sp : spans)
            {
                const DocFont f = sp.code ? DocFont::Mono : sp.bold ? st.bold : st.font;
                const DocColor c = sp.bold || sp.code ? st.boldColor : st.color;
                std::string cur;
                auto push = [&]() {
                    if (!cur.empty())
                    {
                        words.push_back(Word{cur, f, c, pending, 0.0f});
                        pending = false;
                        cur.clear();
                    }
                };
                for (char ch : sp.text)
                {
                    if (ch == ' ' || ch == '\t' || ch == '\n')
                    {
                        push();
                        pending = true;
                    }
                    else
                        cur += ch;
                }
                push();
            }
            if (words.empty())
                return 0.0f;
            for (Word &w : words)
                w.w = m(w.t, st.size, w.f);
            const float spaceW = m(" ", st.size, st.font);
            std::vector<std::pair<size_t, size_t>> lines;
            std::vector<float> lineW;
            size_t begin = 0, i = 0;
            float lw = 0.0f;
            while (i < words.size())
            {
                size_t j = i + 1;
                float uw = words[i].w;
                while (j < words.size() && !words[j].space)
                    uw += words[j++].w;
                if (i != begin && lw + spaceW + uw > width)
                {
                    lines.push_back({begin, i});
                    lineW.push_back(lw);
                    begin = i;
                    lw = uw;
                }
                else
                    lw += (i == begin ? 0.0f : spaceW) + uw;
                i = j;
            }
            lines.push_back({begin, words.size()});
            lineW.push_back(lw);
            const float lh = st.size * st.gap;
            if (o.items)
                for (size_t li = 0; li < lines.size(); ++li)
                {
                    float cx = x + (st.centre ? std::max(0.0f, std::floor((width - lineW[li]) * 0.5f)) : 0.0f);
                    const float ly = y + lh * static_cast<float>(li);
                    std::string run;
                    float runX = cx;
                    DocFont rf = st.font;
                    DocColor rc = st.color;
                    bool runGlued = false;
                    for (size_t k = lines[li].first; k < lines[li].second; ++k)
                    {
                        const Word &w = words[k];
                        const bool sp = k != lines[li].first && w.space;
                        if (!run.empty() && (w.f != rf || w.c != rc))
                        {
                            o.text(runX, ly, run, st.size, rf, rc, runGlued);
                            run.clear();
                        }
                        if (sp)
                            cx += spaceW;
                        if (run.empty())
                        {
                            runX = cx;
                            rf = w.f;
                            rc = w.c;
                            run = w.t;
                            runGlued = k != lines[li].first && !w.space;
                        }
                        else
                            run += (sp ? " " : "") + w.t;
                        cx += w.w;
                    }
                    if (!run.empty())
                        o.text(runX, ly, run, st.size, rf, rc, runGlued);
                }
            return lh * static_cast<float>(lines.size());
        }

        float flowText(Out &o, const std::string &t, float x, float y, float width, const Style &st, const DocMeasure &m)
        {
            return flow(o, std::vector<DocSpan>{DocSpan{t}}, x, y, width, st, m);
        }

        float measureFlow(const std::vector<DocSpan> &spans, float width, const Style &st, const DocMeasure &m)
        {
            Out none;
            return flow(none, spans, 0.0f, 0.0f, width, st, m);
        }

        // ---- tables: a header row, zebra rows, wrapped cells
        DocColor statusColor(const std::string &cell)
        {
            const std::string s = lower(cell);
            if (startsWith(s, "done"))
                return DocColor::Ok;
            if (s.find("in progress") != std::string::npos || startsWith(s, "launcher made"))
                return DocColor::Accent;
            if (startsWith(s, "phase 5b") || startsWith(s, "written as add-ons"))
                return DocColor::Warn;
            if (startsWith(s, "not needed") || startsWith(s, "planned") || startsWith(s, "on hold"))
                return DocColor::Faint;
            return DocColor::Muted;
        }

        float table(Out &o, const DocBlock &b, float x, float y, float W, const DocMeasure &m)
        {
            if (b.rows.empty())
                return 0.0f;
            size_t nc = 0;
            for (const auto &r : b.rows)
                nc = std::max(nc, r.size());
            const float pad = 12.0f, padV = 8.0f, dot = 14.0f;
            enum class Col
            {
                Plain,
                Mono,
                Strong,
                Status,
            };
            std::vector<Col> cols(nc, Col::Plain);
            for (size_t c = 0; c < b.rows[0].size(); ++c)
            {
                const std::string h = lower(b.rows[0][c]);
                if (h == "modules")
                    cols[c] = Col::Mono;
                else if (h == "status")
                    cols[c] = Col::Status;
                else if (h == "number" || h == "word" || h == "phase")
                    cols[c] = Col::Strong;
            }
            auto style = [&](size_t c, bool header) {
                Style st;
                st.size = kCell;
                st.gap = kCellGap;
                if (header)
                {
                    st.size = 12.5f;
                    st.font = st.bold = DocFont::Semibold;
                    st.color = st.boldColor = DocColor::Text;
                }
                else if (cols[c] == Col::Mono)
                {
                    st.size = 11.5f; // Consolas draws larger than Segoe UI at the same size
                    st.gap = 1.5f;
                    st.font = st.bold = DocFont::Mono;
                    st.color = st.boldColor = DocColor::Text;
                }
                else if (cols[c] == Col::Strong)
                {
                    st.font = DocFont::Semibold;
                    st.color = DocColor::Text;
                }
                return st;
            };
            auto cellText = [&](size_t r, size_t c) { return c < b.rows[r].size() ? b.rows[r][c] : std::string(); };
            auto extra = [&](size_t r, size_t c) { return r > 0 && cols[c] == Col::Status ? dot : 0.0f; };
            // Column widths: natural widths when they fit, else each column between its longest
            // word and its natural width.
            std::vector<float> nat(nc, 0.0f), mn(nc, 0.0f), w(nc, 0.0f);
            for (size_t r = 0; r < b.rows.size(); ++r)
                for (size_t c = 0; c < nc; ++c)
                {
                    const Style st = style(c, r == 0);
                    const std::string t = spansText(parseInline(cellText(r, c)));
                    nat[c] = std::max(nat[c], m(t, st.size, st.font) + 2 * pad + extra(r, c));
                    for (const std::string &word : split(t, ' '))
                        mn[c] = std::max(mn[c], m(word, st.size, st.font) + 2 * pad + extra(r, c));
                }
            float sumN = 0.0f, sumM = 0.0f;
            for (size_t c = 0; c < nc; ++c)
            {
                sumN += nat[c];
                sumM += mn[c];
            }
            for (size_t c = 0; c < nc; ++c)
            {
                if (sumN <= W)
                    w[c] = nat[c] + (W - sumN) * nat[c] / sumN;
                else if (sumM >= W)
                    w[c] = W * mn[c] / sumM;
                else
                    w[c] = mn[c] + (W - sumM) * (nat[c] - mn[c]) / (sumN - sumM);
            }
            std::vector<float> rh(b.rows.size(), 0.0f);
            for (size_t r = 0; r < b.rows.size(); ++r)
            {
                for (size_t c = 0; c < nc; ++c)
                    rh[r] = std::max(rh[r], measureFlow(parseInline(cellText(r, c)), w[c] - 2 * pad - extra(r, c), style(c, r == 0), m));
                rh[r] = std::ceil(rh[r] + 2 * padV);
            }
            float H = 0.0f;
            for (float h : rh)
                H += h;
            const float rad = 8.0f;
            // backgrounds (opaque, so a rounded end and its square part can overlap): the header
            // and every other row
            o.round(x, y, W, rh[0], rad, DocColor::Panel3);
            o.fill(x, y + rh[0] * 0.5f, W, rh[0] * 0.5f, DocColor::Panel3);
            float ry = y + rh[0];
            for (size_t r = 1; r < b.rows.size(); ++r)
            {
                if (r % 2 == 0)
                {
                    if (r + 1 == b.rows.size())
                    {
                        o.round(x, ry, W, rh[r], rad, DocColor::RowAlt);
                        o.fill(x, ry, W, rh[r] - rad, DocColor::RowAlt);
                    }
                    else
                        o.fill(x, ry, W, rh[r], DocColor::RowAlt);
                }
                ry += rh[r];
            }
            o.outline(x, y, W, H, rad, DocColor::Border);
            // the cells
            ry = y;
            for (size_t r = 0; r < b.rows.size(); ++r)
            {
                float cx = x;
                for (size_t c = 0; c < nc; ++c)
                {
                    const Style st = style(c, r == 0);
                    const float ex = extra(r, c);
                    if (ex > 0.0f && !cellText(r, c).empty())
                        o.circle(cx + pad + 4.0f, ry + padV + st.size * 0.66f, 3.5f, statusColor(cellText(r, c)));
                    flow(o, parseInline(cellText(r, c)), cx + pad + ex, ry + padV, w[c] - 2 * pad - ex, st, m);
                    cx += w[c];
                }
                ry += rh[r];
            }
            return H;
        }

        // ---- the code illustration: a monospace box; two columns (the header line's) in two colours
        float code(Out &o, const DocBlock &b, float x, float y, float W, const DocMeasure &m)
        {
            const float padX = 18.0f, padY = 15.0f;
            float size = 12.0f;
            size_t longest = 0;
            for (const std::string &l : b.lines)
                longest = std::max(longest, l.size());
            float cw = m("0123456789", size, DocFont::Mono) / 10.0f;
            if (longest > 0 && cw * static_cast<float>(longest) > W - 2 * padX)
            {
                size = std::max(9.5f, size * (W - 2 * padX) / (cw * static_cast<float>(longest)));
                cw = m("0123456789", size, DocFont::Mono) / 10.0f;
            }
            const float lh = std::round(size * 1.85f);
            const float H = 2 * padY + lh * static_cast<float>(b.lines.size()) - (lh - size * kTextH);
            o.round(x, y, W, H, 10.0f, DocColor::CodeBg);
            o.outline(x, y, W, H, 10.0f, DocColor::Border);
            // Two columns when the first line has them (a run of spaces) and every line keeps them apart.
            size_t col2 = 0;
            if (b.lines.size() >= 2)
            {
                const std::string &h = b.lines[0];
                const size_t p = h.find("  ");
                const size_t q = p == std::string::npos ? std::string::npos : h.find_first_not_of(' ', p);
                if (q != std::string::npos)
                {
                    col2 = q;
                    for (const std::string &l : b.lines)
                        if (l.size() > col2 && (l[col2 - 1] != ' ' || l[col2 - 2] != ' '))
                            col2 = 0;
                }
            }
            if (col2)
            {
                size_t leftMax = 0;
                for (const std::string &l : b.lines)
                    leftMax = std::max(leftMax, rtrim(l.substr(0, std::min(col2, l.size()))).size());
                const float dx = x + padX + cw * (static_cast<float>(leftMax + col2) * 0.5f);
                o.line(dx, y + 9.0f, dx, y + H - 9.0f, 1.0f, DocColor::Border);
            }
            for (size_t i = 0; i < b.lines.size(); ++i)
            {
                const std::string &l = b.lines[i];
                const float ly = y + padY + lh * static_cast<float>(i);
                if (col2)
                {
                    const bool head = i == 0;
                    const std::string left = trim(l.substr(0, std::min(col2, l.size())));
                    const std::string right = l.size() > col2 ? rtrim(l.substr(col2)) : std::string();
                    if (!left.empty())
                        o.text(x + padX, ly, left, size, DocFont::Mono, head ? DocColor::Faint : DocColor::AccentText);
                    if (!right.empty())
                        o.text(x + padX + cw * static_cast<float>(col2), ly, right, size, DocFont::Mono, head ? DocColor::Faint : DocColor::Text);
                }
                else
                {
                    const size_t lead = l.find_first_not_of(' ');
                    if (lead != std::string::npos)
                        o.text(x + padX + cw * static_cast<float>(lead), ly, rtrim(l.substr(lead)), size, DocFont::Mono, DocColor::Text);
                }
            }
            return H;
        }

        // ---- the quote: a highlighted card, its bold opening as the headline with a check
        float quote(Out &o, const DocBlock &b, float x, float y, float W, const DocMeasure &m)
        {
            const float pad = 22.0f;
            std::string head;
            std::vector<DocSpan> rest = b.spans;
            if (!rest.empty() && rest[0].bold)
            {
                head = trim(rest[0].text);
                rest.erase(rest.begin());
                if (!rest.empty())
                    rest[0].text = trim(rest[0].text);
            }
            Style st;
            const float headH = head.empty() ? 0.0f : 32.0f;
            const float bodyH = measureFlow(rest, W - 2 * pad, st, m);
            const float H = std::ceil(pad + headH + (head.empty() ? 0.0f : 12.0f) + bodyH + pad - 4.0f);
            o.round(x, y, W, H, 12.0f, DocColor::AccentFill);
            o.outline(x, y, W, H, 12.0f, DocColor::AccentLine, 1.2f);
            o.round(x, y + 16.0f, 4.0f, H - 32.0f, 2.0f, DocColor::Accent);
            float yy = y + pad;
            if (!head.empty())
            {
                o.check(x + pad + 16.0f, yy + 16.0f, 16.0f, DocColor::Ok);
                const float hs = 20.0f;
                o.text(x + pad + 44.0f, yy + (headH - hs * kTextH) * 0.5f, head, hs, DocFont::Bold, DocColor::Text);
                yy += headH + 12.0f;
            }
            flow(o, rest, x + pad, yy, W - 2 * pad, st, m);
            return H;
        }

        // ---- "What's inside a PS2": the text diagram as boxes. The EE with VU0, VU1 and the IPU
        // and the IOP with the IRX modules side by side, the SIF link between them; the GS below the
        // EE and the devices below the IOP, each under the module that drives it.
        float diagram(Out &o, float x, float y, float W, const DocMeasure &m)
        {
            const float fp = 18.0f; // the figure's padding
            const float ix = x + fp, iw = W - 2 * fp;
            const float G = std::clamp(std::floor(iw * 0.15f), 88.0f, 112.0f); // the SIF link's gap
            const float lw = std::floor((iw - G) * 0.46f);
            const float rx = ix + lw + G, rw = ix + iw - rx;
            const float pad = 14.0f, gapV = 54.0f;
            Style title;
            title.size = 15.5f;
            title.font = title.bold = DocFont::Semibold;
            title.color = title.boldColor = DocColor::Text;
            Style sub;
            sub.size = 12.0f;
            sub.gap = 1.4f;
            sub.color = DocColor::Muted;
            Style small = sub;
            small.size = 11.5f;
            small.gap = 1.35f;
            Style smallC = small;
            smallC.centre = true;
            const float titleH = title.size * kTextH;

            const std::string eeSub = "the main processor: runs the game program, CORE.GT4";
            const std::string iopSub = "runs GT4's IRX modules: the drivers for the parts below";
            const std::string gsSub = "paints the picture and sends it to the TV";
            struct Part
            {
                const char *name;
                const char *what;
            };
            const Part units[3] = {{"VU0", "maths helper"}, {"VU1", "builds 3D scenes"}, {"IPU", "movie decoder"}};
            const Part devices[4] = {{"Disc", "drive"}, {"SPU2", "sound chip"}, {"Pads", "and memory cards"}, {"USB", "and network"}};
            const char *const modules[4][2] = {{"PDICDVD", nullptr}, {"PDISPU2", "LIBSD"}, {"DBCMAN", "MCSERV"}, {"USBD", "DEV9"}};

            // sizes
            const float ugap = 8.0f, uw = std::floor((lw - 2 * pad - 2 * ugap) / 3.0f);
            float unitDesc = 0.0f;
            for (const Part &u : units)
                unitDesc = std::max(unitDesc, measureFlow({DocSpan{u.what}}, uw - 16.0f, small, m));
            const float uh = std::ceil(10.0f + 15.0f * kTextH + 3.0f + unitDesc + 9.0f);
            const float eeH = pad + titleH + 5.0f + measureFlow({DocSpan{eeSub}}, lw - 2 * pad, sub, m) + 12.0f + uh + pad;
            const float dgap = 8.0f, dw = std::floor((rw - 3 * dgap) / 4.0f);
            const float chipLine = 10.5f * 1.45f, chipH = std::ceil(9.0f + 2 * chipLine + 6.0f);
            const float iopH = pad + titleH + 5.0f + measureFlow({DocSpan{iopSub}}, rw - 2 * pad, sub, m) + 12.0f + (11.0f * kTextH + 6.0f) + chipH + pad;
            const float topH = std::ceil(std::max(eeH, iopH));
            float devDesc = 0.0f;
            for (const Part &d : devices)
                devDesc = std::max(devDesc, measureFlow({DocSpan{d.what}}, dw - 10.0f, smallC, m));
            const float devH = 12.0f + 13.5f * kTextH + 3.0f + devDesc + 12.0f;
            const float gsH = pad + titleH + 5.0f + measureFlow({DocSpan{gsSub}}, lw - 2 * pad, sub, m) + pad;
            const float botH = std::ceil(std::max(devH, gsH));
            const float ty = y + fp, by = ty + topH + gapV;
            const float H = fp + topH + gapV + botH + fp;
            auto devX = [&](int i) { return rx + static_cast<float>(i) * (dw + dgap); };
            auto devCx = [&](int i) { return devX(i) + dw * 0.5f; };

            o.round(x, y, W, H, 12.0f, DocColor::Panel2, 0.45f);
            o.outline(x, y, W, H, 12.0f, DocColor::Border);
            o.diagram = true;

            // the Emotion Engine, with its vector units and the movie decoder
            o.round(ix, ty, lw, topH, 12.0f, DocColor::AccentFill);
            o.outline(ix, ty, lw, topH, 12.0f, DocColor::AccentLine, 1.5f);
            float yy = ty + pad;
            o.text(ix + pad, yy, "Emotion Engine (EE)", title.size, DocFont::Semibold, DocColor::Text);
            yy += titleH + 5.0f;
            flowText(o, eeSub, ix + pad, yy, lw - 2 * pad, sub, m);
            const float uy = ty + topH - pad - uh;
            for (int i = 0; i < 3; ++i)
            {
                const float ux = ix + pad + static_cast<float>(i) * (uw + ugap);
                o.round(ux, uy, uw, uh, 8.0f, DocColor::Panel);
                o.outline(ux, uy, uw, uh, 8.0f, DocColor::Border);
                o.text(ux + 8.0f, uy + 10.0f, units[i].name, 15.0f, DocFont::Semibold, DocColor::AccentText);
                flowText(o, units[i].what, ux + 8.0f, uy + 10.0f + 15.0f * kTextH + 3.0f, uw - 16.0f, small, m);
            }

            // the I/O processor, with the modules that drive the devices below
            o.round(rx, ty, rw, topH, 12.0f, DocColor::WarnFill);
            o.outline(rx, ty, rw, topH, 12.0f, DocColor::WarnLine, 1.5f);
            yy = ty + pad;
            o.text(rx + pad, yy, "I/O Processor (IOP)", title.size, DocFont::Semibold, DocColor::Text);
            yy += titleH + 5.0f;
            flowText(o, iopSub, rx + pad, yy, rw - 2 * pad, sub, m);
            const float cy = ty + topH - pad - chipH;
            o.text(rx + pad, cy - 11.0f * kTextH - 6.0f, "a few of the modules, each over the part it drives", 11.0f, DocFont::Regular, DocColor::Muted);
            for (int i = 0; i < 4; ++i)
            {
                const float chipW = std::max(30.0f, dw - 10.0f);
                const float cx = std::clamp(devCx(i) - chipW * 0.5f, rx + 7.0f, rx + rw - 7.0f - chipW);
                o.round(cx, cy, chipW, chipH, 6.0f, DocColor::Panel);
                o.outline(cx, cy, chipW, chipH, 6.0f, DocColor::WarnLine, 1.0f, 0.7f);
                const int n = modules[i][1] ? 2 : 1;
                const float first = cy + (chipH - static_cast<float>(n) * chipLine) * 0.5f + 1.0f;
                for (int k = 0; k < n; ++k)
                {
                    const std::string mod = modules[i][k];
                    o.text(cx + (chipW - m(mod, 10.5f, DocFont::Mono)) * 0.5f, first + static_cast<float>(k) * chipLine, mod, 10.5f, DocFont::Mono,
                           DocColor::Text);
                }
            }

            // the SIF link between them
            const float ly = ty + std::floor(std::min(topH * 0.42f, 80.0f));
            o.line(ix + lw + 6.0f, ly, rx - 6.0f, ly, 2.0f, DocColor::Text, 2, 6.0f);
            o.text(ix + lw + (G - m("SIF link", 11.5f, DocFont::Mono)) * 0.5f, ly - 24.0f, "SIF link", 11.5f, DocFont::Mono, DocColor::Text);
            flowText(o, "requests and answers", ix + lw + 4.0f, ly + 10.0f, G - 8.0f, smallC, m);

            // drawing commands down to the GS
            const float ax = ix + std::floor(lw * 0.5f);
            o.line(ax, ty + topH + 4.0f, ax, by - 5.0f, 1.6f, DocColor::Muted, 1);
            o.text(ax + 10.0f, ty + topH + (gapV - 10.5f * kTextH) * 0.5f, "drawing commands", 10.5f, DocFont::Mono, DocColor::Muted);
            o.round(ix, by, lw, botH, 12.0f, DocColor::Panel3, 0.75f);
            o.outline(ix, by, lw, botH, 12.0f, DocColor::Border);
            yy = by + pad;
            o.text(ix + pad, yy, "Graphics Synthesizer (GS)", title.size, DocFont::Semibold, DocColor::Text);
            yy += titleH + 5.0f;
            flowText(o, gsSub, ix + pad, yy, lw - 2 * pad, sub, m);

            // the devices, each below its module
            for (int i = 0; i < 4; ++i)
            {
                o.line(devCx(i), ty + topH + 4.0f, devCx(i), by - 5.0f, 1.6f, DocColor::Muted, 1);
                const float dx = devX(i);
                o.round(dx, by, dw, botH, 10.0f, DocColor::Panel3, 0.75f);
                o.outline(dx, by, dw, botH, 10.0f, DocColor::Border);
                const std::string name = devices[i].name;
                o.text(dx + (dw - m(name, 13.5f, DocFont::Semibold)) * 0.5f, by + 12.0f, name, 13.5f, DocFont::Semibold, DocColor::Text);
                flowText(o, devices[i].what, dx + 5.0f, by + 12.0f + 13.5f * kTextH + 3.0f, dw - 10.0f, smallC, m);
            }
            o.diagram = false;
            return H;
        }
    }

    DocLayout layoutDoc(const Doc &doc, float W, const DocMeasure &m)
    {
        DocLayout L;
        L.width = W;
        Out o;
        o.items = &L.items;
        const Style body;
        float y = 0.0f;
        int tables = 0;
        float headingY = -1.0f; // a heading just placed (a table right below it is anchored with it)
        for (const DocSection &s : doc.sections)
        {
            L.sectionY.push_back(y);
            L.anchors.push_back({"section:" + (s.number.empty() ? s.title : s.number), y});
            if (doc.isLead(s))
            {
                // The lead: larger, with an accent bar.
                Style lead;
                lead.size = kLead;
                lead.gap = kLeadGap;
                lead.color = DocColor::Text;
                float h = 0.0f;
                for (const DocBlock &b : s.blocks)
                    h += measureFlow(b.spans, W - 22.0f, lead, m) + 8.0f;
                o.round(0.0f, y + 3.0f, 4.0f, std::max(8.0f, h - 12.0f), 2.0f, DocColor::Accent);
                for (const DocBlock &b : s.blocks)
                    y += flow(o, b.spans, 22.0f, y, W - 22.0f, lead, m) + 8.0f;
                y += 26.0f;
                continue;
            }
            // The heading: a number badge, the title and a rule to the edge.
            {
                const float bh = 30.0f, hs = 21.0f;
                float tx = 0.0f;
                if (!s.number.empty())
                {
                    o.round(0.0f, y, bh, bh, 8.0f, DocColor::Panel3);
                    const float nw = m(s.number, 13.0f, DocFont::Mono);
                    o.text((bh - nw) * 0.5f, y + (bh - 13.0f * kTextH) * 0.5f, s.number, 13.0f, DocFont::Mono, DocColor::Text);
                    tx = bh + 14.0f;
                }
                o.text(tx, y + (bh - hs * kTextH) * 0.5f, s.title, hs, DocFont::Semibold, DocColor::Text);
                const float ruleX = tx + m(s.title, hs, DocFont::Semibold) + 16.0f;
                if (ruleX + 24.0f < W)
                    o.line(ruleX, y + bh * 0.5f, W, y + bh * 0.5f, 1.0f, DocColor::Border);
                y += bh + 16.0f;
            }
            for (size_t bi = 0; bi < s.blocks.size(); ++bi)
            {
                const DocBlock &b = s.blocks[bi];
                const bool afterHeading = bi > 0 && s.blocks[bi - 1].kind == DocKind::Heading;
                const bool listEnds = bi + 1 >= s.blocks.size() || s.blocks[bi + 1].kind != b.kind;
                switch (b.kind)
                {
                case DocKind::Paragraph: y += flow(o, b.spans, 0.0f, y, W, body, m) + 13.0f; break;
                case DocKind::Heading:
                    y += 6.0f;
                    headingY = y;
                    L.anchors.push_back({"heading:" + b.text, y});
                    o.text(0.0f, y, b.text, 16.5f, DocFont::Semibold, DocColor::Text);
                    y += 16.5f * kTextH + 10.0f;
                    break;
                case DocKind::Bullet:
                    o.circle(7.0f, y + kBody * 0.7f, 2.7f, DocColor::Accent);
                    y += flow(o, b.spans, 20.0f, y, W - 20.0f, body, m) + 7.0f + (listEnds ? 7.0f : 0.0f);
                    break;
                case DocKind::Numbered:
                {
                    const std::string n = std::to_string(b.number);
                    const float cy = y + kBody * 0.7f;
                    o.circle(11.0f, cy, 11.0f, DocColor::AccentFill);
                    o.circle(11.0f, cy, 10.0f, DocColor::Accent, 0.35f);
                    o.text(11.0f - m(n, 12.0f, DocFont::Semibold) * 0.5f, cy - 12.0f * kTextH * 0.5f, n, 12.0f, DocFont::Semibold, DocColor::Text);
                    y += std::max(24.0f, flow(o, b.spans, 34.0f, y, W - 34.0f, body, m)) + 9.0f + (listEnds ? 6.0f : 0.0f);
                    break;
                }
                case DocKind::Table:
                    L.anchors.push_back({"table:" + std::to_string(++tables), afterHeading ? headingY : y});
                    y += table(o, b, 0.0f, y, W, m) + 18.0f;
                    break;
                case DocKind::Code:
                    if (b.diagram)
                    {
                        L.anchors.push_back({"diagram", y});
                        y += diagram(o, 0.0f, y, W, m) + 18.0f;
                        L.anchors.push_back({"diagram-end", y});
                    }
                    else
                    {
                        L.anchors.push_back({"code", y});
                        y += code(o, b, 0.0f, y, W, m) + 18.0f;
                    }
                    break;
                case DocKind::Quote:
                    L.anchors.push_back({"quote", y});
                    y += quote(o, b, 0.0f, y, W, m) + 18.0f;
                    break;
                }
            }
            y += 26.0f;
        }
        L.height = y;
        return L;
    }
}
