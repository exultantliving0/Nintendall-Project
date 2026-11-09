#include "landialog.hpp"

#include <algorithm>

#include "uistyle.hpp"

using namespace ui;

namespace {

constexpr size_t kMaxName = 20;
constexpr size_t kMaxTarget = 64;

// Removes the last UTF-8 character.
void popChar(std::string& s) {
    while (!s.empty()) {
        const unsigned char c = static_cast<unsigned char>(s.back());
        s.pop_back();
        if ((c & 0xC0) != 0x80) break;
    }
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string fit(Text& t, std::string s, int width) {
    while (s.size() > 3 && t.width(s) > width) s = s.substr(0, s.size() - 4) + "...";
    return s;
}

const char* systemLabel(System s) { return s == System::Wii ? "Wii" : "GameCube"; }

void drawButton(Video& v, Text& t, const SDL_Rect& r, const std::string& label, bool enabled, int mouseX, int mouseY,
                bool primary = false) {
    const bool hover = enabled && inside(r, mouseX, mouseY);
    box(v, r, hover ? kButtonHover : kButtonBg, hover || (primary && enabled) ? kAccent : kFieldBorder);
    const int lh = t.lineHeight();
    t.draw(r.x + (r.w - t.width(label)) / 2, r.y + (r.h - lh) / 2, label, enabled ? kTextC : kDisabledText);
}

}  // namespace

std::vector<std::string> wrapText(Text& t, const std::string& text, int width) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t j = para.find(' ', i);
            if (j == std::string::npos) j = para.size();
            const std::string word = para.substr(i, j - i);
            const std::string tryLine = line.empty() ? word : line + " " + word;
            if (!line.empty() && t.width(tryLine) > width) {
                lines.push_back(line);
                line = word;
            } else {
                line = tryLine;
            }
            i = j + 1;
        }
        lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

// ============================================================================
// Host / Join dialog

void LanDialog::open(Kind kind, std::vector<Game> games, int selected, const std::string& name,
                     const std::string& target, int delay) {
    kind_ = kind;
    games_ = std::move(games);
    selected_ = games_.empty() ? -1 : std::clamp(selected, 0, int(games_.size()) - 1);
    listScroll_ = std::max(0, selected_ - 2);
    name_.text = name.substr(0, kMaxName);
    target_.text = target.substr(0, kMaxTarget);
    delay_ = std::clamp(delay, 1, 10);
    focus_ = 0;
    error_.clear();
    requested_ = false;
    open_ = true;
    SDL_StartTextInput();
}

void LanDialog::close() {
    if (open_) SDL_StopTextInput();
    open_ = false;
}

bool LanDialog::takeRequest(Request& out) {
    if (!requested_) return false;
    requested_ = false;
    out.kind = kind_;
    out.game = games_[size_t(selected_)];
    out.name = trim(name_.text);
    out.target = trim(target_.text);
    out.delay = delay_;
    return true;
}

LanDialog::Field* LanDialog::focusedField() {
    if (kind_ == Kind::Join) return focus_ == 0 ? &target_ : &name_;
    return &name_;
}

void LanDialog::focusNext() { focus_ = kind_ == Kind::Join ? 1 - focus_ : 0; }

void LanDialog::confirm() {
    if (selected_ < 0) {
        error_ = "Put the game in your GameCube or Wii games folder first.";
        return;
    }
    if (kind_ == Kind::Join && trim(target_.text).empty()) {
        error_ = "Type the host code (or the host's address).";
        focus_ = 0;
        return;
    }
    if (trim(name_.text).empty()) name_.text = defaultPlayerName();
    requested_ = true;
    close();
}

