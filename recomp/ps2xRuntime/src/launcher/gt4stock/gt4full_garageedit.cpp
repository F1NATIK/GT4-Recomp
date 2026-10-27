// GT4 Recompilation launcher, Full edition: the Garage's changes to the save, on the launcher's side (FULL-LAUNCHER-PLAN.md
// 1.9, F4): where the change goes, the confirmation that lists exactly what changes, the change itself (gt4full_saveedit.h:
// GT4 closed, an automatic backup, the changed save checked before and after it replaces the card's files), the result,
// Car Dealers' specifications, and the hidden --edit-test mode the tests make their changed copies with.

#include "gt4stock_app.h"

#include "gt4full_ui.h"
#include "gt4save.h"
#include "gt4stock_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace gt4stock
{
    using ui::Align;
    using ui::Icon;
    using ui::Style;
    using ui::Weight;

    namespace
    {
        Rectangle R(float x, float y, float w, float h) { return Rectangle{x, y, w, h}; }
        const char *const kDot = " \xc2\xb7 ";
    }

    // ------------------------------------------------------------------ where the change goes
    EditTarget App::garageEditTarget() const
    {
        if (!garageCopy_.empty())
        {
            // Test pictures and tests: a copy of a save in place of the card (the test modes never change it).
            EditTarget t;
            std::error_code ec;
            if (!fs::is_directory(garageCopy_, ec))
            {
                t.why = "Works with folder cards: this save is in a .ps2 card file.";
                return t;
            }
            const bool saveFolder = fs::is_regular_file(garageCopy_ / gt4save::kGamedataFile, ec);
            t = folderCardTarget(saveFolder ? garageCopy_.parent_path() : garageCopy_, 1, backupDir());
            if (saveFolder)
                t.saveDir = garageCopy_;
            return t;
        }
        const int slot = std::clamp(st_.full.garageCard, 1, 2);
        if (cardIsFile(slot))
        {
            EditTarget t;
            t.slot = slot;
            t.why = strf("Works with folder cards: memory card slot %d uses a .ps2 card file.", slot);
            return t;
        }
        return folderCardTarget(cardDir(slot), slot, backupDir());
    }

    std::string App::garageEditBlock() const
    {
        if (busy())
            return preparing_ ? "Preparing shaders: wait a moment (GT4 runs hidden for a few seconds)."
                              : "Close GT4 first: the launcher changes your save only while GT4 is closed.";
        if (pending_.present && pending_.state == "half")
            return "A Garage change was cut off and your save is half changed: restore " + pending_.backup + " first (Saves > Restore).";
        const EditTarget t = garageEditTarget();
        return t.ok ? std::string() : t.why;
    }

    // ------------------------------------------------------------------ a change that was cut off
    void App::checkPendingChange()
    {
        // The note a change leaves while it replaces the card's files (gt4full_saveedit.h). Still there: the change was cut
        // off. As it was or as planned: the note goes (the launcher's own file). Half changed: the backup is offered.
        if (!garageCopy_.empty())
            return; // test pictures of a save copy: never the player's backups
        pending_ = pendingChange(backupDir());
        if (hidden_ || !pending_.present)
            return; // the test modes read only (they change nothing)
        std::string what;
        const bool settled = settlePendingChange(backupDir(), &what);
        pending_ = pendingChange(backupDir());
        if (!settled && pending_.present && !hidden_ && modal_ == Modal::None)
            modal_ = Modal::GarageCutOff;
        else if (settled && !what.empty() && !hidden_)
            setStatus(what.find("had finished") != std::string::npos ? "The last Garage change had finished."
                                                                     : "The last Garage change had not started; your save is as it was.",
                      ui::theme().muted);
    }

    void App::drawGarageCutOffModal()
    {
        const ui::Theme &t = ui::theme();
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        const float w = std::min(700.0f, ui_.width - 48), tw = w - 56;
        const std::string title = "A Garage change was cut off";
        const std::string text = "\"" + (pending_.title.empty() ? std::string("A change") : pending_.title) + "\" was being written to memory card slot " +
                                 std::to_string(std::max(1, pending_.slot)) +
                                 " when the launcher stopped (a crash or a power cut). Your save is half changed, and GT4 may not read it as it should.";
        const std::string fix = "Restore puts back the backup made just before the change, " + pending_.backup +
                                ". The card as it is now is backed up first and nothing is deleted (as Saves > Restore does).";
        const float h = 28 + 34 + ui_.wrapHeight(text, tw, 14) + 12 + ui_.wrapHeight(fix, tw, 13.5f) + 12 + 22 + 24 + 64;
        const Rectangle r = R(std::floor((ui_.width - w) * 0.5f), std::floor((ui_.height - h) * 0.5f), w, h);
        ui_.round(R(r.x + 3, r.y + 6, r.width, r.height), 14, ui::alpha(BLACK, 0.4f));
        ui_.round(r, 14, t.panel);
        ui_.roundLine(r, 14, 1, t.border);
        ui_.icon(Icon::Warn, R(r.x + 28, r.y + 28, 26, 26), t.warn);
        ui_.text(title, r.x + 68, r.y + 28, 20, t.text, Weight::Bold);
        float y = r.y + 72;
        y += ui_.wrap(text, r.x + 28, y, tw, 14, t.text) + 12;
        y += ui_.wrap(fix, r.x + 28, y, tw, 13.5f, t.muted) + 12;
        ui_.wrap(busy() ? "GT4 is running: close it first." : "Not now: the Garage changes nothing until the save is put back.", r.x + 28, y, tw, 13,
                 busy() ? t.bad : t.faint);
        if (ui_.button("cutoff-later", R(r.x + r.width - 28 - 230 - 12 - 150, r.y + r.height - 60, 150, 38), "Not now", Style::Secondary) || esc)
            modal_ = Modal::None;
        if (ui_.button("cutoff-restore", R(r.x + r.width - 28 - 230, r.y + r.height - 60, 230, 38), "Restore the backup", Style::Danger, !busy()))
        {
            modal_ = Modal::None;
            if (hidden_)
                return; // the test pictures never change a card
            restore(backupDir() / pathFrom(pending_.backup), std::clamp(pending_.slot, 1, 2));
            std::string what;
            if (settlePendingChange(backupDir(), &what))
                setStatus("Restored " + pending_.backup + ": your save is as it was before the change.", t.ok);
            pending_ = pendingChange(backupDir());
            loadGarage(true);
        }
    }

    // ------------------------------------------------------------------ the confirmation
    // Save details' Repair and Change name: the save the page shows. A folder card in slot 1 or 2 (as the Garage changes
    // it); the test pictures: the copy they show (they never confirm); anything else (a .ps2 card file, a backup or a
    // copy opened from the disk) is not changed.
    EditTarget App::detailsEditTarget() const
    {
        EditTarget t;
        if (hidden_ && detailsSlot_ == 0 && !detailsSource_.empty())
        {
            std::error_code ec;
            const bool saveFolder = fs::is_regular_file(detailsSource_ / gt4save::kGamedataFile, ec);
            t = folderCardTarget(saveFolder ? detailsSource_.parent_path() : detailsSource_, 1, backupDir());
            if (saveFolder)
                t.saveDir = detailsSource_;
            return t;
        }
        if (detailsSlot_ != 1 && detailsSlot_ != 2)
        {
            t.why = "The launcher changes only the save on memory card slot 1 or 2: open it from Saves.";
            return t;
        }
        if (cardIsFile(detailsSlot_))
        {
            t.slot = detailsSlot_;
            t.why = strf("Works with folder cards: memory card slot %d uses a .ps2 card file.", detailsSlot_);
            return t;
        }
        return folderCardTarget(cardDir(detailsSlot_), detailsSlot_, backupDir());
    }

    void App::drawRenameModal()
    {
        const ui::Theme &t = ui::theme();
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        if (ui_.inputOn() && !hidden_)
        {
            for (int c = GetCharPressed(); c > 0; c = GetCharPressed())
                if (c >= 32 && c < 127 && nameEdit_.size() < kNameMaxLetters)
                    nameEdit_.push_back(static_cast<char>(c));
            if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !nameEdit_.empty())
                nameEdit_.pop_back();
        }
        const float w = std::min(620.0f, ui_.width - 48), tw = w - 56;
        const std::string old = details_.player;
        const std::string problem = details_.appendBug ? std::string("This save still holds the extra data the old memory-card bug left: repair it first "
                                                                     "(Repair, next to that problem on this page), then change the name.")
                                    : nameEdit_ == old ? std::string("Type the new name.")
                                                       : nameProblem(nameEdit_);
        const std::string rules = strf("GT4's own name entry takes up to %zu letters. Here: A-Z, a-z, 0-9 and spaces between them (the letters GT4's "
                                       "name keyboard surely has).",
                                       kNameMaxLetters);
        const float h = 28 + 34 + 24 + 22 + 52 + 14 + ui_.wrapHeight(rules, tw, 13) + 10 + ui_.wrapHeight(problem.empty() ? std::string("x") : problem, tw, 13.5f) + 24 + 64;
        const Rectangle r = R(std::floor((ui_.width - w) * 0.5f), std::floor((ui_.height - h) * 0.5f), w, h);
        ui_.round(R(r.x + 3, r.y + 6, r.width, r.height), 14, ui::alpha(BLACK, 0.4f));
        ui_.round(r, 14, t.panel);
        ui_.roundLine(r, 14, 1, t.border);
        const float x = r.x + 28;
        float y = r.y + 28;
        ui_.text("Change your name in GT4", x, y, 20, t.text, Weight::Bold);
        y += 34;
        ui_.text("Now: " + (old.empty() ? std::string("(no name)") : old), x, y, 14, t.muted);
        y += 24;
        fullui::caps(ui_, x, y, "New name", t.faint);
        y += 22;
        const Rectangle box = R(x, y, std::min(360.0f, tw), 42);
        ui_.round(box, 8, t.panel2);
        ui_.roundLine(box, 8, 1.5f, details_.appendBug ? t.border : t.accent);
        const float nw = ui_.text(nameEdit_, box.x + 14, box.y + 11, 18, t.text, Weight::Semibold);
        if (!details_.appendBug && (hidden_ || (static_cast<int>(GetTime() * 2.0) % 2) == 0))
            ui_.fill(R(box.x + 16 + nw, box.y + 10, 2, 22), t.accent); // the caret
        ui_.text(strf("%zu / %zu", nameEdit_.size(), kNameMaxLetters), box.x + box.width + 12, box.y + 13, 13, t.faint);
        y += 52 + 14;
        y += ui_.wrap(rules, x, y, tw, 13, t.muted) + 10;
        if (!problem.empty())
            ui_.wrap(problem, x, y, tw, 13.5f, details_.appendBug || nameEdit_ != old ? t.warn : t.faint);
        const bool ok = problem.empty() && !details_.appendBug;
        const float by = r.y + r.height - 60;
        if (ui_.button("rename-cancel", R(r.x + r.width - 28 - 200 - 12 - 150, by, 150, 38), "Cancel", Style::Secondary) || esc)
            modal_ = Modal::None;
        if (ui_.button("rename-next", R(r.x + r.width - 28 - 200, by, 200, 38), "Next", Style::Primary, ok) ||
            (ok && ui_.inputOn() && !hidden_ && IsKeyPressed(KEY_ENTER)))
        {
            // (from Save details: its save; from Game progress and save details: the card it reads, the Garage's target)
            const EditTarget target = detailsOpen_ ? detailsEditTarget() : garageEditTarget();
            openGarageEdit(renameEdit(nameEdit_), &target);
        }
    }

    void App::openGarageEdit(const GarageEdit &e, const EditTarget *target)
    {
        editReq_ = e;
        editResult_ = EditResult{};
        editTarget_ = target ? *target : garageEditTarget();
        editPlan_ = EditPlan{};
        if (!editTarget_.ok)
            editPlan_.why = editTarget_.why;
        else
        {
            // The plan from the save as it is now (read again: GT4 may have saved since the page was drawn).
            const gt4save::Save s = gt4save::loadSaveFolder(editTarget_.saveDir, names_ ? names_->spec.get() : nullptr);
            editPlan_ = planEdit(s, editTarget_.saveDir, e, names_.get());
        }
        ui_.closePopups();
        modal_ = Modal::GarageConfirm;
    }

    void App::confirmGarageEdit()
    {
        const ui::Theme &t = ui::theme();
        if (hidden_)
            return; // the test pictures never change a save (--edit-test calls applyEdit itself)
        // GT4 closed, checked again now (the page's own check runs every 2 s).
        const int state = gameRunning_ ? 1 : gt4ProcessState();
        if (state == 1 && !gameRunning_)
            otherGameRunning_ = true;
        editResult_ = applyEdit(editTarget_, editReq_, editPlan_.fingerprint, state, names_ ? names_->spec.get() : nullptr, names_.get(), kVersion);
        refreshCards(); // the new backup on the Saves page
        loadGarage(true);
        if (detailsOpen_)
            reloadDetails();
        readCheatSave(true);
        if (editResult_.ok && editReq_.action == GarageAction::AddCar && editPlan_.newSlot >= 0)
            garageSel_ = editPlan_.newSlot; // My garage shows the new car
        setStatus(editResult_.message, editResult_.ok ? t.ok : t.bad);
        modal_ = Modal::GarageResult;
    }

    namespace
    {
        Rectangle modalPanel(ui::Ui &u, float w, float h)
        {
            const ui::Theme &t = ui::theme();
            w = std::min(w, u.width - 48);
            h = std::min(h, u.height - 48);
            const Rectangle r = R(std::floor((u.width - w) * 0.5f), std::floor((u.height - h) * 0.5f), w, h);
            u.round(R(r.x + 3, r.y + 6, r.width, r.height), 14, ui::alpha(BLACK, 0.4f));
            u.round(r, 14, t.panel);
            u.roundLine(r, 14, 1, t.border);
            return r;
        }
    }

    void App::drawGarageEditModal()
    {
        const ui::Theme &t = ui::theme();
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        const float w = std::min(760.0f, ui_.width - 48);
        const float tw = w - 56;
        const EditPlan &p = editPlan_;
        const std::string title = p.ok ? p.title : std::string("This cannot be changed now");
        const std::string backupName = strf("backups\\GT4-mc%d-<date>_<time>-before-edit.zip", std::max(1, editTarget_.slot) - 1);
        std::vector<std::string> before;
        before.push_back(busy() ? "GT4 is running: close it first. The launcher changes your save only while GT4 is closed."
                                : "GT4 is closed (checked again when you confirm).");
        before.push_back(strf("Memory card slot %d is backed up first, to ", std::max(1, editTarget_.slot)) + backupName +
                         ". Saves > Restore puts it back.");
        before.push_back("The changed save is checked the way GT4 checks a save, and nothing else in it changes. If a check fails, your save is left as it was.");
        // Height: title, the changes (or the reason), the "before" box, the buttons.
        float h = 28 + ui_.wrapHeight(title, tw - 40, 20, Weight::Bold) + 18;
        // Adding a car: its colour, a swatch and GT4's name ("Colour N" when GT4 gives none).
        const bool colourRow = p.ok && editReq_.action == GarageAction::AddCar;
        if (colourRow)
            h += 34;
        if (p.ok)
        {
            h += 26;
            for (const std::string &c : p.changes)
                h += ui_.wrapHeight(c, tw - 22, 14) + 8;
        }
        else
            h += ui_.wrapHeight(p.why, tw, 14) + 8;
        h += 18 + 26;
        for (const std::string &b : before)
            h += ui_.wrapHeight(b, tw - 40, 13) + 6;
        h += 24 + 64;
        const Rectangle r = modalPanel(ui_, w, h);
        const float x = r.x + 28;
        float y = r.y + 28;
        ui_.icon(p.ok ? Icon::Garage : Icon::Warn, R(x, y, 26, 26), p.ok ? t.accent : t.warn);
        y += ui_.wrap(title, x + 40, y, tw - 40, 20, t.text, Weight::Bold) + 18;
        if (colourRow)
        {
            const auto c = names_ ? names_->colour(editReq_.code, editReq_.colour) : std::nullopt;
            const std::string name = dealerColourName(names_.get(), editReq_.code, editReq_.colour, !editReq_.colourName.empty());
            float tx = x;
            if (c)
            {
                ui_.circle(x + 10, y + 10, 9, Color{c->r, c->g, c->b, 255});
                ui_.ring(x + 10, y + 10, 9, 10, t.border);
                tx = x + 28;
            }
            const float lw = ui_.text("Colour: ", tx, y + 2, 14, t.muted);
            ui_.text(name, tx + lw, y + 2, 14, t.text, Weight::Semibold);
            y += 34;
        }
        if (p.ok)
        {
            fullui::caps(ui_, x, y, "What changes in your save", t.faint);
            y += 26;
            for (const std::string &c : p.changes)
            {
                ui_.circle(x + 6, y + 9, 3, t.accent);
                y += ui_.wrap(c, x + 22, y, tw - 22, 14, t.text) + 8;
            }
        }
        else
            y += ui_.wrap(p.why, x, y, tw, 14, t.warn) + 8;
        y += 18;
        fullui::caps(ui_, x, y, "Before anything is written", t.faint);
        y += 26;
        for (size_t i = 0; i < before.size(); ++i)
        {
            const Icon ic = i == 0 ? (busy() ? Icon::Warn : Icon::Check) : i == 1 ? Icon::Card : Icon::Check;
            ui_.icon(ic, R(x, y, 18, 18), i == 0 && busy() ? t.bad : t.muted);
            y += ui_.wrap(before[i], x + 30, y, tw - 40, 13, i == 0 && busy() ? t.bad : t.muted) + 6;
        }
        const float by = r.y + r.height - 60;
        if (ui_.button("edit-cancel", R(r.x + r.width - 28 - 150 - 12 - 250, by, 150, 38), "Cancel", Style::Secondary) || esc)
            modal_ = Modal::None;
        if (ui_.button("edit-confirm", R(r.x + r.width - 28 - 250, by, 250, 38), p.ok ? p.confirm : std::string("Confirm"),
                       editReq_.action == GarageAction::RemoveRigidity ? Style::Danger : Style::Primary, p.ok && !busy(), 14))
            confirmGarageEdit();
    }

    void App::drawGarageDoneModal()
    {
        const ui::Theme &t = ui::theme();
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        const float w = std::min(640.0f, ui_.width - 48), tw = w - 56;
        const std::string title = editResult_.ok ? "Done" : "Nothing was changed";
        std::string text = editResult_.message;
        if (editResult_.ok && startsWith(text, "Done: ") && text.size() > 6)
        {
            text = text.substr(6); // the title says Done
            text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
        }
        if (!editResult_.ok && editResult_.rolledBack)
            text += " (Your save's files were put back as they were.)";
        const float h = 28 + 30 + ui_.wrapHeight(text, tw, 14) + 24 + 64;
        const Rectangle r = modalPanel(ui_, w, h);
        ui_.icon(editResult_.ok ? Icon::Check : Icon::Warn, R(r.x + 28, r.y + 28, 26, 26), editResult_.ok ? t.ok : t.warn);
        ui_.text(title, r.x + 68, r.y + 28, 20, t.text, Weight::Bold);
        ui_.wrap(text, r.x + 28, r.y + 72, tw, 14, t.muted);
        if (ui_.button("edit-ok", R(r.x + r.width - 168, r.y + r.height - 60, 140, 38), "OK", Style::Primary) || esc)
            modal_ = Modal::None;
    }

    void App::drawCarSpecsModal()
    {
        const ui::Theme &t = ui::theme();
        const bool esc = IsKeyPressed(KEY_ESCAPE);
        const DiscCar *d = cat_ ? cat_->carByLabel(specsLabel_) : nullptr;
        if (!d)
        {
            modal_ = Modal::None;
            return;
        }
        const float w = std::min(560.0f, ui_.width - 48), tw = w - 56;
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Maker", d->maker.empty() ? std::string("-") : d->maker});
        rows.push_back({"Year", d->year ? std::to_string(d->year) : std::string("-")});
        rows.push_back({"Drivetrain", drivetrainName(d->drivetrain)});
        rows.push_back({"Power (catalogue)", d->powerPs ? strf("%d PS (%.0f hp, %.0f kW)", d->powerPs, d->powerPs * 0.98632, d->powerPs * 0.73550) : std::string("-")});
        rows.push_back({"Torque (catalogue)", d->torqueX100 ? strf("%.1f kgfm (%.0f Nm)", d->torqueX100 / 100.0, d->torqueX100 / 100.0 * 9.80665) : std::string("-")});
        rows.push_back({"Weight", d->weightKg ? groupedNumber(static_cast<uint64_t>(d->weightKg)) + " kg" : std::string("-")});
        rows.push_back({"Weight balance (front : rear)", weightBalanceText(d->frontWeightPct)});
        rows.push_back({"Length / height", d->lengthMm ? groupedNumber(static_cast<uint64_t>(d->lengthMm)) + " / " + groupedNumber(static_cast<uint64_t>(d->heightMm)) + " mm"
                                                      : std::string("-")});
        rows.push_back({"Wheelbase", d->wheelbaseMm ? groupedNumber(static_cast<uint64_t>(d->wheelbaseMm)) + " mm" : std::string("-")});
        rows.push_back({"Tyres front / rear", d->front.ok() ? tyreText(d->front) + " / " + tyreText(d->rear) : std::string("-")});
        rows.push_back({"New price (GT4's car data)", d->price ? "Cr. " + groupedNumber(d->price) : std::string("none")});
        const float h = 28 + ui_.wrapHeight(d->fullName, tw, 20, Weight::Bold) + 16 + rows.size() * 30.0f + 30 + 64;
        const Rectangle r = modalPanel(ui_, w, h);
        float y = r.y + 28;
        y += ui_.wrap(d->fullName, r.x + 28, y, tw, 20, t.text, Weight::Bold) + 16;
        for (const auto &[k, v] : rows)
        {
            fullui::specRow(ui_, r.x + 28, y, tw, k, v, t.text, 13);
            y += 30;
        }
        ui_.wrap("From GT4's own car data on your disc.", r.x + 28, y + 4, tw, 12, t.faint);
        if (ui_.button("specs-ok", R(r.x + r.width - 168, r.y + r.height - 60, 140, 38), "Close", Style::Primary) || esc)
            modal_ = Modal::None;
    }

    // ------------------------------------------------------------------ --edit-test (tests only)
    int App::runEditTest()
    {
        report(strf("%s launcher %s, %s edition: --edit-test %s", kProductName, kVersion, kEdition, opt_.editTest.c_str()));
        // Never a player's own save: only a GT4 folder a test made (it holds edit-test-root.txt), never one under a
        // "GT4 Test Build" folder.
        std::error_code ec;
        const std::string rootText = lower(pathText(fs::weakly_canonical(inst_.root, ec)));
        if (!fs::is_regular_file(inst_.root / "edit-test-root.txt", ec) || rootText.find("gt4 test build") != std::string::npos)
        {
            report("REFUSED: --edit-test works only on a test copy (a GT4 folder with edit-test-root.txt, not under a GT4 Test Build folder): " + pathText(inst_.root));
            return 2;
        }
        applyExtraCars(); // extra cars: the pack next to --exe (no switch)
        startNames();
        pollNames(true);
        startCatalogue();
        pollCatalogue(true);
        garageCopy_.clear();
        report("extra cars: " + (extraCarsKey_.empty() ? "none (no " + pathText(extraCarsPack()) + ", or GT4_EXTRA_CARS=off, or a GT4 program without them)"
                                                       : "esso (" + pathText(extraCarsPack()) + ")" + (extraCarsNote_.empty() ? std::string() : ": " + extraCarsNote_)));
        loadGarage(true);
        report(names_ ? strf("names from the disc: %zu cars", names_->carNameCount()) : "names from the disc: none (" + namesError_ + ")");
        report(cat_ ? strf("car data from the disc: %zu cars", cat_->carCount()) : "car data from the disc: none (" + catError_ + ")");
        // The request, made as the pages make it.
        const std::vector<std::string> a = split(opt_.editTest, ':');
        GarageEdit e;
        bool made = false;
        long long slot = -1;
        if (a.size() == 1 && a[0] == "repair")
        {
            e = repairEdit(); // Save details > Repair
            made = true;
        }
        else if (a.size() == 2 && a[0] == "rename")
        {
            e = renameEdit(a[1]); // Save details > Change name
            made = true;
            report("the name: " + (nameProblem(a[1]).empty() ? std::string("GT4 takes it") : "refused (" + nameProblem(a[1]) + ")"));
        }
        else if (a.size() >= 2 && (a[0] == "oil" || a[0] == "wash" || a[0] == "unrigid" || a[0] == "refresh") && parseInt(a[1], slot))
        {
            if (const GarageCar *c = garage_.bySlot(static_cast<int>(slot)))
            {
                const GarageAction act = a[0] == "oil"       ? GarageAction::OilChange
                                         : a[0] == "wash"    ? GarageAction::Wash
                                         : a[0] == "refresh" ? GarageAction::RefreshBody
                                                             : GarageAction::RemoveRigidity;
                e = serviceEdit(*c, act);
                made = true;
                const std::string why = serviceBlock(garage_, *c, act);
                report("the page's button: " + (why.empty() ? std::string("enabled") : "disabled (" + why + ")"));
            }
        }
        else if (a.size() >= 3 && a[0] == "extra" && cat_)
        {
            // Car Dealers > Extra Cars: extra:<label>:<pay|free>[:<colour index>] (analysis/jp-cars).
            for (const char *label : kExtraCars)
                if (a[1] == label)
                    if (const DiscCar *d = cat_->carByLabel(label))
                    {
                        e = prizeCarEdit(*d, a[2] == "pay");
                        made = true;
                        if (long long col = 0; a.size() >= 4 && parseInt(a[3], col))
                            e = withColour(e, static_cast<int>(col), nullptr);
                    }
        }
        else if (a.size() >= 3 && (a[0] == "hidden" || a[0] == "prize") && cat_)
        {
            const bool pay = a[2] == "pay";
            if (a[0] == "hidden")
            {
                for (const HiddenCar &h : kHiddenCars)
                    if (a[1] == h.label)
                    {
                        e = hiddenCarEdit(*cat_, h, pay);
                        made = !e.label.empty();
                        if (long long col = 0; made && a.size() >= 4 && parseInt(a[3], col))
                            e = withColour(e, static_cast<int>(col), &h); // hidden:<label>:<pay|free>:<colour index>
                    }
            }
            else
                for (const PrizeCar &pc : cat_->prizes())
                    if (pc.label == a[1])
                        if (const DiscCar *d = cat_->car(pc.code))
                        {
                            e = prizeCarEdit(*d, pay);
                            made = true;
                            if (long long col = 0; a.size() >= 4 && parseInt(a[3], col))
                                e = withColour(e, static_cast<int>(col), nullptr); // prize:<label>:<pay|free>:<colour index>
                        }
        }
        if (!made)
        {
            report("REFUSED: not an action this mode knows, or no such car / garage slot: " + opt_.editTest);
            return 2;
        }
        const std::string block = garageEditBlock();
        report("the Garage can change the save: " + (block.empty() ? std::string("yes") : "no (" + block + ")"));
        openGarageEdit(e);
        modal_ = Modal::None;
        report("confirmation: " + (editPlan_.ok ? editPlan_.title + " [" + editPlan_.confirm + "]" : "refused: " + editPlan_.why));
        for (const std::string &c : editPlan_.changes)
            report("  - " + c);
        report("  fields allowed to change: " + join(editPlan_.allowed, ", "));
        if (!editPlan_.ok)
            return 1;
        const int state = gt4ProcessState();
        report(strf("GT4 running now: %s", state == 0 ? "no" : state == 1 ? "YES" : "cannot tell"));
        editResult_ = applyEdit(editTarget_, editReq_, editPlan_.fingerprint, state, names_ ? names_->spec.get() : nullptr, names_.get(), kVersion);
        for (const std::string &l : editResult_.log)
            report("  " + l);
        report(std::string("result: ") + (editResult_.ok ? "ok" : "NOT CHANGED") + kDot + editResult_.message);
        loadGarage(true);
        reportGarage("after");
        if (e.action == GarageAction::AddCar && editPlan_.newSlot >= 0)
            if (const GarageCar *c = garage_.bySlot(editPlan_.newSlot))
                report(strf("read back: garage slot %d is %s (code %u), colour index %d (%s)", editPlan_.newSlot, c->label.c_str(), static_cast<unsigned>(c->code),
                            c->colourIndex, c->colourName.empty() ? "no name in GT4's data" : c->colourName.c_str()));
        return editResult_.ok ? 0 : 1;
    }
}
