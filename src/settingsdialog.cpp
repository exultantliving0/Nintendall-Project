#include "settingsdialog.hpp"

#include <algorithm>
#include <cctype>

#include "uistyle.hpp"

using namespace ui;

void SettingsDialog::open(System system, LibretroCore& core) {
    system_ = system;
    rows_.clear();
    scroll_ = 0;
    auto& options = core.options();

    std::vector<SettingGroup> groups = consoleSettings(system);
    if (system == System::WiiU && !groups.empty()) {
        // Upscaling the way standalone Cemu does it: the game's own Resolution graphic pack, made for
        // that game (Cemu's generic resize broke shadows). Cemu declares each pack of the loaded game
        // as options named after the pack: "Resolution" (on/off) and "Resolution: <group>" (preset).
        std::string enableKey, presetKey;
        for (const auto& [key, o] : options) {
            if (key.rfind("cemu_zgp_", 0) != 0) continue;
            if (o.description == "Resolution") enableKey = key;
            else if (o.description.rfind("Resolution: ", 0) == 0 &&
                     (presetKey.empty() || o.description == "Resolution: Preset"))
                presetKey = key;
        }
        if (!enableKey.empty() && !presetKey.empty()) {
            Setting res;
            res.label = "Resolution";
            res.keys = {enableKey, presetKey};
            res.restart = true;
            res.hint = "This game's own resolution pack: sharper, with shadows and effects kept right";
            res.choices.push_back({"Off (native)", {{enableKey, "disabled"}}});
            for (const std::string& preset : options[presetKey].values)
                res.choices.push_back({preset, {{enableKey, "enabled"}, {presetKey, preset}}});
            auto& picture = groups[0].settings;
            picture.insert(picture.begin() + std::min<size_t>(1, picture.size()), res);
        }
    }

    for (const auto& group : groups) {
        std::vector<Row> groupRows;
        for (const auto& s : group.settings) {
            // Only show settings this build of the emulator actually has.
            bool ok = !s.keys.empty();
            for (const auto& k : s.keys) ok = ok && options.count(k);
            if (!ok) continue;
            Row r;
            r.setting = s;
            if (s.choices.empty()) {
                const auto& o = options[s.keys[0]];
                for (size_t i = 0; i < o.values.size(); ++i) {
                    const std::string& label = i < o.labels.size() && o.labels[i] != o.values[i] ? o.labels[i] : prettyValue(o.values[i]);
                    r.choices.push_back({label, {{s.keys[0], o.values[i]}}});
                }
            } else {
                for (const auto& c : s.choices) {  // drop choices the emulator doesn't offer
                    bool valid = true;
                    for (const auto& [k, v] : c.set) {
                        const auto& vals = options[k].values;
                        valid = valid && (vals.empty() || std::find(vals.begin(), vals.end(), v) != vals.end());
                    }
                    if (valid) r.choices.push_back(c);
                }
            }
            if (r.choices.size() >= 2) groupRows.push_back(std::move(r));
        }
        if (groupRows.empty()) continue;
        Row header;
        header.header = group.title;
        rows_.push_back(header);
        for (auto& r : groupRows) rows_.push_back(std::move(r));
    }
    open_ = true;
}