void LanDialog::handleEvent(const SDL_Event& e, int mouseX, int mouseY) {
    if (!open_) return;
    mouseX_ = mouseX;
    mouseY_ = mouseY;
    switch (e.type) {
    case SDL_TEXTINPUT: {
        Field* f = focusedField();
        const size_t maxLen = f == &name_ ? kMaxName : kMaxTarget;
        if (f->text.size() + std::char_traits<char>::length(e.text.text) <= maxLen) f->text += e.text.text;
        error_.clear();
        return;
    }
    case SDL_KEYDOWN: {
        const SDL_Keycode k = e.key.keysym.sym;
        const bool ctrl = (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) != 0;
        if (k == SDLK_ESCAPE) close();
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) confirm();
        else if (k == SDLK_TAB) focusNext();
        else if (k == SDLK_BACKSPACE) popChar(focusedField()->text);
        else if (k == SDLK_UP && selected_ > 0) --selected_;
        else if (k == SDLK_DOWN && selected_ + 1 < int(games_.size())) ++selected_;
        else if (ctrl && k == SDLK_v) {
            if (char* clip = SDL_GetClipboardText()) {
                Field* f = focusedField();
                const size_t maxLen = f == &name_ ? kMaxName : kMaxTarget;
                f->text = (f->text + trim(clip)).substr(0, maxLen);
                SDL_free(clip);
            }
        }
        if (k == SDLK_UP || k == SDLK_DOWN) {
            if (selected_ < listScroll_) listScroll_ = selected_;
            if (selected_ >= listScroll_ + listRows_) listScroll_ = selected_ - listRows_ + 1;
        }
        return;
    }
    case SDL_MOUSEWHEEL:
        if (inside(list_, mouseX_, mouseY_))
            listScroll_ = std::clamp(listScroll_ - e.wheel.y, 0, std::max(0, int(games_.size()) - listRows_));
        return;
    case SDL_MOUSEBUTTONDOWN: {
        if (e.button.button != SDL_BUTTON_LEFT) return;
        if (inside(okBtn_, mouseX_, mouseY_)) confirm();
        else if (inside(cancelBtn_, mouseX_, mouseY_)) close();
        else if (inside(target_.box, mouseX_, mouseY_) && kind_ == Kind::Join) focus_ = 0;
        else if (inside(name_.box, mouseX_, mouseY_)) focus_ = kind_ == Kind::Join ? 1 : 0;
        else if (inside(delayPrev_, mouseX_, mouseY_)) delay_ = std::max(1, delay_ - 1);
        else if (inside(delayNext_, mouseX_, mouseY_)) delay_ = std::min(10, delay_ + 1);
        else if (inside(delayBox_, mouseX_, mouseY_)) delay_ = delay_ >= 10 ? 1 : delay_ + 1;
        else if (inside(list_, mouseX_, mouseY_)) {
            const int i = listScroll_ + (mouseY_ - list_.y) / std::max(1, rowH_);
            if (i >= 0 && i < int(games_.size())) selected_ = i;
        }
        return;
    }
    default:
        return;
    }
}

