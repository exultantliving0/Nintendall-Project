#include "namedialog.hpp"

#include <algorithm>

#include "landialog.hpp"  // wrapText
#include "uistyle.hpp"

using namespace ui;

namespace {

// Characters in a UTF-8 string.
size_t length(const std::string& s) {
    size_t n = 0;
    for (const char c : s)
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    return n;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void drawButton(Video& v, Text& t, const SDL_Rect& r, const std::string& label, int mouseX, int mouseY, bool primary) {
    const bool hover = inside(r, mouseX, mouseY);
    box(v, r, hover ? kButtonHover : kButtonBg, hover || primary ? kAccent : kFieldBorder);
    t.draw(r.x + (r.w - t.width(label)) / 2, r.y + (r.h - t.lineHeight()) / 2, label, kTextC);
}

}  // namespace

void NameDialog::open(const std::string& title, const std::string& message, const std::string& name, size_t maxLength) {
    title_ = title;
    message_ = message;
    text_ = name;
    start_ = name;
    maxLength_ = maxLength;
    confirmed_ = cancelled_ = false;
    open_ = true;
    SDL_StartTextInput();
}

void NameDialog::close() {
    if (open_) SDL_StopTextInput();
    open_ = false;
}

bool NameDialog::takeName(std::string& out) {
    if (!confirmed_) return false;
    confirmed_ = false;
    out = trim(text_);
    if (out.empty()) out = start_;
    return true;
}

bool NameDialog::takeCancel() {
    const bool c = cancelled_;
    cancelled_ = false;
    return c;
}

void NameDialog::confirm() {
    confirmed_ = true;
    close();
}

void NameDialog::handleEvent(const SDL_Event& e, int mouseX, int mouseY) {
    if (!open_) return;
    mouseX_ = mouseX;
    mouseY_ = mouseY;
    switch (e.type) {
    case SDL_TEXTINPUT: {
        const std::string add = e.text.text;
        if (length(text_) + length(add) <= maxLength_) text_ += add;
        return;
    }
    case SDL_KEYDOWN: {
        const SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_ESCAPE) {
            cancelled_ = true;
            close();
        } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            confirm();
        } else if (k == SDLK_BACKSPACE) {
            while (!text_.empty()) {
                const unsigned char c = static_cast<unsigned char>(text_.back());
                text_.pop_back();
                if ((c & 0xC0) != 0x80) break;
            }
        } else if ((e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI)) && k == SDLK_v) {
            if (char* clip = SDL_GetClipboardText()) {
                std::string add = trim(clip);
                SDL_free(clip);
                for (size_t i = 0; i < add.size();) {  // whole characters, up to the limit
                    size_t n = 1;
                    while (i + n < add.size() && (static_cast<unsigned char>(add[i + n]) & 0xC0) == 0x80) ++n;
                    if (length(text_) >= maxLength_) break;
                    text_ += add.substr(i, n);
                    i += n;
                }
            }
        }
        return;
    }
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button != SDL_BUTTON_LEFT) return;
        if (inside(okBtn_, mouseX_, mouseY_)) {
            confirm();
        } else if (inside(cancelBtn_, mouseX_, mouseY_)) {
            cancelled_ = true;
            close();
        }
        return;
    default:
        return;
    }
}

void NameDialog::draw(Video& v, Text& t, int W, int H) {
    if (!open_) return;
    const float s = t.scale();
    const int lh = t.lineHeight();
    const int pad = int(14 * s);
    const int titleH = lh + int(12 * s);
    const int fieldH = lh + int(10 * s);
    const int bh = lh + int(10 * s);
    const int dw = std::min(W - int(16 * s), int(460 * s));
    const int w = dw - pad * 2;
    const std::vector<std::string> lines = wrapText(t, message_, w);
    const int dh = titleH + pad + int(lines.size()) * lh + pad + fieldH + pad + bh + pad;
    dialog_ = {(W - dw) / 2, std::max(0, (H - dh) / 2), dw, dh};

    v.fillRect(0, 0, W, H, {0, 0, 0, 120});
    box(v, dialog_, kDialogBg, kBorder);
    v.fillRect(dialog_.x + 1, dialog_.y + 1, dialog_.w - 2, titleH, kTitleBg);
    t.draw(dialog_.x + pad, dialog_.y + (titleH - lh) / 2, title_, kTextC);

    const int x = dialog_.x + pad;
    int y = dialog_.y + titleH + pad;
    for (const std::string& line : lines) {
        t.draw(x, y, line, kTextC);
        y += lh;
    }
    y += pad;

    field_ = {x, y, w, fieldH};
    box(v, field_, kField, kAccent);
    const int ty = y + (fieldH - lh) / 2;
    const int tw = t.draw(x + int(6 * s), ty, text_, kTextC);
    if ((SDL_GetTicks() / 500) % 2 == 0) v.fillRect(x + int(6 * s) + tw + 1, ty + 1, std::max(1, int(s)), lh - 2, kTextC);
    y += fieldH + pad;

    const int cw = std::max(int(80 * s), t.width("Cancel") + int(28 * s));
    const int ow = std::max(int(80 * s), t.width("OK") + int(28 * s));
    cancelBtn_ = {dialog_.x + dw - pad - cw, y, cw, bh};
    okBtn_ = {cancelBtn_.x - int(8 * s) - ow, y, ow, bh};
    drawButton(v, t, okBtn_, "OK", mouseX_, mouseY_, true);
    drawButton(v, t, cancelBtn_, "Cancel", mouseX_, mouseY_, false);
}
