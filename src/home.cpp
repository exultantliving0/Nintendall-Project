#include "home.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace {

const Color kBg{30, 30, 34};
const Color kTabBar{44, 44, 50};
const Color kTabHover{58, 58, 66};
const Color kTabSelected{70, 70, 80};
const Color kRed{230, 0, 18};
const Color kLine{20, 20, 24};
const Color kText{235, 235, 240};
const Color kDim{150, 150, 160};
const Color kTile{52, 52, 60};
const Color kTileBorder{72, 72, 82};
const Color kButton{62, 62, 72};
const Color kButtonHover{82, 82, 94};

bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

void frame(Video& v, const SDL_Rect& r, int t, Color c) {
    v.fillRect(r.x - t, r.y - t, r.w + 2 * t, t, c);
    v.fillRect(r.x - t, r.y + r.h, r.w + 2 * t, t, c);
    v.fillRect(r.x - t, r.y, t, r.h, c);
    v.fillRect(r.x + r.w, r.y, t, r.h, c);
}

// Splits text into at most `maxLines` lines no wider than `width`; the last one ends in "..." if cut.
std::vector<std::string> wrap(Text& t, const std::string& text, int width, int maxLines) {
    std::vector<std::string> words, lines;
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find(' ', a);
        if (b == std::string::npos) b = text.size();
        if (b > a) words.push_back(text.substr(a, b - a));
        a = b + 1;
    }
    std::string line;
    size_t i = 0;
    for (; i < words.size(); ++i) {
        const std::string next = line.empty() ? words[i] : line + " " + words[i];
        if (line.empty() || t.width(next) <= width) {
            line = next;
            continue;
        }
        lines.push_back(line);
        line = words[i];
        if (int(lines.size()) == maxLines) break;
    }
    if (int(lines.size()) < maxLines && !line.empty()) {
        lines.push_back(line);
        line.clear();
        ++i;
    }
    const bool cut = i < words.size() || (!lines.empty() && t.width(lines.back()) > width);
    if (cut && !lines.empty()) {
        std::string& last = lines.back();
        while (!last.empty() && t.width(last + "...") > width) {
            last.pop_back();
            while (!last.empty() && (static_cast<unsigned char>(last.back()) & 0xC0) == 0x80) last.pop_back();  // UTF-8
        }
        while (!last.empty() && last.back() == ' ') last.pop_back();
        last += "...";
    }
    return lines;
}

}  // namespace

void Home::start(const fs::path& gamesRoot, const fs::path& coverCache, const std::string& consoleKey) {
    root_ = gamesRoot;
    createGameFolders(root_);
    covers_.start(coverCache);
    const auto& list = homeConsoles();
    for (size_t i = 0; i < list.size(); ++i)
        if (consoleKey == systemInfo(list[i].system).key) console_ = int(i);
    refresh();
}

void Home::shutdown() { covers_.clear(); }

std::string Home::consoleKey() const { return systemInfo(homeConsoles()[size_t(console_)].system).key; }

fs::path Home::consoleFolder() const { return root_ / fs::u8path(homeConsoles()[size_t(console_)].folder); }

void Home::refresh() {
    std::error_code ec;
    if (!fs::is_directory(consoleFolder(), ec)) createGameFolders(root_);
    games_ = library_.scan(consoleFolder(), homeConsoles()[size_t(console_)].system);
    if (selected_ >= int(games_.size())) selected_ = int(games_.size()) - 1;
    lastScan_ = SDL_GetTicks();
}

void Home::selectConsole(int index) {
    const int n = int(homeConsoles().size());
    console_ = (index % n + n) % n;
    covers_.clear();
    lines_.clear();
    scroll_ = 0;
    pressed_ = -1;
    refresh();
    selected_ = showSelected_ && !games_.empty() ? 0 : -1;
}

// ============================================================================
// Layout

