#include "buttonconfig.hpp"

#include <algorithm>
#include <cstdlib>

namespace {
// Rows in the order people expect to see them.
const int kRows[Input::kButtons] = {
    RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_A,  RETRO_DEVICE_ID_JOYPAD_B,    RETRO_DEVICE_ID_JOYPAD_X,    RETRO_DEVICE_ID_JOYPAD_Y,
    RETRO_DEVICE_ID_JOYPAD_L,  RETRO_DEVICE_ID_JOYPAD_R,    RETRO_DEVICE_ID_JOYPAD_L2,   RETRO_DEVICE_ID_JOYPAD_R2,
    RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3,   RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_SELECT,
};

const Color kDialogBg{240, 240, 240};
const Color kBorder{120, 120, 120};
const Color kTitleBg{255, 255, 255};
const Color kTextC{0, 0, 0};
const Color kDim{96, 96, 96};
const Color kField{255, 255, 255};
const Color kFieldBorder{173, 173, 173};
const Color kAccent{0, 120, 215};
const Color kListenBg{204, 232, 255};
const Color kButtonBg{225, 225, 225};
const Color kButtonHover{229, 241, 251};

bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

void box(Video& v, const SDL_Rect& r, Color fill, Color border) {
    v.fillRect(r.x, r.y, r.w, r.h, border);
    v.fillRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, fill);
}
}  // namespace

void ButtonConfig::open() {
    open_ = true;
    listenRow_ = listenCol_ = -1;
}

void ButtonConfig::handleEvent(const SDL_Event& e, int mx, int my, Input& input) {
    if (!open_) return;
    const int button = listenRow_ >= 0 ? kRows[listenRow_] : -1;

    switch (e.type) {
    case SDL_MOUSEMOTION:
        mouseX_ = mx;
        mouseY_ = my;
        return;
    case SDL_MOUSEBUTTONDOWN: {
        mouseX_ = mx;
        mouseY_ = my;
        for (int p = 0; p < Input::kMaxPlayers; ++p)
            if (inside(tabs_[p], mx, my)) {
                player_ = p;
                listenRow_ = -1;
                return;
            }
        for (int r = 0; r < Input::kButtons; ++r)
            for (int c = 0; c < 2; ++c)
                if (inside(cells_[r][c], mx, my)) {
                    if (e.button.button == SDL_BUTTON_RIGHT) {  // right-click clears
                        if (c == 0) input.setKey(player_, kRows[r], SDL_SCANCODE_UNKNOWN);
                        else input.setPad(player_, kRows[r], PadBind{});
                        changed_ = true;
                        listenRow_ = -1;
                    } else {
                        listenRow_ = r;
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
            if (listenRow_ >= 0) listenRow_ = -1;
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
        if (listenRow_ >= 0 && listenCol_ == 1) {
            input.setPad(player_, button, {PadBind::Button, uint8_t(e.cbutton.button)});
            changed_ = true;
            listenRow_ = -1;
        }
        return;
    case SDL_CONTROLLERAXISMOTION:
        if (listenRow_ >= 0 && listenCol_ == 1 && std::abs(int(e.caxis.value)) > 24000) {
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
    const int fixedH = titleH + pad + tabH + int(8 * s) + lh + int(8 * s) + lh + int(6 * s) + footerH;
    const int rowH = std::max(lh + 2, std::min(lh + int(9 * s), (H - int(20 * s) - fixedH) / Input::kButtons));

    const int dw = std::min(W - int(16 * s), int(560 * s));
    const int dh = fixedH + rowH * Input::kButtons;
    dialog_ = {(W - dw) / 2, std::max(0, (H - dh) / 2), dw, dh};

    v.fillRect(0, 0, W, H, {0, 0, 0, 120});  // dim the game behind
    box(v, dialog_, kDialogBg, kBorder);
    v.fillRect(dialog_.x + 1, dialog_.y + 1, dialog_.w - 2, titleH, kTitleBg);
    t.draw(dialog_.x + pad, dialog_.y + (titleH - lh) / 2, "Button Config", kTextC);

    // Player tabs
    int y = dialog_.y + titleH + pad;
    int x = dialog_.x + pad;
    for (int p = 0; p < Input::kMaxPlayers; ++p) {
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

    const std::string pad0 = input.controllerName(player_);
    t.draw(dialog_.x + pad, y, "Controller: " + (pad0.empty() ? std::string("none connected") : pad0), kDim);
    y += lh + int(8 * s);

    // Column headers
    const int labelW = int(90 * s);
    const int colW = (dialog_.w - pad * 2 - labelW - int(10 * s)) / 2;
    const int col0 = dialog_.x + pad + labelW, col1 = col0 + colW + int(10 * s);
    t.draw(dialog_.x + pad, y, "Button", kDim);
    t.draw(col0, y, "Keyboard", kDim);
    t.draw(col1, y, "Controller", kDim);
    y += lh + int(6 * s);

    for (int r = 0; r < Input::kButtons; ++r) {
        const int b = kRows[r];
        t.draw(dialog_.x + pad, y + (rowH - lh) / 2, Input::buttonLabel(b), kTextC);
        for (int c = 0; c < 2; ++c) {
            SDL_Rect cell{c == 0 ? col0 : col1, y + 1, colW, rowH - 2};
            cells_[r][c] = cell;
            const bool listening = listenRow_ == r && listenCol_ == c;
            const bool hover = inside(cell, mouseX_, mouseY_);
            box(v, cell, listening ? kListenBg : kField, listening || hover ? kAccent : kFieldBorder);
            std::string label = listening ? (c == 0 ? "Press a key..." : "Press a controller button...")
                                          : (c == 0 ? Input::keyName(input.key(player_, b))
                                                    : Input::padName(input.pad(player_, b)));
            t.draw(cell.x + int(6 * s), cell.y + (cell.h - lh) / 2, label, listening ? kAccent : kTextC);
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
}