static bool sameText(const std::string& a, const std::string& b) {
    // Some emulators store a value in a different letter case than they list it ("english" vs "English").
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

int SettingsDialog::currentChoice(const Row& r, LibretroCore& core) const {
    auto& options = core.options();
    for (size_t i = 0; i < r.choices.size(); ++i) {
        bool match = true;
        for (const auto& [k, v] : r.choices[i].set) {
            auto it = options.find(k);
            match = match && it != options.end() && sameText(it->second.value, v);
        }
        if (match) return int(i);
    }
    return -1;
}

void SettingsDialog::apply(Row& r, int choice, LibretroCore& core) {
    const int n = int(r.choices.size());
    choice = ((choice % n) + n) % n;
    for (const auto& [k, v] : r.choices[size_t(choice)].set) core.setOption(k, v);
    if (r.setting.restart) pendingRestart_ = true;
}

void SettingsDialog::handleEvent(const SDL_Event& e, int mx, int my, LibretroCore& core) {
    if (!open_) return;
    switch (e.type) {
    case SDL_MOUSEMOTION:
        mouseX_ = mx;
        mouseY_ = my;
        return;
    case SDL_MOUSEWHEEL:
        scroll_ = std::clamp(scroll_ - e.wheel.y * rowH_ * 2, 0, maxScroll_);
        return;
    case SDL_KEYDOWN:
        if (e.key.keysym.sym == SDLK_ESCAPE) close();
        else if (e.key.keysym.sym == SDLK_DOWN || e.key.keysym.sym == SDLK_PAGEDOWN)
            scroll_ = std::min(maxScroll_, scroll_ + rowH_ * (e.key.keysym.sym == SDLK_DOWN ? 1 : 8));
        else if (e.key.keysym.sym == SDLK_UP || e.key.keysym.sym == SDLK_PAGEUP)
            scroll_ = std::max(0, scroll_ - rowH_ * (e.key.keysym.sym == SDLK_UP ? 1 : 8));
        return;
    case SDL_MOUSEBUTTONDOWN: {
        mouseX_ = mx;
        mouseY_ = my;
        if (inside(closeBtn_, mx, my)) {
            close();
            return;
        }
        if (system_ == System::WiiU && inside(cacheBtn_, mx, my)) {
            clearCacheRequested_ = true;
            close();
            return;
        }
        if (inside(restartBtn_, mx, my) && pendingRestart_) {
            restartRequested_ = true;
            pendingRestart_ = false;
            close();
            return;
        }
        if (inside(resetBtn_, mx, my)) {
            for (auto& r : rows_) {
                if (!r.header.empty()) continue;
                for (const auto& k : r.setting.keys) {
                    auto& o = core.options()[k];
                    if (o.value != o.defaultValue) {
                        core.setOption(k, o.defaultValue);
                        if (r.setting.restart) pendingRestart_ = true;
                    }
                }
            }
            return;
        }
        if (!inside(list_, mx, my)) return;
        for (auto& r : rows_) {
            if (!r.header.empty()) continue;
            const int cur = std::max(0, currentChoice(r, core));
            const bool back = e.button.button == SDL_BUTTON_RIGHT || inside(r.prevBox, mx, my);
            if (inside(r.valueBox, mx, my)) {
                apply(r, back ? cur - 1 : cur + 1, core);
                return;
            }
        }
        return;
    }
    default:
        return;
    }
}

void SettingsDialog::draw(Video& v, Text& t, LibretroCore& core, int W, int H) {
    if (!open_) return;
    const float s = t.scale();
    const int lh = t.lineHeight();
    const int pad = int(14 * s);
    const int titleH = lh + int(12 * s);
    rowH_ = lh + int(12 * s);
    const int footerH = lh + int(16 * s) + lh + int(14 * s);

    int contentH = 0;
    for (const auto& r : rows_) contentH += r.header.empty() ? rowH_ : rowH_ + int(4 * s);
    if (rows_.empty()) contentH = rowH_ * 2;
    const int dw = std::min(W - int(16 * s), int(640 * s));
    const int maxListH = H - int(24 * s) - titleH - footerH - pad;
    const int listH = std::max(rowH_, std::min(contentH, maxListH));
    const int dh = titleH + pad / 2 + listH + footerH + pad / 2;
    dialog_ = {(W - dw) / 2, std::max(0, (H - dh) / 2), dw, dh};
    list_ = {dialog_.x + 1, dialog_.y + titleH + pad / 2, dw - 2, listH};
    maxScroll_ = std::max(0, contentH - listH);
    scroll_ = std::clamp(scroll_, 0, maxScroll_);

    v.fillRect(0, 0, W, H, {0, 0, 0, 120});
    box(v, dialog_, kDialogBg, kBorder);
    v.fillRect(dialog_.x + 1, dialog_.y + 1, dialog_.w - 2, titleH, kTitleBg);
    t.draw(dialog_.x + pad, dialog_.y + (titleH - lh) / 2,
           std::string("Console Settings - ") + systemInfo(system_).fullName, kTextC);

    // Rows (clipped to the list area so scrolling works).
    SDL_Renderer* ren = v.renderer();
    SDL_RenderSetClipRect(ren, &list_);
    const int valueW = int(dw * 0.46);
    const int valueX = dialog_.x + dw - pad - valueW;
    const int arrowW = int(24 * s);
    std::string hoverHint;
    int y = list_.y - scroll_;
    if (rows_.empty())
        t.draw(dialog_.x + pad, y + (rowH_ - lh) / 2, "This console has no extra settings.", kDim);
    for (auto& r : rows_) {
        if (!r.header.empty()) {
            y += int(4 * s);
            t.draw(dialog_.x + pad, y + (rowH_ - lh) / 2, r.header, kAccent);
            v.fillRect(dialog_.x + pad + t.width(r.header) + int(8 * s), y + rowH_ / 2,
                       dw - pad * 2 - t.width(r.header) - int(8 * s), 1, kFieldBorder);
            y += rowH_;
            continue;
        }
        const bool rowHover = mouseY_ >= y && mouseY_ < y + rowH_ && inside(list_, mouseX_, mouseY_);
        if (rowHover && !r.setting.hint.empty()) hoverHint = r.setting.hint;
        t.draw(dialog_.x + pad, y + (rowH_ - lh) / 2, r.setting.label + (r.setting.restart ? " *" : ""), kTextC);

        r.valueBox = {valueX, y + int(3 * s), valueW, rowH_ - int(6 * s)};
        r.prevBox = {r.valueBox.x, r.valueBox.y, arrowW, r.valueBox.h};
        r.nextBox = {r.valueBox.x + r.valueBox.w - arrowW, r.valueBox.y, arrowW, r.valueBox.h};
        const bool hover = inside(r.valueBox, mouseX_, mouseY_) && inside(list_, mouseX_, mouseY_);
        box(v, r.valueBox, kField, hover ? kAccent : kFieldBorder);
        const int cur = currentChoice(r, core);
        const auto& o = core.options()[r.setting.keys[0]];
        std::string label = cur >= 0                   ? r.choices[size_t(cur)].label
                            : o.value == o.defaultValue ? std::string("Default")
                                                        : prettyValue(o.value) + " (custom)";
        const int ty = r.valueBox.y + (r.valueBox.h - lh) / 2;
        t.draw(r.prevBox.x + (arrowW - t.width("<")) / 2, ty, "<", kDim);
        t.draw(r.nextBox.x + (arrowW - t.width(">")) / 2, ty, ">", kDim);
        const int maxTextW = valueW - arrowW * 2;
        while (label.size() > 3 && t.width(label) > maxTextW) label = label.substr(0, label.size() - 4) + "...";
        t.draw(r.valueBox.x + (valueW - t.width(label)) / 2, ty, label, kTextC);
        y += rowH_;
    }
    SDL_RenderSetClipRect(ren, nullptr);

    // Scroll bar
    if (maxScroll_ > 0) {
        const int barH = std::max(int(20 * s), listH * listH / contentH);
        const int barY = list_.y + (listH - barH) * scroll_ / maxScroll_;
        v.fillRect(dialog_.x + dw - int(6 * s), barY, int(4 * s), barH, kFieldBorder);
    }

    // Footer: hint for the setting under the mouse, then buttons.
    int fy = list_.y + listH + int(8 * s);
    v.fillRect(dialog_.x + 1, fy - int(4 * s), dw - 2, 1, kFieldBorder);
    std::string hint = !hoverHint.empty() ? hoverHint
                       : pendingRestart_  ? "Click Restart Game to apply settings marked *"
                                          : "Click to change. * = applies after a restart";
    while (hint.size() > 4 && t.width(hint) > dw - pad * 2) hint = hint.substr(0, hint.size() - 4) + "...";
    t.draw(dialog_.x + pad, fy, hint, pendingRestart_ && hoverHint.empty() ? kAccent : kDim);
    fy += lh + int(8 * s);
    const int bh = lh + int(10 * s);
    auto button = [&](SDL_Rect& rect, int x, const std::string& label, bool enabled) {
        rect = {x, fy, std::max(int(80 * s), t.width(label) + int(28 * s)), bh};
        const bool hover = enabled && inside(rect, mouseX_, mouseY_);
        box(v, rect, hover ? kButtonHover : kButtonBg, hover ? kAccent : kFieldBorder);
        t.draw(rect.x + (rect.w - t.width(label)) / 2, fy + (bh - lh) / 2, label, enabled ? kTextC : kDisabledText);
    };
    button(resetBtn_, dialog_.x + pad, "Reset to Defaults", true);
    // Wii U: Cemu's compiled shaders can go stale (parts of games drawn black); this rebuilds them.
    cacheBtn_ = {};
    if (system_ == System::WiiU) button(cacheBtn_, resetBtn_.x + resetBtn_.w + int(8 * s), "Clear Shader Cache", true);
    const int closeW = std::max(int(80 * s), t.width("Close") + int(28 * s));
    button(closeBtn_, dialog_.x + dw - pad - closeW, "Close", true);
    const int restartW = std::max(int(80 * s), t.width("Restart Game") + int(28 * s));
    button(restartBtn_, closeBtn_.x - int(8 * s) - restartW, "Restart Game", pendingRestart_);
}
