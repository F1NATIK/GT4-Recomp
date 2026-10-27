#pragma once

// GT4 Recompilation launcher: the About page's "How it works" text and its layout.
//
// The text is gt4stock_how_it_works.md (1.3: analysis/launcher-v3/about-text/HOW-IT-WORKS-FULL.md,
// byte for byte), built into the exe as a resource. parseDoc() reads the Markdown it uses:
// "# " the title, "## " a section ("## 3. The IRX modules": number 3), "### " a heading inside a
// section, paragraphs with **bold**, "- " bullets, "1. " numbered steps, "| a | b |" tables (the
// first row is the header), ``` code blocks and "> " quotes. layoutDoc() places the whole text in a
// column of a given width with the caller's text measure: draw items in reading order (text runs,
// boxes, lines), the "What's inside a PS2" text diagram drawn as a box diagram. recompAnswer()
// gives About's "Is it a full recompilation?" card from the text. No raylib here (the host tests
// build it).

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gt4stock
{
    struct DocSpan
    {
        std::string text;
        bool bold = false;
        bool code = false;
    };
    std::vector<DocSpan> parseInline(const std::string &text); // **bold** and `code`

    enum class DocKind
    {
        Paragraph,
        Bullet,
        Numbered,
        Heading, // "### " inside a section
        Table,
        Code,
        Quote,
    };

    struct DocBlock
    {
        DocKind kind = DocKind::Paragraph;
        std::string text;            // the plain text (Markdown marks removed)
        std::vector<DocSpan> spans;  // the text with its bold runs
        int number = 0;              // Numbered: its number
        std::vector<std::vector<std::string>> rows; // Table: rows[0] is the header
        std::vector<std::string> lines;             // Code: the lines as written
        bool diagram = false;        // Code: the "What's inside a PS2" text diagram
    };

    struct DocSection
    {
        std::string heading; // as written ("1. What's inside a PS2")
        std::string number;  // "1" ("" when the heading has none)
        std::string title;   // "What's inside a PS2"
        std::vector<DocBlock> blocks;
    };

    struct Doc
    {
        std::string title;
        std::vector<DocSection> sections;
        const DocSection *find(const std::string &heading) const; // by heading or title
        bool isLead(const DocSection &s) const;                    // "About" / "Short version": the lead
        std::string shortVersion() const;                          // the lead's text
        std::string plainText() const;
    };
    Doc parseDoc(const std::string &markdown);

    // About's "Is it a full recompilation?" card, from that section's answer: the headline (its
    // bold opening), the function counts and the key points. A point is shown only when the
    // answer says it (its `source` is in the answer's text).
    struct RecompAnswer
    {
        bool found = false;
        std::string question;  // "Is it a full recompilation?"
        std::string headline;  // "Yes, it's a full recompilation."
        std::string total, translated, replaced; // "27,350", "27,295", "55"
        struct Point
        {
            int kind = 0; // 0 a check, 1 a note, 2 information
            std::string text;
            std::string source;
        };
        std::vector<Point> points;
        std::string answer; // the whole answer, plain text
    };
    RecompAnswer recompAnswer(const Doc &doc);

    // ---- layout ----
    enum class DocFont
    {
        Regular,
        Semibold,
        Bold,
        Mono,
    };
    // Colour roles; the page maps them to the launcher's theme.
    enum class DocColor
    {
        Text,
        Body,
        Muted,
        Faint,
        Accent,
        AccentText,
        Ok,
        Warn,
        Panel,
        Panel2,
        Panel3,
        RowAlt, // a table's every other row (opaque)
        Border,
        CodeBg,
        AccentFill,
        AccentLine,
        WarnFill,
        WarnLine,
        OkFill,
    };

    struct DocItem
    {
        enum class Kind
        {
            Text,      // text at (x, y): y is the top of the line box
            Fill,      // a rectangle
            Round,     // a rounded rectangle (r: the radius)
            RoundLine, // its outline (r: the radius, thick: the line)
            Line,      // from (x, y) to (x + w, y + h); dash > 0: dashed; heads: 0, 1 (at the end) or 2
            Circle,    // centre (x, y), radius r
            Check,     // a check mark in a circle: centre (x, y), radius r
        };
        Kind kind = Kind::Text;
        float x = 0, y = 0, w = 0, h = 0;
        float r = 0;
        float thick = 1.0f;
        float dash = 0.0f;
        int heads = 0;
        DocColor color = DocColor::Text;
        float alpha = 1.0f;
        std::string text;
        float size = 14.5f;
        DocFont font = DocFont::Regular;
        bool diagram = false; // a label of the PS2 diagram (the tests check those as a set)
        bool glued = false;   // its first word continues the run before it ("modules" + ":")
        float top() const;    // the vertical extent, for drawing only what is in view
        float bottom() const;
    };

    struct DocLayout
    {
        float width = 0, height = 0;
        std::vector<DocItem> items;
        std::vector<float> sectionY;                       // the top of each section
        std::vector<std::pair<std::string, float>> anchors; // "section:1", "heading:Status of the modules", "diagram", "code", "quote", "table:1"
        float anchor(const std::string &name, float fallback = -1.0f) const;
    };

    // The width of a text run in dp (the page passes its font measure).
    using DocMeasure = std::function<float(const std::string &text, float size, DocFont font)>;
    DocLayout layoutDoc(const Doc &doc, float width, const DocMeasure &measure);
}
