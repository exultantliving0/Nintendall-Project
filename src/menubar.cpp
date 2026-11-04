#include "menubar.hpp"

#include <algorithm>

namespace {
// Light theme close to standard Windows menus.
const Color kBarBg{240, 240, 240};
const Color kBarLine{218, 218, 218};
const Color kTitleHover{229, 243, 255};
const Color kTitleOpen{204, 232, 255};
const Color kDropBg{249, 249, 249};
const Color kDropBorder{204, 204, 204};
const Color kItemHover{145, 201, 247};
const Color kTextColor{0, 0, 0};
const Color kDisabled{160, 160, 160};
const Color kSeparator{215, 215, 215};

bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
}  // namespace

int MenuBar::height(const Text& text) const { return text.lineHeight() + int(8 * text.scale()); }

bool MenuBar::selectable(int item) const {
    if (open_ < 0 || item < 0 || item >= int(menus_[size_t(open_)].items.size())) return false;
    const MenuItem& it = menus_[size_t(open_)].items[size_t(item)];
    return !it.label.empty() && it.enabled;
}

void MenuBar::moveHover(int dir) {
    if (open_ < 0) return;
    const int n = int(menus_[size_t(open_)].items.size());
    for (int step = 0; step < n; ++step) {
        hover_ = (hover_ < 0 && dir < 0) ? n - 1 : (hover_ + dir + n) % n;
        if (selectable(hover_)) return;
    }
    hover_ = -1;
}

bool MenuBar::handleEvent(const SDL_Event& e, int mx, int my, int& action) {
    action = 0;
    auto titleAt = [&](int x, int y) {
        for (size_t i = 0; i < titleRects_.size(); ++i)
            if (inside(titleRects_[i], x, y)) return int(i);
        return -1;
    };
    auto itemAt = [&](int x, int y) {
        for (size_t i = 0; i < itemRects_.size(); ++i)
            if (inside(itemRects_[i], x, y)) return int(i);
        return -1;
    };

    switch (e.type) {
    case SDL_MOUSEMOTION: {
        hoverTitle_ = titleAt(mx, my);
        if (open_ >= 0) {
            if (hoverTitle_ >= 0 && hoverTitle_ != open_) {  // slide across the bar
                open_ = hoverTitle_;
                hover_ = -1;
                itemRects_.clear();
            }
            const int it = itemAt(mx, my);
            hover_ = selectable(it) ? it : -1;
            return true;
        }
        return my < barHeight_;
    }
    case SDL_MOUSEBUTTONDOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) return open_ >= 0 || my < barHeight_;
        const int t = titleAt(mx, my);
        if (t >= 0) {
            pressedOnTitle_ = true;
            if (open_ == t) close();
            else {
                open_ = t;
                hover_ = -1;
                itemRects_.clear();
            }
            return true;
        }
        if (open_ >= 0) {
            if (!inside(dropRect_, mx, my)) close();  // click outside closes the menu
            return true;
        }
        return my < barHeight_;
    }
    case SDL_MOUSEBUTTONUP: {
        if (e.button.button != SDL_BUTTON_LEFT) return open_ >= 0;
        if (pressedOnTitle_) {
            pressedOnTitle_ = false;
            return true;
        }
        if (open_ >= 0) {
            const int it = itemAt(mx, my);
            if (selectable(it)) {
                action = menus_[size_t(open_)].items[size_t(it)].action;
                close();
            }
            return true;
        }
        return my < barHeight_;
    }
    case SDL_KEYDOWN: {
        if (open_ < 0) return false;
        const int n = int(menus_.size());
        switch (e.key.keysym.sym) {
        case SDLK_ESCAPE: close(); break;
        case SDLK_UP: moveHover(-1); break;
        case SDLK_DOWN: moveHover(1); break;
        case SDLK_LEFT: open_ = (open_ + n - 1) % n; hover_ = -1; itemRects_.clear(); break;
        case SDLK_RIGHT: open_ = (open_ + 1) % n; hover_ = -1; itemRects_.clear(); break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (selectable(hover_)) {
                action = menus_[size_t(open_)].items[size_t(hover_)].action;
                close();
            }
            break;
        default: break;
        }
        return true;  // keys never reach the game while a menu is open
    }
    default:
        return false;
    }
}

