#pragma once
#include <SDL.h>

#include "video.hpp"

// Shared look for Nintendall's small dialog windows (Button Config, Console Settings).
namespace ui {

inline const Color kDialogBg{240, 240, 240};
inline const Color kBorder{120, 120, 120};
inline const Color kTitleBg{255, 255, 255};
inline const Color kTextC{0, 0, 0};
inline const Color kDim{96, 96, 96};
inline const Color kDisabledText{160, 160, 160};
inline const Color kField{255, 255, 255};
inline const Color kFieldBorder{173, 173, 173};
inline const Color kAccent{0, 120, 215};
inline const Color kListenBg{204, 232, 255};
inline const Color kButtonBg{225, 225, 225};
inline const Color kButtonHover{229, 241, 251};

inline bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

inline void box(Video& v, const SDL_Rect& r, Color fill, Color border) {
    v.fillRect(r.x, r.y, r.w, r.h, border);
    v.fillRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, fill);
}

}  // namespace ui
