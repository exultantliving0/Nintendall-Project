#include "buttonconfig.hpp"

#include "uistyle.hpp"

#include <algorithm>
#include <cstdlib>

using namespace ui;

void ButtonConfig::open(System system, const std::vector<ButtonLabel>& rows) {
    system_ = system;
    rows_ = rows;
    players_ = systemPlayers(system);
    if (player_ >= players_) player_ = 0;
    cells_.assign(rows_.size(), {});
    open_ = true;
    dropOpen_ = false;
    listenRow_ = listenCol_ = -1;
}

// `s` shortened with "..." to fit in `maxW` pixels.
static std::string fit(Text& t, std::string s, int maxW) {
    while (s.size() > 4 && t.width(s) > maxW) s = s.substr(0, s.size() - 4) + "...";
    return s;
}

// Small downward triangle (the drop-down arrow), centered at (cx, cy).
static void drawArrow(Video& v, int cx, int cy, int size, Color c) {
    for (int i = 0; i < size; ++i) v.fillRect(cx - size + i + 1, cy - size / 2 + i, (size - i) * 2 - 1, 1, c);
}

void ButtonConfig::handleEvent(const SDL_Event& e, int mx, int my, Input& input) {
    if (!open_) return;
    const int button = listenRow_ >= 0 ? rows_[size_t(listenRow_)].id : -1;

    switch (e.type) {
    case SDL_MOUSEMOTION:
        mouseX_ = mx;
        mouseY_ = my;
        return;
    case SDL_MOUSEBUTTONDOWN: {
        mouseX_ = mx;
        mouseY_ = my;
        if (dropOpen_) {  // the open list is on top: a click picks an item or closes it
            dropOpen_ = false;
            for (size_t i = 0; i < dropItems_.size() && i < dropIds_.size(); ++i)
                if (inside(dropItems_[i], mx, my)) {
                    input.assignController(player_, dropIds_[i]);
                    changed_ = true;
                }
            return;
        }
        if (inside(dropBox_, mx, my) && e.button.button == SDL_BUTTON_LEFT) {
            dropOpen_ = true;
            listenRow_ = -1;
            return;
        }
        for (int p = 0; p < players_; ++p)
            if (inside(tabs_[p], mx, my)) {
                player_ = p;
                dropOpen_ = false;
                listenRow_ = -1;
                return;
            }
        for (size_t r = 0; r < rows_.size(); ++r)
            for (int c = 0; c < 2; ++c)
                if (inside(cells_[r][size_t(c)], mx, my)) {
                    if (e.button.button == SDL_BUTTON_RIGHT) {  // right-click clears
                        if (c == 0) input.setKey(player_, rows_[r].id, SDL_SCANCODE_UNKNOWN);
                        else input.setPad(player_, rows_[r].id, PadBind{});
                        changed_ = true;
                        listenRow_ = -1;
                    } else {
                        listenRow_ = int(r);
                        listenCol_ = c;
                    }
                    return;
                }
        if (inside(resetBtn_, mx, my)) {
            input.resetDefaults(player_);
            changed_ = true;
            listenRow_ = -1;
        } else if (inside(closeBtn_, mx, my)) {
            close();
        } else {
            listenRow_ = -1;
        }
        return;
    }
    case SDL_KEYDOWN:
        if (e.key.repeat) return;
        if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
            if (dropOpen_) dropOpen_ = false;
            else if (listenRow_ >= 0) listenRow_ = -1;
            else close();
            return;
        }
        if (listenRow_ >= 0 && listenCol_ == 0) {
            input.setKey(player_, button, e.key.keysym.scancode);
            changed_ = true;
            listenRow_ = -1;
        }
        return;
    case SDL_CONTROLLERBUTTONDOWN:
        if (listenRow_ >= 0 && listenCol_ == 1 && input.portForController(e.cbutton.which) == player_) {
            input.setPad(player_, button, {PadBind::Button, uint8_t(e.cbutton.button)});
            changed_ = true;
            listenRow_ = -1;
        }
        return;
    case SDL_CONTROLLERAXISMOTION:
        if (listenRow_ >= 0 && listenCol_ == 1 && std::abs(int(e.caxis.value)) > 24000 &&
            input.portForController(e.caxis.which) == player_) {
            input.setPad(player_, button, {e.caxis.value > 0 ? PadBind::AxisPos : PadBind::AxisNeg, e.caxis.axis});
            changed_ = true;
            listenRow_ = -1;
        }
        return;
    default:
        return;
    }
}