void LanDialog::draw(Video& v, Text& t, int W, int H) {
    if (!open_) return;
    const float s = t.scale();
    const int lh = t.lineHeight();
    const int pad = int(14 * s);
    const int titleH = lh + int(12 * s);
    rowH_ = lh + int(10 * s);
    const int fieldH = lh + int(10 * s);
    const int bh = lh + int(10 * s);
    const bool join = kind_ == Kind::Join;

    const int dw = std::min(W - int(16 * s), int(560 * s));
    // Fit the game list to the window.
    const int fixedH = titleH + pad + (join ? lh + fieldH + pad : 0) + lh + int(4 * s) + pad + lh + fieldH + pad +
                       (join ? 0 : fieldH + pad) + lh * 2 + int(8 * s) + bh + pad;
    listRows_ = std::clamp((H - int(24 * s) - fixedH) / rowH_, 3, 8);
    const int dh = fixedH + listRows_ * rowH_;
    dialog_ = {(W - dw) / 2, std::max(0, (H - dh) / 2), dw, dh};

    v.fillRect(0, 0, W, H, {0, 0, 0, 120});
    box(v, dialog_, kDialogBg, kBorder);
    v.fillRect(dialog_.x + 1, dialog_.y + 1, dialog_.w - 2, titleH, kTitleBg);
    t.draw(dialog_.x + pad, dialog_.y + (titleH - lh) / 2, join ? "Join a LAN Game" : "Host a LAN Game", kTextC);

    const int x = dialog_.x + pad, w = dw - pad * 2;
    int y = dialog_.y + titleH + pad;
    const bool caretOn = (SDL_GetTicks() / 500) % 2 == 0;
    auto field = [&](Field& f, bool focused, const std::string& placeholder) {
        f.box = {x, y, w, fieldH};
        box(v, f.box, kField, focused ? kAccent : kFieldBorder);
        const int ty = y + (fieldH - lh) / 2;
        std::string shown = f.text;
        while (!shown.empty() && t.width(shown) > w - int(16 * s)) shown.erase(0, 1);
        if (shown.empty() && !placeholder.empty() && !focused) t.draw(x + int(6 * s), ty, placeholder, kDisabledText);
        const int tw = t.draw(x + int(6 * s), ty, shown, kTextC);
        if (focused && caretOn) v.fillRect(x + int(6 * s) + tw + 1, ty + 1, std::max(1, int(s)), lh - 2, kTextC);
        y += fieldH + pad;
    };

    if (join) {
        t.draw(x, y, "Host code (or the host's address on the same network)", kTextC);
        y += lh;
        field(target_, focus_ == 0, "e.g. a1b2c3d4");
    }

    // Game list
    t.draw(x, y, join ? "Game (the same one the host picked)" : "Game", kTextC);
    y += lh + int(4 * s);
    list_ = {x, y, w, listRows_ * rowH_};
    box(v, list_, kField, kFieldBorder);
    SDL_Renderer* ren = v.renderer();
    const SDL_Rect clip{list_.x + 1, list_.y + 1, list_.w - 2, list_.h - 2};
    SDL_RenderSetClipRect(ren, &clip);
    if (games_.empty()) {
        int ly = y + int(4 * s);
        for (const std::string& line :
             wrapText(t, "No GameCube or Wii games yet. Put them in your Games folder (File > Open Games Folder).",
                      w - int(16 * s))) {
            t.draw(x + int(8 * s), ly, line, kDim);
            ly += lh;
        }
    }
    listScroll_ = std::clamp(listScroll_, 0, std::max(0, int(games_.size()) - listRows_));
    for (int i = listScroll_; i < int(games_.size()) && i < listScroll_ + listRows_; ++i) {
        const int ry = y + (i - listScroll_) * rowH_;
        const SDL_Rect row{list_.x + 1, ry, list_.w - 2, rowH_};
        if (i == selected_) v.fillRect(row.x, row.y, row.w, row.h, kListenBg);
        else if (inside(row, mouseX_, mouseY_)) v.fillRect(row.x, row.y, row.w, row.h, kButtonHover);
        const std::string sys = systemLabel(games_[size_t(i)].system);
        const int sysW = t.width(sys);
        t.draw(row.x + row.w - sysW - int(8 * s), ry + (rowH_ - lh) / 2, sys, kDim);
        t.draw(row.x + int(8 * s), ry + (rowH_ - lh) / 2, fit(t, games_[size_t(i)].title, row.w - sysW - int(28 * s)),
               kTextC);
    }
    SDL_RenderSetClipRect(ren, nullptr);
    if (int(games_.size()) > listRows_) {
        const int barH = std::max(int(16 * s), list_.h * listRows_ / int(games_.size()));
        const int barY = list_.y + (list_.h - barH) * listScroll_ / std::max(1, int(games_.size()) - listRows_);
        v.fillRect(list_.x + list_.w - int(5 * s), barY, int(3 * s), barH, kFieldBorder);
    }
    y += list_.h + pad;

    t.draw(x, y, "Your name (what the other players see)", kTextC);
    y += lh;
    field(name_, join ? focus_ == 1 : true, "");

    delayPrev_ = delayNext_ = delayBox_ = {};
    if (!join) {
        // Input delay: frames each button press waits so the computers stay in step.
        const std::string label = "Input delay";
        t.draw(x, y + (fieldH - lh) / 2, label, kTextC);
        const int boxW = int(170 * s), arrowW = int(24 * s);
        delayBox_ = {x + t.width(label) + int(16 * s), y, boxW, fieldH};
        delayPrev_ = {delayBox_.x, y, arrowW, fieldH};
        delayNext_ = {delayBox_.x + boxW - arrowW, y, arrowW, fieldH};
        box(v, delayBox_, kField, inside(delayBox_, mouseX_, mouseY_) ? kAccent : kFieldBorder);
        const int ty = y + (fieldH - lh) / 2;
        t.draw(delayPrev_.x + (arrowW - t.width("<")) / 2, ty, "<", kDim);
        t.draw(delayNext_.x + (arrowW - t.width(">")) / 2, ty, ">", kDim);
        const std::string value = std::to_string(delay_) + (delay_ == 1 ? " frame" : " frames");
        t.draw(delayBox_.x + (boxW - t.width(value)) / 2, ty, value, kTextC);
        t.draw(delayBox_.x + boxW + int(10 * s), ty, delay_ <= 3 ? "same network" : delay_ <= 6 ? "online" : "far away",
               kDim);
        y += fieldH + pad;
    }

    // Footer: a hint (or what's wrong), then buttons.
    v.fillRect(dialog_.x + 1, y - pad / 2, dw - 2, 1, kFieldBorder);
    const std::string hint =
        !error_.empty() ? error_
        : join          ? "Everyone needs the same game (region and version) and the same Nintendall version."
                        : "Next, friends join with LAN > Join Game and the host code you'll see.";
    int hy = y;
    for (const std::string& line : wrapText(t, hint, w)) {
        if (hy >= y + lh * 2) break;
        t.draw(x, hy, line, error_.empty() ? kDim : kAccent);
        hy += lh;
    }
    y += lh * 2 + int(8 * s);
    const std::string okLabel = join ? "Join" : "Host";
    const int okW = std::max(int(80 * s), t.width(okLabel) + int(28 * s));
    const int cancelW = std::max(int(80 * s), t.width("Cancel") + int(28 * s));
    cancelBtn_ = {dialog_.x + dw - pad - cancelW, y, cancelW, bh};
    okBtn_ = {cancelBtn_.x - int(8 * s) - okW, y, okW, bh};
    drawButton(v, t, okBtn_, okLabel, selected_ >= 0, mouseX_, mouseY_, true);
    drawButton(v, t, cancelBtn_, "Cancel", true, mouseX_, mouseY_);
}