void MenuBar::draw(Video& v, Text& text, int width) {
    const float s = text.scale();
    barHeight_ = height(text);
    const int padX = int(9 * s);
    const int textY = (barHeight_ - text.lineHeight()) / 2;

    v.fillRect(0, 0, width, barHeight_, kBarBg);
    v.fillRect(0, barHeight_ - 1, width, 1, kBarLine);

    // Titles
    titleRects_.clear();
    int x = int(2 * s);
    for (size_t i = 0; i < menus_.size(); ++i) {
        const int w = text.width(menus_[i].title) + padX * 2;
        SDL_Rect r{x, 0, w, barHeight_ - 1};
        titleRects_.push_back(r);
        if (int(i) == open_) v.fillRect(r.x, r.y, r.w, r.h, kTitleOpen);
        else if (int(i) == hoverTitle_) v.fillRect(r.x, r.y, r.w, r.h, kTitleHover);
        text.draw(x + padX, textY, menus_[i].title, kTextColor);
        x += w;
    }
    if (open_ < 0 || open_ >= int(menus_.size())) return;

    // Drop-down
    const Menu& m = menus_[size_t(open_)];
    const int itemH = text.lineHeight() + int(9 * s);
    const int sepH = int(7 * s);
    const int gutter = int(28 * s), rightPad = int(22 * s), gap = int(36 * s);
    int labelW = 0, shortW = 0, totalH = int(3 * s) * 2;
    for (const auto& it : m.items) {
        if (it.label.empty()) {
            totalH += sepH;
            continue;
        }
        labelW = std::max(labelW, text.width(it.label));
        if (!it.shortcut.empty()) shortW = std::max(shortW, text.width(it.shortcut));
        totalH += itemH;
    }
    const int dropW = std::max(int(170 * s), gutter + labelW + (shortW ? gap + shortW : 0) + rightPad);
    dropRect_ = {titleRects_[size_t(open_)].x, barHeight_ - 1, dropW, totalH};
    v.fillRect(dropRect_.x + int(2 * s), dropRect_.y + int(2 * s), dropRect_.w, dropRect_.h, {0, 0, 0, 40});  // shadow
    v.fillRect(dropRect_.x, dropRect_.y, dropRect_.w, dropRect_.h, kDropBorder);
    v.fillRect(dropRect_.x + 1, dropRect_.y + 1, dropRect_.w - 2, dropRect_.h - 2, kDropBg);

    itemRects_.clear();
    int y = dropRect_.y + int(3 * s);
    for (size_t i = 0; i < m.items.size(); ++i) {
        const MenuItem& it = m.items[i];
        if (it.label.empty()) {
            itemRects_.push_back({0, 0, 0, 0});
            v.fillRect(dropRect_.x + gutter, y + sepH / 2, dropW - gutter - int(4 * s), 1, kSeparator);
            y += sepH;
            continue;
        }
        SDL_Rect r{dropRect_.x + int(3 * s), y, dropW - int(6 * s), itemH};
        itemRects_.push_back(r);
        if (int(i) == hover_ && it.enabled) v.fillRect(r.x, r.y, r.w, r.h, kItemHover);
        const Color c = it.enabled ? kTextColor : kDisabled;
        const int ty = y + (itemH - text.lineHeight()) / 2;
        if (it.checked) {
            // Check mark: two short strokes.
            const int cx = dropRect_.x + int(10 * s), cy = y + itemH / 2;
            for (int t = 0; t < std::max(2, int(2 * s)); ++t) {
                v.line(cx, cy + t, cx + int(3 * s), cy + int(3 * s) + t, c);
                v.line(cx + int(3 * s), cy + int(3 * s) + t, cx + int(9 * s), cy - int(4 * s) + t, c);
            }
        }
        text.draw(dropRect_.x + gutter, ty, it.label, c);
        if (!it.shortcut.empty())
            text.draw(dropRect_.x + dropW - rightPad - text.width(it.shortcut), ty, it.shortcut, c);
        y += itemH;
    }
}