Home::Layout Home::layout(Text& t, const SDL_Rect& area) const {
    Layout L;
    const float s = L.s = t.scale();
    const auto& list = homeConsoles();
    const int n = int(list.size());

    // Console tabs across the top; short names when the window is too narrow for the usual ones.
    const int tabH = int(44 * s), pad = int(14 * s);
    L.tabBar = {area.x, area.y, area.w, tabH};
    for (const bool shortNames : {false, true}) {
        L.labels.clear();
        int total = 0;
        std::vector<int> widths;
        for (const HomeConsole& c : list) {
            L.labels.push_back(shortNames ? c.shortLabel : c.label);
            widths.push_back(t.width(L.labels.back()) + 2 * pad);
            total += widths.back();
        }
        if (total > area.w && !shortNames) continue;
        // Share out the spare room so the tabs fill the bar.
        const int spare = std::max(0, area.w - total);
        L.tabs.clear();
        int x = area.x;
        for (int i = 0; i < n; ++i) {
            const int w = total > area.w ? area.w / n : widths[size_t(i)] + spare / n + (i < spare % n ? 1 : 0);
            L.tabs.push_back({x, area.y, w, tabH});
            x += w;
        }
        break;
    }

    L.margin = int(24 * s);
    L.header = {area.x, area.y + tabH, area.w, int(52 * s)};
    const int bw = t.width("Open Folder") + int(28 * s), bh = t.lineHeight() + int(14 * s);
    L.folderButton = {L.header.x + L.header.w - L.margin - bw, L.header.y + (L.header.h - bh) / 2, bw, bh};
    L.grid = {area.x, L.header.y + L.header.h, area.w, std::max(0, area.y + area.h - L.header.y - L.header.h)};

    L.cardW = int(128 * s);
    L.coverH = int(180 * s);
    L.cardH = L.coverH + int(8 * s) + 2 * t.lineHeight();
    L.gapX = int(22 * s);
    L.gapY = int(22 * s);
    L.cols = std::max(1, (L.grid.w - 2 * L.margin + L.gapX) / (L.cardW + L.gapX));
    L.x0 = L.grid.x + (L.grid.w - (L.cols * L.cardW + (L.cols - 1) * L.gapX)) / 2;
    const int rows = (int(games_.size()) + L.cols - 1) / L.cols;
    const int content = rows == 0 ? 0 : L.margin / 2 + rows * L.cardH + (rows - 1) * L.gapY + L.margin;
    L.maxScroll = std::max(0, content - L.grid.h);
    return L;
}

SDL_Rect Home::cardRect(const Layout& L, int i) const {
    return {L.x0 + (i % L.cols) * (L.cardW + L.gapX),
            L.grid.y + L.margin / 2 + (i / L.cols) * (L.cardH + L.gapY) - scroll_, L.cardW, L.cardH};
}

int Home::cardAt(const Layout& L, int x, int y) const {
    if (!inside(L.grid, x, y)) return -1;
    for (int i = 0; i < int(games_.size()); ++i)
        if (inside(cardRect(L, i), x, y)) return i;
    return -1;
}

void Home::scrollTo(const Layout& L, int i) {
    const SDL_Rect r = cardRect(L, i);
    const int top = L.grid.y + L.margin / 2, bottom = L.grid.y + L.grid.h - L.margin / 2;
    if (r.y < top) scroll_ -= top - r.y;
    else if (r.y + r.h > bottom) scroll_ += r.y + r.h - bottom;
    scroll_ = std::clamp(scroll_, 0, L.maxScroll);
}

void Home::moveSelection(const Layout& L, int dx, int dy) {
    const int n = int(games_.size());
    showSelected_ = true;
    if (n == 0) return;
    if (selected_ < 0) {
        selected_ = 0;
    } else if (dx != 0) {
        selected_ = std::clamp(selected_ + dx, 0, n - 1);
    } else if (dy != 0) {
        const int next = selected_ + dy * L.cols;
        if (next >= 0 && next < n) selected_ = next;
        else if (dy > 0 && selected_ / L.cols < (n - 1) / L.cols) selected_ = n - 1;  // last row is shorter
    }
    scrollTo(L, selected_);
}

const std::vector<std::string>& Home::titleLines(Text& t, const LibraryGame& g, int width, int maxLines) {
    if (linesScale_ != t.scale()) {
        lines_.clear();
        linesScale_ = t.scale();
    }
    const std::string key = std::to_string(width) + "/" + std::to_string(maxLines) + "\n" + g.title;
    auto it = lines_.find(key);
    if (it == lines_.end()) it = lines_.emplace(key, wrap(t, g.title, width, maxLines)).first;
    return it->second;
}

// ============================================================================
// Drawing