// ============================================================================
// Lobby

LanLobby::Click LanLobby::handleEvent(const SDL_Event& e, int mouseX, int mouseY, const LanStatus& st,
                                      const Info& info) {
    if (e.type == SDL_KEYDOWN) {
        if (e.key.keysym.sym == SDLK_ESCAPE) return Click::Leave;
        if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)) {
            if (!info.errorText.empty()) return Click::Leave;
            if (startEnabled_) return Click::Start;
        }
        return Click::None;
    }
    if (e.type != SDL_MOUSEBUTTONDOWN || e.button.button != SDL_BUTTON_LEFT) return Click::None;
    if (inside(leaveBtn_, mouseX, mouseY)) return Click::Leave;
    if (startEnabled_ && inside(startBtn_, mouseX, mouseY)) return Click::Start;
    if (copyBtn_.w > 0 && inside(copyBtn_, mouseX, mouseY)) return Click::Copy;
    return Click::None;
}

void LanLobby::draw(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY, const LanStatus& st,
                    const Info& info) {
    const float s = t.scale();
    const int lh = t.lineHeight(), lhL = t.lineHeight(Text::Large);
    const int pad = int(16 * s);
    const int bh = lh + int(10 * s);
    const int dw = std::min(area.w - int(16 * s), int(600 * s));
    const int w = dw - pad * 2;

    // What to show, line by line: {text, color, large}.
    struct Line {
        std::string text;
        Color color;
        bool large = false;
    };
    std::vector<Line> lines;
    auto add = [&](const std::string& text, Color c, bool large = false) {
        for (const std::string& l : wrapText(t, text, w)) lines.push_back({l, c, large});
    };
    auto gap = [&] { lines.push_back({"", kTextC}); };

    bool showCode = false;
    if (!info.errorText.empty()) {
        add(info.errorText, kTextC);
    } else if (st.state == LanState::Connecting || st.state == LanState::Off) {
        add(info.host ? "Opening the game for players..." : "Connecting to " + info.target + "...", kTextC);
    } else {
        if (info.host) {
            if (st.codeState == "ready" && !st.code.empty()) {
                showCode = true;
                add("Friends over the internet join with this host code:", kTextC);
                lines.push_back({st.code, kAccent, true});
            } else if (st.codeState == "failed") {
                add("No host code: Dolphin's server couldn't be reached. Friends on the same network can still join.",
                    kDim);
            } else {
                add("Getting a host code...", kDim);
            }
            if (!info.addresses.empty()) {
                std::string a;
                for (size_t i = 0; i < info.addresses.size() && i < 3; ++i) a += (i ? ",  " : "") + info.addresses[i];
                add("On the same network they can type your address instead: " + a, kDim);
            }
            gap();
        }
        std::string players = st.hostName.empty() ? "" : st.hostName + " (host)";
        for (const std::string& p : st.players) players += (players.empty() ? "" : ",  ") + p;
        const size_t count = st.players.size() + (st.hostName.empty() ? 0 : 1);
        add("Players (" + std::to_string(count) + "):  " + players, kTextC);
        gap();
        if (!st.progress.empty()) add(st.progress, kAccent);
        else if (st.state == LanState::Starting) add("Starting the game...", kAccent);
        else if (info.host) add(st.players.empty() ? "Waiting for players to join..." : "Click Start Game when everyone has joined.", kDim);
        else add("Waiting for " + (st.hostName.empty() ? std::string("the host") : st.hostName) + " to start the game...", kDim);
    }

    const int titleH = lh + int(12 * s);
    int contentH = 0;
    for (const Line& l : lines) contentH += l.large ? lhL + int(6 * s) : lh;
    const int dh = titleH + pad + contentH + pad + bh + pad;
    const SDL_Rect d{area.x + (area.w - dw) / 2, area.y + std::max(0, (area.h - dh) / 2), dw, dh};

    v.fillRect(area.x, area.y, area.w, area.h, {0, 0, 0, 150});
    box(v, d, kDialogBg, kBorder);
    v.fillRect(d.x + 1, d.y + 1, d.w - 2, titleH, kTitleBg);
    t.draw(d.x + pad, d.y + (titleH - lh) / 2, fit(t, "LAN Game - " + info.gameTitle, w), kTextC);

    int y = d.y + titleH + pad;
    copyBtn_ = {};
    for (const Line& l : lines) {
        if (l.large) {
            const int tw = t.draw(d.x + pad, y + int(3 * s), l.text, l.color, Text::Large);
            if (showCode) {
                const int cw = std::max(int(70 * s), t.width("Copy") + int(24 * s));
                copyBtn_ = {d.x + pad + tw + int(16 * s), y + (lhL + int(6 * s) - bh) / 2, cw, bh};
                drawButton(v, t, copyBtn_, "Copy", true, mouseX, mouseY);
            }
            y += lhL + int(6 * s);
        } else {
            t.draw(d.x + pad, y, l.text, l.color);
            y += lh;
        }
    }
    y += pad;

    // Buttons
    startEnabled_ = info.host && info.errorText.empty() && st.state == LanState::Lobby;
    const std::string leave = !info.errorText.empty() ? "OK" : info.host ? "Cancel" : "Leave";
    const int leaveW = std::max(int(80 * s), t.width(leave) + int(28 * s));
    leaveBtn_ = {d.x + dw - pad - leaveW, y, leaveW, bh};
    drawButton(v, t, leaveBtn_, leave, true, mouseX, mouseY, !info.errorText.empty());
    startBtn_ = {};
    if (info.host && info.errorText.empty()) {
        const int sw = std::max(int(110 * s), t.width("Start Game") + int(28 * s));
        startBtn_ = {leaveBtn_.x - int(8 * s) - sw, y, sw, bh};
        drawButton(v, t, startBtn_, "Start Game", startEnabled_, mouseX, mouseY, true);
    }
}