void ButtonConfig::draw(Video& v, Text& t, const Input& input, int W, int H) {
    if (!open_) return;
    const float s = t.scale();
    const int lh = t.lineHeight();
    const int pad = int(14 * s);
    const int titleH = lh + int(12 * s);
    const int tabH = lh + int(10 * s);
    const int footerH = lh + int(24 * s) + lh + int(10 * s);
    const int fixedH = titleH + pad + tabH + int(8 * s) + lh + int(16 * s) + lh + int(6 * s) + footerH;  // tabs, drop-down, headers
    const int rowCount = std::max(1, int(rows_.size()));
    const int rowH = std::max(lh + 2, std::min(lh + int(9 * s), (H - int(20 * s) - fixedH) / rowCount));

    const int dw = std::min(W - int(16 * s), int(560 * s));
    const int dh = fixedH + rowH * rowCount;
    dialog_ = {(W - dw) / 2, std::max(0, (H - dh) / 2), dw, dh};

    v.fillRect(0, 0, W, H, {0, 0, 0, 120});  // dim the game behind
    box(v, dialog_, kDialogBg, kBorder);
    v.fillRect(dialog_.x + 1, dialog_.y + 1, dialog_.w - 2, titleH, kTitleBg);
    const std::string title =
        system_ == System::Unknown ? "Button Config" : std::string("Button Config - ") + systemInfo(system_).fullName;
    t.draw(dialog_.x + pad, dialog_.y + (titleH - lh) / 2, title, kTextC);

    // Player tabs
    int y = dialog_.y + titleH + pad;
    int x = dialog_.x + pad;
    for (int p = 0; p < players_; ++p) {
        const std::string label = "Player " + std::to_string(p + 1);
        const int w = t.width(label) + int(24 * s);
        tabs_[p] = {x, y, w, tabH};
        const bool sel = p == player_;
        box(v, tabs_[p], sel ? kListenBg : (inside(tabs_[p], mouseX_, mouseY_) ? kButtonHover : kButtonBg),
            sel ? kAccent : kFieldBorder);
        t.draw(x + int(12 * s), y + (tabH - lh) / 2, label, kTextC);
        x += w + int(6 * s);
    }
    y += tabH + int(8 * s);

    // Controller drop-down
    const auto pads = input.controllers();
    const std::string padLabel = "Controller:";
    const int boxH = lh + int(8 * s);
    t.draw(dialog_.x + pad, y + (boxH - lh) / 2, padLabel, kTextC);
    const int boxX = dialog_.x + pad + t.width(padLabel) + int(10 * s);
    dropBox_ = {boxX, y, dialog_.x + dialog_.w - pad - boxX, boxH};
    std::string current = input.controllerName(player_);
    if (current.empty())
        current = input.waitingForController(player_) ? "Chosen controller isn't connected"
                : input.keyboardOnly(player_) || !pads.empty() ? "None (keyboard only)"
                : "None connected";
    const bool boxHover = inside(dropBox_, mouseX_, mouseY_) && !dropOpen_;
    box(v, dropBox_, kField, dropOpen_ || boxHover ? kAccent : kFieldBorder);
    const int arrowSpace = int(22 * s);
    t.draw(dropBox_.x + int(6 * s), y + (boxH - lh) / 2, fit(t, current, dropBox_.w - int(12 * s) - arrowSpace),
           pads.empty() && current == "None connected" ? kDim : kTextC);
    drawArrow(v, dropBox_.x + dropBox_.w - arrowSpace / 2, y + boxH / 2, std::max(3, int(4 * s)), kDim);
    y += boxH + int(8 * s);

    // Column headers
    int labelW = int(90 * s);
    for (const auto& row : rows_) labelW = std::max(labelW, t.width(row.label) + int(16 * s));
    const int colW = (dialog_.w - pad * 2 - labelW - int(10 * s)) / 2;
    const int col0 = dialog_.x + pad + labelW, col1 = col0 + colW + int(10 * s);
    t.draw(dialog_.x + pad, y, "Button", kDim);
    t.draw(col0, y, "Keyboard", kDim);
    t.draw(col1, y, "Controller", kDim);
    y += lh + int(6 * s);

    for (size_t r = 0; r < rows_.size(); ++r) {
        const int b = rows_[r].id;
        t.draw(dialog_.x + pad, y + (rowH - lh) / 2, rows_[r].label, kTextC);
        for (int c = 0; c < 2; ++c) {
            SDL_Rect cell{c == 0 ? col0 : col1, y + 1, colW, rowH - 2};
            cells_[r][size_t(c)] = cell;
            const bool listening = listenRow_ == int(r) && listenCol_ == c;
            const bool hover = inside(cell, mouseX_, mouseY_);
            box(v, cell, listening ? kListenBg : kField, listening || hover ? kAccent : kFieldBorder);
            std::string label = listening ? (c == 0 ? "Press a key..."
                                             : input.controllerForPort(player_) ? "Press a controller button..."
                                                                                : "Pick a controller above first")
                              : (c == 0 ? Input::keyName(input.key(player_, b)) : Input::padName(input.pad(player_, b)));
            t.draw(cell.x + int(6 * s), cell.y + (cell.h - lh) / 2, label,
                   listening ? kAccent : kTextC);
        }
        y += rowH;
    }

    // Footer
    y += int(10 * s);
    t.draw(dialog_.x + pad, y, "Click a box, then press a key or controller button. Right-click clears it.", kDim);
    y += lh + int(10 * s);
    const int bh = lh + int(10 * s);
    const std::string resetLabel = "Reset to Defaults", closeLabel = "Close";
    resetBtn_ = {dialog_.x + pad, y, t.width(resetLabel) + int(28 * s), bh};
    closeBtn_ = {dialog_.x + dialog_.w - pad - std::max(int(80 * s), t.width(closeLabel) + int(28 * s)), y,
                 std::max(int(80 * s), t.width(closeLabel) + int(28 * s)), bh};
    for (const auto* b : {&resetBtn_, &closeBtn_}) {
        const bool hover = inside(*b, mouseX_, mouseY_);
        box(v, *b, hover ? kButtonHover : kButtonBg, hover ? kAccent : kFieldBorder);
    }
    t.draw(resetBtn_.x + (resetBtn_.w - t.width(resetLabel)) / 2, y + (bh - lh) / 2, resetLabel, kTextC);
    t.draw(closeBtn_.x + (closeBtn_.w - t.width(closeLabel)) / 2, y + (bh - lh) / 2, closeLabel, kTextC);

    // Open drop-down list, drawn last so it covers the rows below it.
    dropItems_.clear();
    dropIds_.clear();
    if (dropOpen_) {
        std::vector<std::string> labels;
        for (const auto& c : pads) {
            labels.push_back(c.player >= 0 && c.player != player_
                                 ? c.name + "  (Player " + std::to_string(c.player + 1) + ")"
                                 : c.name);
            dropIds_.push_back(c.id);
        }
        labels.push_back("None (keyboard only)");
        dropIds_.push_back(-1);
        const int itemH = lh + int(8 * s);
        SDL_Rect list{dropBox_.x, dropBox_.y + dropBox_.h - 1, dropBox_.w, itemH * int(labels.size()) + 2};
        box(v, list, kField, kAccent);
        const int selPort = player_;
        for (size_t i = 0; i < labels.size(); ++i) {
            SDL_Rect item{list.x + 1, list.y + 1 + itemH * int(i), list.w - 2, itemH};
            dropItems_.push_back(item);
            const bool selected = dropIds_[i] < 0 ? !input.controllerForPort(selPort) && input.keyboardOnly(selPort)
                                                  : input.portForController(dropIds_[i]) == selPort;
            const bool hover = inside(item, mouseX_, mouseY_);
            if (hover) v.fillRect(item.x, item.y, item.w, item.h, kAccent);
            else if (selected) v.fillRect(item.x, item.y, item.w, item.h, kListenBg);
            t.draw(item.x + int(6 * s), item.y + (itemH - lh) / 2, fit(t, labels[i], item.w - int(12 * s)),
                   hover ? Color{255, 255, 255} : kTextC);
        }
    }
}
