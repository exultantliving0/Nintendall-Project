#include "consolesettings.hpp"

#include <cctype>

namespace {

using Choices = std::vector<std::pair<std::string, std::string>>;  // value, label

// A setting backed by a single core option.
Setting opt(const std::string& label, const std::string& key, const Choices& choices, bool restart = false,
            const std::string& hint = "") {
    Setting s;
    s.label = label;
    s.keys = {key};
    for (const auto& [value, text] : choices) s.choices.push_back({text, {{key, value}}});
    s.restart = restart;
    s.hint = hint;
    return s;
}

// A setting that lists whatever values the core offers.
Setting raw(const std::string& label, const std::string& key, bool restart = false, const std::string& hint = "") {
    Setting s;
    s.label = label;
    s.keys = {key};
    s.restart = restart;
    s.hint = hint;
    return s;
}

const Choices kOnOff = {{"disabled", "Off"}, {"enabled", "On"}};
const Choices kOffOnCaps = {{"OFF", "Off"}, {"ON", "On"}};
const Choices kFalseTrue = {{"False", "Off"}, {"True", "On"}};

Choices percent(const std::vector<int>& values, int normal = -1) {
    Choices c;
    for (int v : values) c.push_back({std::to_string(v), std::to_string(v) + "%" + (v == normal ? " (normal)" : "")});
    return c;
}

// ---------------------------------------------------------------------------

std::vector<SettingGroup> nes(bool fds) {
    Setting edges;
    edges.label = "Hide Screen Edges";
    edges.keys = {"mesen_overscan_up", "mesen_overscan_down", "mesen_overscan_left", "mesen_overscan_right"};
    edges.choices = {
        {"Off", {{"mesen_overscan_up", "None"}, {"mesen_overscan_down", "None"}, {"mesen_overscan_left", "None"}, {"mesen_overscan_right", "None"}}},
        {"Top and bottom", {{"mesen_overscan_up", "8px"}, {"mesen_overscan_down", "8px"}, {"mesen_overscan_left", "None"}, {"mesen_overscan_right", "None"}}},
        {"All edges", {{"mesen_overscan_up", "8px"}, {"mesen_overscan_down", "8px"}, {"mesen_overscan_left", "8px"}, {"mesen_overscan_right", "8px"}}},
    };
    edges.hint = "Hides the glitchy border some games draw";

    std::vector<SettingGroup> g = {
        {"Picture",
         {raw("Color Palette", "mesen_palette"),
          opt("TV Filter", "mesen_ntsc_filter",
              {{"Disabled", "Off"}, {"Composite (Blargg)", "Composite TV"}, {"S-Video (Blargg)", "S-Video TV"},
               {"RGB (Blargg)", "RGB monitor"}, {"Monochrome (Blargg)", "Black and white TV"}, {"Bisqwit 2x", "Bisqwit (sharp)"}}),
          opt("Aspect Ratio", "mesen_aspect_ratio",
              {{"Auto", "Auto"}, {"NTSC", "TV (NTSC)"}, {"PAL", "TV (PAL)"}, {"4:3", "4:3"}, {"No Stretching", "Square pixels"}}),
          edges,
          opt("Remove Sprite Limit", "mesen_nospritelimit", kOnOff, false, "Less flickering when lots is on screen")}},
        {"Console",
         {opt("Overclock", "mesen_overclock",
              {{"None", "Off"}, {"Low", "Low"}, {"Medium", "Medium"}, {"High", "High"}, {"Very High", "Very high"}}, false,
              "Reduces slowdown in busy scenes"),
          raw("Region", "mesen_region", true)}},
        {"Sound", {opt("Stereo Effect", "mesen_fake_stereo", kOnOff)}},
    };
    if (fds)
        g.push_back({"Disk System",
                     {opt("Insert Disks Automatically", "mesen_fdsautoinsertdisk", kOnOff),
                      opt("Fast-Forward Disk Loading", "mesen_fdsfastforwardload", kOnOff)}});
    return g;
}

std::vector<SettingGroup> snes() {
    return {
        {"Picture",
         {opt("HD Mode 7", "bsnes_mode7_scale",
              {{"1x", "Off"}, {"2x", "2x"}, {"3x", "3x"}, {"4x", "4x"}, {"6x", "6x"}, {"8x", "8x"}}, false,
              "Sharper 3D floors (Mario Kart, F-Zero, Pilotwings)"),
          opt("HD Mode 7 Supersampling", "bsnes_mode7_supersample", kOffOnCaps),
          opt("TV Filter", "bsnes_video_filter",
              {{"None", "Off"}, {"NTSC (RF)", "RF (antenna)"}, {"NTSC (Composite)", "Composite TV"},
               {"NTSC (S-Video)", "S-Video TV"}, {"NTSC (RGB)", "RGB monitor"}}),
          opt("Blur Emulation", "bsnes_blur_emulation", kOffOnCaps, false, "Softens dithered transparency like a real TV"),
          opt("Aspect Ratio", "bsnes_aspect_ratio",
              {{"Auto", "Auto"}, {"4:3", "4:3"}, {"NTSC", "TV (NTSC)"}, {"PAL", "TV (PAL)"}, {"1:1", "Square pixels"}}),
          opt("Crop Top and Bottom", "bsnes_ppu_overscan_v", {{"0", "Off"}, {"8", "8 px"}, {"12", "12 px"}, {"16", "16 px"}}),
          opt("Remove Sprite Limit", "bsnes_ppu_no_sprite_limit", kOffOnCaps, false, "Less flickering")}},
        {"Performance",
         {opt("Run-Ahead", "bsnes_run_ahead_frames",
              {{"OFF", "Off"}, {"1", "1 frame"}, {"2", "2 frames"}, {"3", "3 frames"}, {"4", "4 frames"}}, false,
              "Cuts input lag; uses more CPU"),
          opt("CPU Overclock", "bsnes_cpu_overclock", percent({100, 120, 150, 200, 300}, 100), false,
              "Reduces slowdown (Gradius III, Super R-Type)"),
          opt("Super FX Overclock", "bsnes_cpu_sfx_overclock", percent({100, 200, 300, 400, 800}, 100), false,
              "Smoother Star Fox, Stunt Race FX, Yoshi's Island")}},
    };
}

std::vector<SettingGroup> gameBoy() {
    return {
        {"Picture",
         {opt("Game Boy Palette", "sameboy_mono_palette",
              {{"greyscale", "Grey"}, {"lime", "Green (original)"}, {"olive", "Olive"}, {"teal", "Teal"}}, false,
              "Colors for original Game Boy games"),
          raw("Color Correction", "sameboy_color_correction_mode", false, "How Game Boy Color colors are shown"),
          opt("Border", "sameboy_border",
              {{"Super Game Boy only", "Super Game Boy games"}, {"always", "Always"}, {"never", "Never"}})}},
        {"Console",
         {opt("Console", "sameboy_model",
              {{"Auto", "Auto"}, {"Auto (SGB)", "Super Game Boy (colors + borders)"}, {"Game Boy", "Game Boy"},
               {"Game Boy Pocket", "Game Boy Pocket"}, {"Game Boy Color", "Game Boy Color"},
               {"Game Boy Advance", "Game Boy Advance"}},
              true),
          opt("Rumble", "sameboy_rumble",
              {{"rumble-enabled games", "Games with rumble"}, {"all games", "All games"}, {"never", "Off"}})}},
    };
}

std::vector<SettingGroup> gba() {
    return {
        {"Picture",
         {opt("Color Correction", "mgba_color_correction", {{"OFF", "Off"}, {"GBA", "GBA screen"}, {"GBC", "GBC screen"}, {"Auto", "Auto"}}),
          opt("LCD Ghosting", "mgba_interframe_blending",
              {{"OFF", "Off"}, {"mix", "Blend frames"}, {"mix_smart", "Smart blend"}, {"lcd_ghosting", "LCD ghosting"},
               {"lcd_ghosting_fast", "LCD ghosting (fast)"}},
              false, "Some games flicker on purpose for see-through effects")}},
        {"Console",
         {raw("Solar Sensor Level", "mgba_solar_sensor_level", false, "For Boktai games"),
          opt("Skip BIOS Intro", "mgba_skip_bios", kOffOnCaps, true),
          opt("Game Boy Player Rumble", "mgba_force_gbp", kOffOnCaps, true)}},
        {"Sound", {opt("Smooth Sound", "mgba_audio_low_pass_filter", kOnOff)}},
    };
}

std::vector<SettingGroup> ds() {
    return {
        {"Picture",
         {opt("Renderer", "melonds_render_mode", {{"software", "Software (accurate)"}, {"opengl", "OpenGL (allows upscaling)"}}, true),
          opt("3D Resolution", "melonds_opengl_resolution",
              {{"1", "Native (256x192)"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"5", "5x"}, {"6", "6x"}, {"7", "7x"}, {"8", "8x"}},
              true, "Needs the OpenGL renderer"),
          opt("Better Polygons", "melonds_opengl_better_polygons", kOnOff, true, "OpenGL renderer only"),
          opt("Gap Between Screens", "melonds_screen_gap",
              {{"0", "None"}, {"8", "8 px"}, {"16", "16 px"}, {"32", "32 px"}, {"48", "48 px"}, {"64", "64 px"}, {"90", "90 px"}}),
          opt("Big Screen Size", "melonds_hybrid_ratio", {{"2", "2x"}, {"3", "3x"}}, false, "For the Large + Small layout"),
          opt("Stylus Cursor", "melonds_show_cursor",
              {{"timeout", "Briefly after moving"}, {"touching", "While touching"}, {"always", "Always"}, {"disabled", "Hidden"}})}},
        {"Console", {opt("Speed Boost (JIT)", "melonds_jit_enable", kOnOff, true)}},
        {"Sound",
         {opt("Sound Smoothing", "melonds_audio_interpolation",
              {{"disabled", "Off"}, {"linear", "Linear"}, {"cosine", "Cosine"}, {"cubic", "Cubic"}, {"gaussian", "Gaussian"}})}},
    };
}

std::vector<SettingGroup> n3ds() {
    return {
        {"Picture",
         {opt("Resolution", "citra_resolution_factor",
              {{"1", "Native (400x240)"}, {"2", "2x (800x480)"}, {"3", "3x (1200x720)"}, {"4", "4x (1600x960)"},
               {"5", "5x"}, {"6", "6x"}, {"8", "8x"}, {"10", "10x"}},
              true),
          opt("Texture Filter", "citra_texture_filter",
              {{"none", "Off"}, {"Anime4K", "Anime4K"}, {"Bicubic", "Bicubic"}, {"ScaleForce", "ScaleForce"},
               {"xBRZ", "xBRZ"}, {"MMPX", "MMPX"}},
              true),
          opt("Texture Sampling", "citra_texture_sampling",
              {{"GameControlled", "Game decides"}, {"NearestNeighbor", "Sharp"}, {"Linear", "Smooth"}}, true),
          opt("Stereoscopic 3D", "citra_render_3d",
              {{"off", "Off"}, {"anaglyph", "Red/cyan glasses"}, {"side-by-side", "Side by side"}, {"interlaced", "Interlaced"}},
              true),
          opt("3D Depth", "citra_factor_3d", percent({0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100})),
          opt("Big Screen Size", "citra_large_screen_proportion",
              {{"1.50", "1.5x"}, {"2.00", "2x"}, {"2.50", "2.5x"}, {"3.00", "3x"}, {"4.00", "4x"}, {"5.00", "5x"}}, false,
              "For the Large + Small layout")}},
        {"Console",
         {opt("Model", "citra_is_new_3ds", {{"New 3DS", "New 3DS"}, {"Old 3DS", "Old 3DS"}}, true),
          raw("Region", "citra_region_value", true),
          raw("System Language", "citra_language_value", true),
          opt("CPU Speed", "citra_cpu_clock_percentage", percent({50, 75, 100, 125, 150, 200, 300, 400}, 100), true,
              "Higher can fix slowdown; too high can break games")}},
        {"Controls",
         {opt("Right Stick", "citra_analog_function",
              {{"c_stick_and_touchscreen", "C-Stick + touch pointer"}, {"c_stick", "C-Stick"}, {"touchscreen_pointer", "Touch pointer"}},
              true),
          opt("Stick Deadzone", "citra_analog_deadzone", percent({0, 5, 10, 15, 20, 25, 30, 35}), true)}},
    };
}

std::vector<SettingGroup> n64() {
    Setting res;
    res.label = "Resolution";
    res.keys = {"mupen64plus-43screensize", "mupen64plus-169screensize"};
    res.choices = {
        {"Native (240p)", {{"mupen64plus-43screensize", "320x240"}, {"mupen64plus-169screensize", "640x360"}}},
        {"2x (480p)", {{"mupen64plus-43screensize", "640x480"}, {"mupen64plus-169screensize", "960x540"}}},
        {"3x (720p)", {{"mupen64plus-43screensize", "960x720"}, {"mupen64plus-169screensize", "1280x720"}}},
        {"4x (960p)", {{"mupen64plus-43screensize", "1280x960"}, {"mupen64plus-169screensize", "1920x1080"}}},
        {"6x (1440p)", {{"mupen64plus-43screensize", "1920x1440"}, {"mupen64plus-169screensize", "2560x1440"}}},
        {"9x (4K)", {{"mupen64plus-43screensize", "2880x2160"}, {"mupen64plus-169screensize", "3840x2160"}}},
    };
    res.restart = true;
    res.hint = "Renders 3D sharper than a real N64";

    const Choices pak = {{"memory", "Controller Pak (saves)"}, {"rumble", "Rumble Pak"}, {"none", "Nothing"}};
    return {
        {"Picture",
         {res,
          opt("Widescreen", "mupen64plus-aspect", {{"4:3", "Off (4:3)"}, {"16:9 adjusted", "On (16:9 hack)"}}, true,
              "Shows more of the 3D world; some games glitch"),
          opt("Anti-Aliasing", "mupen64plus-MultiSampling",
              {{"0", "Off"}, {"2", "2x MSAA"}, {"4", "4x MSAA"}, {"8", "8x MSAA"}, {"16", "16x MSAA"}}, true,
              "Smooths jagged edges"),
          opt("FXAA", "mupen64plus-FXAA", {{"0", "Off"}, {"1", "On"}}, true),
          opt("Texture Filter", "mupen64plus-txFilterMode",
              {{"None", "Off"}, {"Smooth filtering 1", "Smooth 1"}, {"Smooth filtering 2", "Smooth 2"},
               {"Smooth filtering 3", "Smooth 3"}, {"Smooth filtering 4", "Smooth 4"},
               {"Sharp filtering 1", "Sharp 1"}, {"Sharp filtering 2", "Sharp 2"}},
              true),
          opt("Texture Enhancement", "mupen64plus-txEnhancementMode",
              {{"None", "Off"}, {"X2SAI", "2xSaI"}, {"HQ2X", "HQ2x"}, {"HQ4X", "HQ4x"}, {"2xBRZ", "2xBRZ"},
               {"4xBRZ", "4xBRZ"}, {"6xBRZ", "6xBRZ"}},
              true, "Upscales the game's textures"),
          opt("Bilinear Filtering", "mupen64plus-BilinearMode", {{"3point", "N64 3-point (accurate)"}, {"standard", "Standard"}},
              true),
          opt("Dithering", "mupen64plus-DitheringPattern", kFalseTrue, true)}},
        {"Console",
         {opt("Expansion Pak", "mupen64plus-ForceDisableExtraMem", {{"False", "On (8 MB)"}, {"True", "Off (4 MB)"}}, true,
              "Needed by Majora's Mask, Donkey Kong 64, Perfect Dark"),
          opt("Player 1 Accessory", "mupen64plus-pak1", pak, true),
          opt("Player 2 Accessory", "mupen64plus-pak2", pak, true)}},
        {"Controls",
         {opt("Stick Sensitivity", "mupen64plus-astick-sensitivity", percent({50, 75, 90, 100, 110, 125, 150}, 100)),
          opt("Stick Deadzone", "mupen64plus-astick-deadzone", percent({0, 5, 10, 15, 20, 25, 30}))}},
    };
}

std::vector<SettingGroup> dolphin(bool wii) {
    std::vector<SettingGroup> g = {
        {"Picture",
         {opt("Resolution", "dolphin_efb_scale",
              {{"1", "Native (480p)"}, {"2", "2x (720p)"}, {"3", "3x (1080p)"}, {"4", "4x (1440p)"}, {"5", "5x"},
               {"6", "6x (4K)"}, {"8", "8x"}},
              true, "Renders 3D sharper than the real console"),
          opt("Widescreen Hack", "dolphin_widescreen_hack", kOnOff, true, "Shows more of the 3D world in 16:9; some games glitch"),
          opt("Aspect Ratio", "dolphin_aspect_ratio",
              {{"0", "Auto"}, {"1", "Force 16:9"}, {"2", "Force 4:3"}, {"3", "Stretch"}}, true),
          opt("Anti-Aliasing", "dolphin_anti_aliasing",
              {{"0", "Off"}, {"1", "2x MSAA"}, {"2", "4x MSAA"}, {"3", "8x MSAA"}, {"4", "2x SSAA"}, {"5", "4x SSAA"},
               {"6", "8x SSAA"}},
              true, "Smooths jagged edges (SSAA looks best but is slowest)"),
          opt("Anisotropic Filtering", "dolphin_max_anisotropy", {{"0", "Off"}, {"1", "2x"}, {"2", "4x"}, {"3", "8x"}, {"4", "16x"}},
              true, "Sharper textures at an angle (floors, roads)"),
          opt("Texture Filtering", "dolphin_force_texture_filtering_mode", {{"0", "Game decides"}, {"1", "Sharp"}, {"2", "Smooth"}},
              true),
          opt("Disable Fog", "dolphin_disable_fog", kOnOff, true),
          opt("Shader Compilation", "dolphin_shader_compilation_mode",
              {{"0", "Wait (may stutter)"}, {"2", "Smooth (ubershaders)"}, {"3", "Fastest (may flicker)"}}, true,
              "\"Smooth\" avoids hitches the first time effects appear")}},
        {"Console",
         {opt("CPU Speed", "dolphin_cpu_clock_rate",
              {{"0.50", "50%"}, {"0.80", "80%"}, {"1.00", "100% (normal)"}, {"1.50", "150%"}, {"2.00", "200%"}, {"3.00", "300%"}},
              true, "Higher can fix slowdown; too high can break games"),
          opt("Faster Disc Loading", "dolphin_fast_disc_speed", kOnOff, true),
          raw("System Language", "dolphin_language", true),
          opt("Rumble", "dolphin_enable_rumble", kOnOff, true)}},
    };
    if (wii)
        g.push_back({"Wii",
                     {opt("Wii Controller", "dolphin_wiimote_default_device",
                          {{"nunchuk", "Wiimote + Nunchuk"}, {"sideways", "Wiimote (sideways)"}, {"upright", "Wiimote"},
                           {"classic", "Classic Controller"}},
                          true, "Button Config follows this after the game restarts"),
                      opt("Wiimote Pointer", "dolphin_ir_mode", {{"1", "Right stick"}, {"0", "Right stick (relative)"}, {"2", "Mouse"}},
                          true),
                      opt("Widescreen (Wii setting)", "dolphin_widescreen", kOnOff, true),
                      opt("Sensor Bar", "dolphin_sensor_bar_position", {{"0", "Below the TV"}, {"1", "Above the TV"}}, true)}});
    return g;
}

std::vector<SettingGroup> wiiU() {
    return {
        {"Picture",
         {opt("Resolution", "cemu_internal_resolution",
              {{"0.5x", "0.5x (faster)"}, {"native", "Native (720p)"}, {"1.5x", "1.5x (1080p)"},
               {"2x", "2x (1440p)"}, {"3x", "3x (4K)"}},
              true, "Some games draw parts at a fixed size and can look odd when resized"),
          opt("Smoother Frame Rate", "cemu_render_ahead", kOnOff, false,
              "Draws the next frame early; smoother in heavy games, one frame more input lag"),
          opt("Accurate Shader Math", "cemu_accurate_shader_mul",
              {{"auto", "Game decides"}, {"enabled", "On"}, {"disabled", "Off (faster, may glitch)"}}),
          opt("Wait for GPU (GX2DrawDone)", "cemu_gx2drawdone_sync", kOnOff, false,
              "Some games need it to show correctly; off can be faster"),
          opt("Shader Cache", "cemu_precompiled_shaders",
              {{"auto", "Game decides"}, {"enabled", "On"}, {"disabled", "Off"}}, false,
              "Keeps compiled shaders so games stutter less the next time")}},
        {"Console",
         {opt("CPU Mode", "cemu_cpu_mode",
              {{"auto", "Automatic"}, {"multicore_recompiler", "Multi-core (fastest)"},
               {"singlecore_recompiler", "Single-core"}, {"singlecore_interpreter", "Interpreter (slow, for testing)"}},
              true, "Multi-core is fastest; single-core fixes a few games"),
          raw("System Language", "cemu_console_language", true),
          opt("Audio Thread", "cemu_audio_thread", kOnOff, true,
              "Keeps sound smooth when the game briefly slows down (loading, new effects)"),
          opt("Rumble", "cemu_rumble_strength",
              {{"0", "Off"}, {"50", "50%"}, {"75", "75%"}, {"100", "100%"}}),
          opt("Stick Dead Zone", "cemu_stick_deadzone_p1",
              {{"0", "Off"}, {"10", "10%"}, {"15", "15%"}, {"20", "20%"}, {"25", "25%"}, {"30", "30%"}}, false,
              "Raise this if a character drifts without you touching the stick")}},
    };
}

}  // namespace

const std::vector<SettingGroup>& consoleSettings(System s) {
    static const std::vector<SettingGroup> none;
    static const auto nesS = nes(false), fdsS = nes(true), snesS = snes(), gbS = gameBoy(), gbaS = gba(), dsS = ds(),
                      ctrS = n3ds(), n64S = n64(), gcS = dolphin(false), wiiS = dolphin(true), wiiuS = wiiU();
    switch (s) {
    case System::NES: return nesS;
    case System::FDS: return fdsS;
    case System::SNES: return snesS;
    case System::GB:
    case System::GBC: return gbS;
    case System::GBA: return gbaS;
    case System::NDS: return dsS;
    case System::N3DS: return ctrS;
    case System::N64: return n64S;
    case System::GameCube: return gcS;
    case System::Wii: return wiiS;
    case System::WiiU: return wiiuS;
    default: return none;
    }
}

std::string prettyValue(const std::string& v) {
    if (v == "enabled" || v == "ON" || v == "True") return "On";
    if (v == "disabled" || v == "OFF" || v == "False") return "Off";
    std::string out = v;
    if (!out.empty()) out[0] = char(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}
