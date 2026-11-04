#include "core.hpp"

#include <SDL.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "config.hpp"

namespace fs = std::filesystem;

// libretro callbacks carry no user pointer, so they reach the active core through this.
static LibretroCore* g_core = nullptr;

static bool g_verboseCoreLog = SDL_getenv("NINTENDALL_VERBOSE") != nullptr;

static void RETRO_CALLCONV cbLog(enum retro_log_level level, const char* fmt, ...) {
    if (level == RETRO_LOG_DEBUG && !g_verboseCoreLog) return;
    static const char* names[] = {"debug", "info", "warn", "error"};
    std::fprintf(stderr, "[core %s] ", names[level <= RETRO_LOG_ERROR ? level : RETRO_LOG_ERROR]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static bool RETRO_CALLCONV cbEnvironment(unsigned cmd, void* data) { return g_core && g_core->environment(cmd, data); }
static void RETRO_CALLCONV cbVideo(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (g_core) g_core->videoRefresh(data, w, h, pitch);
}
static void RETRO_CALLCONV cbAudioSample(int16_t l, int16_t r) {
    if (g_core) g_core->audioSample(l, r);
}
static size_t RETRO_CALLCONV cbAudioBatch(const int16_t* data, size_t frames) {
    return g_core ? g_core->audioBatch(data, frames) : frames;
}
static void RETRO_CALLCONV cbInputPoll() {}  // SDL is polled once per frame by the app
static int16_t RETRO_CALLCONV cbInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    return g_core ? g_core->inputState(port, device, index, id) : 0;
}
static bool RETRO_CALLCONV cbRumble(unsigned port, enum retro_rumble_effect effect, uint16_t strength) {
    return g_core && g_core->host().onRumble(port, effect, strength);
}

LibretroCore::LibretroCore(CoreHost& host, fs::path systemDir, fs::path saveDir, fs::path optionsFile)
    : host_(host), systemDir_(systemDir.u8string()), saveDir_(saveDir.u8string()), optionsFile_(std::move(optionsFile)) {}

LibretroCore::~LibretroCore() { unload(); }

template <typename T>
static bool sym(void* lib, const char* name, T& out) {
    out = reinterpret_cast<T>(SDL_LoadFunction(lib, name));
    return out != nullptr;
}

bool LibretroCore::load(const fs::path& corePath, std::string& error) {
    if (g_core && g_core != this) {
        error = "Another core is already running";
        return false;
    }
    corePath_ = corePath.u8string();
    lib_ = SDL_LoadObject(corePath_.c_str());
    if (!lib_) {
        error = std::string("Couldn't load core: ") + SDL_GetError();
        return false;
    }
    bool ok = sym(lib_, "retro_init", api_.init) && sym(lib_, "retro_deinit", api_.deinit) &&
              sym(lib_, "retro_api_version", api_.api_version) &&
              sym(lib_, "retro_get_system_info", api_.get_system_info) &&
              sym(lib_, "retro_get_system_av_info", api_.get_system_av_info) &&
              sym(lib_, "retro_set_environment", api_.set_environment) &&
              sym(lib_, "retro_set_video_refresh", api_.set_video_refresh) &&
              sym(lib_, "retro_set_audio_sample", api_.set_audio_sample) &&
              sym(lib_, "retro_set_audio_sample_batch", api_.set_audio_sample_batch) &&
              sym(lib_, "retro_set_input_poll", api_.set_input_poll) &&
              sym(lib_, "retro_set_input_state", api_.set_input_state) &&
              sym(lib_, "retro_set_controller_port_device", api_.set_controller_port_device) &&
              sym(lib_, "retro_reset", api_.reset) && sym(lib_, "retro_run", api_.run) &&
              sym(lib_, "retro_serialize_size", api_.serialize_size) &&
              sym(lib_, "retro_serialize", api_.serialize) && sym(lib_, "retro_unserialize", api_.unserialize) &&
              sym(lib_, "retro_load_game", api_.load_game) && sym(lib_, "retro_unload_game", api_.unload_game) &&
              sym(lib_, "retro_get_memory_data", api_.get_memory_data) &&
              sym(lib_, "retro_get_memory_size", api_.get_memory_size);
    if (!ok) {
        error = "File is not a valid libretro core: " + corePath_;
        unload();
        return false;
    }
    if (api_.api_version() != RETRO_API_VERSION) {
        error = "Core uses an incompatible libretro API version";
        unload();
        return false;
    }

    loadOptionOverrides();
    g_core = this;
    // Same order as RetroArch: environment, init, then the other callbacks.
    // Some cores (e.g. Mesen) crash if callbacks are set before retro_init.
    api_.set_environment(cbEnvironment);
    api_.init();
    initialized_ = true;
    api_.set_video_refresh(cbVideo);
    api_.set_audio_sample(cbAudioSample);
    api_.set_audio_sample_batch(cbAudioBatch);
    api_.set_input_poll(cbInputPoll);
    api_.set_input_state(cbInputState);

    retro_system_info info{};
    api_.get_system_info(&info);
    libraryName_ = info.library_name ? info.library_name : "Unknown core";
    libraryVersion_ = info.library_version ? info.library_version : "";
    needFullpath_ = info.need_fullpath;
    return true;
}

bool LibretroCore::loadGame(const fs::path& gamePath, std::string& error) {
    gamePathStr_ = gamePath.u8string();
    retro_game_info gi{};
    gi.path = gamePathStr_.c_str();
    if (!needFullpath_) {
        std::ifstream f(gamePath, std::ios::binary);
        if (!f) {
            error = "Couldn't read " + gamePathStr_;
            return false;
        }
        gameData_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        gi.data = gameData_.data();
        gi.size = gameData_.size();
    }
    if (!api_.load_game(&gi)) {
        error = libraryName_ + " couldn't start this game";
        if (hwRequested_) error += " (it needs GPU rendering, which isn't supported yet)";
        return false;
    }
    gameLoaded_ = true;
    api_.get_system_av_info(&av_);
    if (av_.timing.fps <= 0) av_.timing.fps = 60.0;
    if (av_.timing.sample_rate <= 0) av_.timing.sample_rate = 44100.0;
    return true;
}

void LibretroCore::unload() {
    if (lib_) {
        if (gameLoaded_) api_.unload_game();
        if (initialized_) api_.deinit();
        if (initialized_) saveOptions();
        SDL_UnloadObject(lib_);
    }
    lib_ = nullptr;
    gameLoaded_ = initialized_ = false;
    gameData_.clear();
    if (g_core == this) g_core = nullptr;
}

void LibretroCore::run() {
    if (frameTimeCb_.callback) frameTimeCb_.callback(frameTimeCb_.reference);
    audioBuf_.clear();
    api_.run();
    if (!audioBuf_.empty()) host_.onAudio(audioBuf_.data(), audioBuf_.size() / 2);
}

void LibretroCore::reset() { api_.reset(); }

std::vector<uint8_t> LibretroCore::saveState() {
    std::vector<uint8_t> buf(api_.serialize_size());
    if (buf.empty() || !api_.serialize(buf.data(), buf.size())) buf.clear();
    return buf;
}

bool LibretroCore::loadState(const std::vector<uint8_t>& data) {
    return !data.empty() && api_.unserialize(data.data(), data.size());
}

uint8_t* LibretroCore::saveRamData() {
    return gameLoaded_ ? static_cast<uint8_t*>(api_.get_memory_data(RETRO_MEMORY_SAVE_RAM)) : nullptr;
}
size_t LibretroCore::saveRamSize() { return gameLoaded_ ? api_.get_memory_size(RETRO_MEMORY_SAVE_RAM) : 0; }

// ---------------------------------------------------------------------------
// Core options

void LibretroCore::defineOption(const char* key, const char* desc, const char* def, std::vector<std::string> values) {
    if (!key) return;
    CoreOption o;
    o.description = desc ? desc : key;
    o.values = std::move(values);
    o.defaultValue = def ? def : (o.values.empty() ? "" : o.values[0]);
    o.value = o.defaultValue;
    auto ov = optionOverrides_.find(key);
    if (ov != optionOverrides_.end()) {
        bool valid = o.values.empty();
        for (const auto& v : o.values) valid = valid || v == ov->second;
        if (valid) o.value = ov->second;
    }
    auto forced = forced_.find(key);
    if (forced != forced_.end()) o.value = forced->second;
    options_[key] = std::move(o);
}

void LibretroCore::setOption(const std::string& key, const std::string& value) {
    auto it = options_.find(key);
    if (it == options_.end() || it->second.value == value) return;
    it->second.value = value;
    optionsDirty_ = true;
}

void LibretroCore::forceOption(const std::string& key, const std::string& value) {
    forced_[key] = value;
    auto it = options_.find(key);
    if (it != options_.end() && it->second.value != value) {
        it->second.value = value;
        optionsDirty_ = true;
    }
}

void LibretroCore::loadOptionOverrides() {
    Config c;
    if (c.load(optionsFile_)) optionOverrides_ = c.all();
}

void LibretroCore::saveOptions() const {
    if (options_.empty() || optionsFile_.empty()) return;
    std::ofstream f(optionsFile_);
    f << "# " << libraryName_ << " options. Edit values here; they load next time the core starts.\n";
    for (const auto& [key, o] : options_) {
        f << "\n# " << o.description;
        if (!o.values.empty()) {
            f << "  [";
            for (size_t i = 0; i < o.values.size(); ++i) f << (i ? "|" : "") << o.values[i];
            f << "]";
        }
        f << "\n" << key << " = \"" << o.value << "\"\n";
    }
}

// ---------------------------------------------------------------------------
// libretro environment: how cores ask the frontend for things.

bool LibretroCore::environment(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data) *static_cast<bool*>(data) = true;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        const auto f = *static_cast<const retro_pixel_format*>(data);
        if (f != RETRO_PIXEL_FORMAT_0RGB1555 && f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565)
            return false;
        pixelFormat_ = f;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
        *static_cast<const char**>(data) = systemDir_.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *static_cast<const char**>(data) = saveDir_.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
        *static_cast<const char**>(data) = corePath_.c_str();
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        static_cast<retro_log_callback*>(data)->log = cbLog;
        return true;

    // --- options ---
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *static_cast<unsigned*>(data) = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        // Legacy format: value = "Description; default|other|other"
        for (auto* v = static_cast<const retro_variable*>(data); v && v->key; ++v) {
            std::string s = v->value ? v->value : "";
            std::string desc = s, list;
            const auto semi = s.find("; ");
            if (semi != std::string::npos) {
                desc = s.substr(0, semi);
                list = s.substr(semi + 2);
            }
            std::vector<std::string> values;
            size_t start = 0;
            while (start <= list.size() && !list.empty()) {
                const auto bar = list.find('|', start);
                values.push_back(list.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
            defineOption(v->key, desc.c_str(), values.empty() ? "" : values[0].c_str(), values);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
        const retro_core_option_definition* defs =
            cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS ? static_cast<const retro_core_option_definition*>(data)
                                                      : static_cast<const retro_core_options_intl*>(data)->us;
        for (auto* d = defs; d && d->key; ++d) {
            std::vector<std::string> values;
            for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i) values.push_back(d->values[i].value);
            defineOption(d->key, d->desc, d->default_value, values);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        const retro_core_options_v2* opts =
            cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2 ? static_cast<const retro_core_options_v2*>(data)
                                                         : static_cast<const retro_core_options_v2_intl*>(data)->us;
        if (!opts) return false;
        for (auto* d = opts->definitions; d && d->key; ++d) {
            std::vector<std::string> values;
            for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && d->values[i].value; ++i) values.push_back(d->values[i].value);
            defineOption(d->key, d->desc, d->default_value, values);
        }
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* v = static_cast<retro_variable*>(data);
        v->value = nullptr;
        if (!v->key) return false;
        auto it = options_.find(v->key);
        if (it == options_.end()) return false;
        v->value = it->second.value.c_str();
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *static_cast<bool*>(data) = optionsDirty_;
        optionsDirty_ = false;
        return true;

    // --- video / timing ---
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
        const auto* g = static_cast<const retro_game_geometry*>(data);
        av_.geometry.base_width = g->base_width;
        av_.geometry.base_height = g->base_height;
        av_.geometry.aspect_ratio = g->aspect_ratio;
        host_.onAvInfoChanged();
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        av_ = *static_cast<const retro_system_av_info*>(data);
        host_.onAvInfoChanged();
        return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER:
    case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
        hwRequested_ = true;
        cbLog(RETRO_LOG_WARN, "Core asked for GPU rendering; not supported yet.\n");
        return false;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
        return false;
    case RETRO_ENVIRONMENT_SET_ROTATION:
        // e.g. the DS "sideways" layouts: the core sends the picture upright and we turn it.
        rotation_ = *static_cast<const unsigned*>(data) % 4;
        host_.onAvInfoChanged();
        return true;
    case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
        frameTimeCb_ = *static_cast<const retro_frame_time_callback*>(data);
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        if (data) *static_cast<int*>(data) = 3;  // video + audio
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        *static_cast<bool*>(data) = host_.isFastForwarding();
        return true;
    case RETRO_ENVIRONMENT_GET_OVERSCAN:
        *static_cast<bool*>(data) = false;
        return true;

    // --- input ---
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
        *static_cast<unsigned*>(data) = 4;
        return true;
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        static_cast<retro_rumble_interface*>(data)->set_rumble_state = cbRumble;
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        return true;

    // --- messages ---
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *static_cast<unsigned*>(data) = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const auto* m = static_cast<const retro_message*>(data);
        if (m->msg) host_.onMessage(m->msg, m->frames * 1000 / 60);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        const auto* m = static_cast<const retro_message_ext*>(data);
        if (!m->msg) return true;
        if (m->target != RETRO_MESSAGE_TARGET_OSD) cbLog(RETRO_LOG_INFO, "%s\n", m->msg);
        if (m->target != RETRO_MESSAGE_TARGET_LOG) host_.onMessage(m->msg, m->duration);
        return true;
    }

    // --- misc ---
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        *static_cast<unsigned*>(data) = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_USERNAME:
        *static_cast<const char**>(data) = "Player";
        return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:
        shutdownRequested_ = true;
        return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        return true;
    default:
        return false;
    }
}

void LibretroCore::videoRefresh(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data || data == RETRO_HW_FRAME_BUFFER_VALID) return;  // duplicate frame: keep showing the last one
    host_.onVideo(data, w, h, pitch, pixelFormat_);
}

void LibretroCore::audioSample(int16_t l, int16_t r) {
    audioBuf_.push_back(l);
    audioBuf_.push_back(r);
}

size_t LibretroCore::audioBatch(const int16_t* data, size_t frames) {
    audioBuf_.insert(audioBuf_.end(), data, data + frames * 2);
    return frames;
}

int16_t LibretroCore::inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    return host_.onInputState(port, device, index, id);
}
