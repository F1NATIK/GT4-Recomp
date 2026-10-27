#pragma once

// GT4 Recompilation launcher, Full edition: the pieces its pages share (the mock-ups' tabs, pills, radio lists, chips),
// drawn with the launcher's own toolkit (gt4stock_ui.h).

#include "gt4stock_ui.h"

#include <string>
#include <vector>

namespace gt4stock::fullui
{
    // The mock-ups' pills: "Classic too" (the Classic edition has it), "Full" (the full launcher only), "Experimental"
    // (works, checked less), "Not built yet" (development builds only: GT4.exe has no side for it yet).
    enum class Pill
    {
        ClassicToo,
        Full,
        Experimental,
        NotBuilt,
    };
    float pillWidth(ui::Ui &u, Pill p);
    float pill(ui::Ui &u, float x, float y, Pill p); // returns its width

    // Tabs with an underline (the mock-ups' Display and Garage tabs). Returns true when the choice changed.
    bool tabs(ui::Ui &u, const std::string &id, float x, float y, float w, const std::vector<std::string> &labels, int &sel, float size = 14.5f);

    // A card: title, pills on the right (right to left as given), and an optional dimmed look for a card whose controls
    // are all not built yet. Pill::Full and Pill::ClassicToo are not drawn (one launcher, no edition labels: developer 2026-10-20).
    void card(ui::Ui &u, Rectangle r, const std::string &title, const std::vector<Pill> &pills = {});

    // A radio row: a ring, the label, a muted hint after it; a greyed row is not clickable. Returns true when picked.
    bool radio(ui::Ui &u, const std::string &id, Rectangle r, bool selected, const std::string &label, const std::string &hint, bool enabled,
               bool notBuilt = false);

    // Small capitals ("SECTION", "MAKER").
    void caps(ui::Ui &u, float x, float y, const std::string &t, Color c);

    // The development builds' one line under a greyed control. Returns the height used.
    float notBuilt(ui::Ui &u, float x, float y, float w, const std::string &what = std::string());

    // A chip that is a button (Car Dealers' section and maker chips). Returns true when clicked; width gets its width.
    bool chipButton(ui::Ui &u, float x, float y, const std::string &label, bool selected, bool enabled, float &width, float size = 13.0f);

    // A label on the left and a value on the right, with a hairline under it (the mock-ups' spec rows).
    void specRow(ui::Ui &u, float x, float y, float w, const std::string &label, const std::string &value, Color valueColor, float size = 13.5f);

    // A labelled bar: value 0..1 against a scale, coloured by thresholds (ok below warnAt, warn below badAt, bad above).
    void conditionBar(ui::Ui &u, Rectangle r, float v01, float warnAt, float badAt);

    // A stat tile: big value, small label.
    void statTile(ui::Ui &u, Rectangle r, const std::string &value, const std::string &label, Color valueColor);
}