void Home::draw(Video& v, Text& t, const SDL_Rect& area, int mx, int my) {
    // Look in the folder again every few seconds, and whenever the home screen comes back.
    const Uint32 now = SDL_GetTicks();
    if (now - lastDraw_ > 500 || now - lastScan_ > 3000) refresh();
    lastDraw_ = now;
    covers_.update(v.renderer());

    const Layout L = layout(t, area);
    const float s = L.s;
    scroll_ = std::clamp(scroll_, 0, L.maxScroll);
    v.fillRect(area.x, area.y, area.w, area.h, kBg);

    // Console tabs
    v.fillRect(L.tabBar.x, L.tabBar.y, L.tabBar.w, L.tabBar.h, kTabBar);
    for (size_t i = 0; i < L.tabs.size(); ++i) {
        const SDL_Rect& r = L.tabs[i];
        const bool sel = int(i) == console_;
        if (sel) v.fillRect(r.x, r.y, r.w, r.h, kTabSelected);
        else if (inside(r, mx, my)) v.fillRect(r.x, r.y, r.w, r.h, kTabHover);
        if (sel) v.fillRect(r.x, r.y + r.h - int(3 * s), r.w, int(3 * s), kRed);
        const int tw = t.width(L.labels[i]);
        t.draw(r.x + (r.w - tw) / 2, r.y + (r.h - t.lineHeight()) / 2, L.labels[i], sel ? kText : kDim);
    }
    v.fillRect(L.tabBar.x, L.tabBar.y + L.tabBar.h, L.tabBar.w, std::max(1, int(s)), kLine);

    // Heading: console name, game count, Open Folder
    const HomeConsole& con = homeConsoles()[size_t(console_)];
    const int hy = L.header.y + (L.header.h - t.lineHeight(Text::Large)) / 2;
    const int nameW = t.draw(L.header.x + L.margin, hy, con.name, kText, Text::Large);
    const std::string count = games_.size() == 1 ? "1 game" : std::to_string(games_.size()) + " games";
    t.draw(L.header.x + L.margin + nameW + int(12 * s), hy + t.lineHeight(Text::Large) - t.lineHeight() - int(2 * s),
           count, kDim);
    const SDL_Rect& b = L.folderButton;
    v.fillRect(b.x, b.y, b.w, b.h, inside(b, mx, my) ? kButtonHover : kButton);
    t.draw(b.x + (b.w - t.width("Open Folder")) / 2, b.y + (b.h - t.lineHeight()) / 2, "Open Folder", kText);

    // No games yet: say where they go.
    if (games_.empty()) {
        const std::string folder = consoleFolder().u8string();
        const std::vector<std::pair<std::string, Color>> msg = {
            {std::string("No ") + con.name + " games yet", kText},
            {"", kText},
            {std::string("Put your ") + con.name + " games in this folder:", kDim},
            {folder, kText},
            {"", kText},
            {"Click Open Folder to go there. New games show up here by themselves.", kDim},
        };
        int y = L.grid.y + std::max(int(30 * s), (L.grid.h - int(msg.size()) * (t.lineHeight() + int(4 * s))) / 3);
        for (const auto& [line, c] : msg) {
            const bool big = &line == &msg[0].first;
            const Text::Size size = big ? Text::Large : Text::Normal;
            if (!line.empty()) t.draw(L.grid.x + (L.grid.w - t.width(line, size)) / 2, y, line, c, size);
            y += t.lineHeight(size) + int(4 * s);
        }
        return;
    }

    // Game covers
    SDL_Renderer* r = v.renderer();
    SDL_RenderSetClipRect(r, &L.grid);
    const int hover = pressed_ >= 0 || inside(L.grid, mx, my) ? cardAt(L, mx, my) : -1;
    for (int i = 0; i < int(games_.size()); ++i) {
        const SDL_Rect card = cardRect(L, i);
        if (card.y + card.h < L.grid.y || card.y > L.grid.y + L.grid.h) continue;
        const LibraryGame& g = games_[size_t(i)];
        const bool hot = i == hover || (showSelected_ && i == selected_);

        SDL_Rect cover{card.x, card.y, L.cardW, L.coverH};
        if (SDL_Texture* tex = covers_.get(g)) {
            // Fit the box inside the cover space, standing on its bottom edge like on a shelf.
            int tw = 0, th = 0;
            SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
            const double k = std::min(double(L.cardW) / tw, double(L.coverH) / th);
            const int w = int(tw * k), h = int(th * k);
            cover = {card.x + (L.cardW - w) / 2, card.y + L.coverH - h, w, h};
            SDL_RenderCopy(r, tex, nullptr, &cover);
        } else {
            // No cover (yet): a plain box with the console and the title.
            v.fillRect(cover.x, cover.y, cover.w, cover.h, kTileBorder);
            v.fillRect(cover.x + 1, cover.y + 1, cover.w - 2, cover.h - 2, kTile);
            const char* sys = systemInfo(g.system).name;
            t.draw(cover.x + (cover.w - t.width(sys)) / 2, cover.y + int(10 * s), sys, kDim);
            const auto& lines = titleLines(t, g, cover.w - int(16 * s), 4);
            int y = cover.y + (cover.h - int(lines.size()) * t.lineHeight()) / 2;
            for (const auto& line : lines) {
                t.draw(cover.x + (cover.w - t.width(line)) / 2, y, line, kText);
                y += t.lineHeight();
            }
        }
        if (hot) frame(v, cover, std::max(2, int(3 * s)), i == hover ? kText : kRed);

        int y = card.y + L.coverH + int(8 * s);
        for (const auto& line : titleLines(t, g, L.cardW, 2)) {
            t.draw(card.x + (L.cardW - t.width(line)) / 2, y, line, hot ? kText : Color{200, 200, 208});
            y += t.lineHeight();
        }
    }
    SDL_RenderSetClipRect(r, nullptr);

    // Scroll bar
    if (L.maxScroll > 0) {
        const int track = L.grid.h - 2 * int(6 * s);
        const int thumb = std::max(int(30 * s), track * L.grid.h / (L.grid.h + L.maxScroll));
        const int y = L.grid.y + int(6 * s) + (track - thumb) * scroll_ / L.maxScroll;
        v.fillRect(L.grid.x + L.grid.w - int(9 * s), y, int(5 * s), thumb, {90, 90, 100});
    }
}

