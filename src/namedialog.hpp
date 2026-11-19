#pragma once
#include <SDL.h>

#include <string>

#include "text.hpp"
#include "video.hpp"

// A small window that asks for a name: a title, a few words about what it's for, one text box
// (filled in with a name to start from) and OK / Cancel. Used for the 3DS username.
class NameDialog {
public:
    void open(const std::string& title, const std::string& message, const std::string& name, size_t maxLength);
    void close();
    bool isOpen() const { return open_; }
    // OK was clicked (or Enter pressed): the name (true once). Empty names become the one it
    // started with.
    bool takeName(std::string& out);
    // Cancel was clicked (or Esc pressed) (true once).
    bool takeCancel();

    // Modal: uses every event while open. Mouse coordinates in renderer pixels.
    void handleEvent(const SDL_Event& e, int mouseX, int mouseY);
    void draw(Video& v, Text& t, int width, int height);

private:
    void confirm();

    bool open_ = false;
    bool confirmed_ = false, cancelled_ = false;
    std::string title_, message_, text_, start_;
    size_t maxLength_ = 20;
    int mouseX_ = 0, mouseY_ = 0;
    SDL_Rect dialog_{}, field_{}, okBtn_{}, cancelBtn_{};
};