// ============================================================================
// Mouse, keyboard and controller

bool Home::handleEvent(const SDL_Event& e, int mx, int my, Text& t, const SDL_Rect& area, fs::path& launch) {
    const Layout L = layout(t, area);
    const int n = int(games_.size());
    auto play = [&](int i) {
        if (i >= 0 && i < n) launch = games_[size_t(i)].path;
    };

    switch (e.type) {
    case SDL_MOUSEMOTION:
        showSelected_ = false;
        return false;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button != SDL_BUTTON_LEFT || !inside(area, mx, my)) return false;
        for (size_t i = 0; i < L.tabs.size(); ++i)
            if (inside(L.tabs[i], mx, my)) {
                if (int(i) != console_) selectConsole(int(i));
                return true;
            }
        if (inside(L.folderButton, mx, my)) {
            openFolder(consoleFolder());
            return true;
        }
        pressed_ = cardAt(L, mx, my);
        return true;
    case SDL_MOUSEBUTTONUP:
        if (e.button.button != SDL_BUTTON_LEFT || pressed_ < 0) return false;
        if (cardAt(L, mx, my) == pressed_) {
            selected_ = pressed_;
            play(pressed_);
        }
        pressed_ = -1;
        return true;
    case SDL_MOUSEWHEEL: {
        const int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        scroll_ = std::clamp(scroll_ - dy * int(90 * L.s), 0, L.maxScroll);
        return true;
    }
    case SDL_KEYDOWN: {
        if (e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) return false;
        switch (e.key.keysym.sym) {
        case SDLK_LEFT: moveSelection(L, -1, 0); return true;
        case SDLK_RIGHT: moveSelection(L, 1, 0); return true;
        case SDLK_UP: moveSelection(L, 0, -1); return true;
        case SDLK_DOWN: moveSelection(L, 0, 1); return true;
        case SDLK_PAGEUP: moveSelection(L, 0, -std::max(1, L.grid.h / (L.cardH + L.gapY))); return true;
        case SDLK_PAGEDOWN: moveSelection(L, 0, std::max(1, L.grid.h / (L.cardH + L.gapY))); return true;
        case SDLK_HOME:
            if (n) selected_ = 0, showSelected_ = true, scrollTo(L, 0);
            return true;
        case SDLK_END:
            if (n) selected_ = n - 1, showSelected_ = true, scrollTo(L, n - 1);
            return true;
        case SDLK_TAB: selectConsole(console_ + ((e.key.keysym.mod & KMOD_SHIFT) ? -1 : 1)); return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (showSelected_) play(selected_);
            else moveSelection(L, 0, 0);
            return true;
        case SDLK_F5: refresh(); return true;
        default: return false;
        }
    }
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: moveSelection(L, -1, 0); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: moveSelection(L, 1, 0); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: moveSelection(L, 0, -1); return true;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: moveSelection(L, 0, 1); return true;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: showSelected_ = true; selectConsole(console_ - 1); return true;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: showSelected_ = true; selectConsole(console_ + 1); return true;
        case SDL_CONTROLLER_BUTTON_A:
        case SDL_CONTROLLER_BUTTON_START:
            if (showSelected_ && selected_ >= 0) play(selected_);
            else moveSelection(L, 0, 0);
            return true;
        default: return false;
        }
    case SDL_CONTROLLERAXISMOTION: {
        // The left stick moves one step per push.
        const int dir = e.caxis.value > 20000 ? 1 : e.caxis.value < -20000 ? -1 : 0;
        int* last = e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX ? &stickX_ : e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY ? &stickY_ : nullptr;
        if (!last || dir == *last) return false;
        *last = dir;
        if (dir != 0) {
            if (last == &stickX_) moveSelection(L, dir, 0);
            else moveSelection(L, 0, dir);
        }
        return true;
    }
    default:
        return false;
    }
}
