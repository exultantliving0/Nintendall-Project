#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "hwrender.hpp"
#include "libretro.h"

// What a running core needs from the app (screen, speakers, controllers...).
class CoreHost {
public:
    virtual ~CoreHost() = default;
    virtual void onVideo(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt) = 0;
    virtual void onAudio(const int16_t* stereo, size_t frames) = 0;
    virtual int16_t onInputState(unsigned port, unsigned device, unsigned index, unsigned id) = 0;
    virtual bool onRumble(unsigned port, retro_rumble_effect effect, uint16_t strength) = 0;
    virtual void onMessage(const std::string& msg, unsigned durationMs) = 0;
    virtual void onAvInfoChanged() = 0;
    virtual bool isFastForwarding() const = 0;
    virtual unsigned audioOutputRate() const { return 48000; }  // what the speakers play at
    virtual float displayRefreshRate() const { return 60.0f; }
};

struct CoreOption {
    std::string description;
    std::string value;
    std::string defaultValue;
    std::vector<std::string> values;
    std::vector<std::string> labels;  // the core's display name for each value (may equal the value)
};

// One loaded libretro core (a shared library implementing an emulator).
// Only one core can be loaded at a time, because libretro callbacks are global.
class LibretroCore {
public:
    LibretroCore(CoreHost& host, std::filesystem::path systemDir, std::filesystem::path saveDir,
                 std::filesystem::path optionsFile);
    ~LibretroCore();
    LibretroCore(const LibretroCore&) = delete;
    LibretroCore& operator=(const LibretroCore&) = delete;

    bool load(const std::filesystem::path& corePath, std::string& error);
    bool loadGame(const std::filesystem::path& gamePath, std::string& error);
    void unload();

    void run();
    void reset();
    // Plug a controller type into a port (RETRO_DEVICE_JOYPAD), or unplug it (RETRO_DEVICE_NONE).
    void setPortDevice(unsigned port, unsigned device);
    std::vector<uint8_t> saveState();
    bool loadState(const std::vector<uint8_t>& data);

    uint8_t* saveRamData();
    size_t saveRamSize();

    const retro_system_av_info& avInfo() const { return av_; }
    const std::string& name() const { return libraryName_; }
    const std::string& version() const { return libraryVersion_; }
    bool wantsHardwareRendering() const { return hwRequested_; }
    bool usingHardwareRendering() const { return hwActive_; }
    uintptr_t hwFramebuffer() const { return hw_.framebuffer(); }
    bool shutdownRequested() const { return shutdownRequested_; }
    // Screen rotation the core asked for, in 90 degree counter-clockwise steps (0-3).
    unsigned rotation() const { return rotation_; }

    // Core options (persisted to the options file on unload).
    std::map<std::string, CoreOption>& options() { return options_; }
    void setOption(const std::string& key, const std::string& value);
    // Set an option from the app (e.g. the DS screen layout). Applies now, or as soon as
    // the core defines the option, and wins over the user's options file.
    void forceOption(const std::string& key, const std::string& value);
    // Dolphin announces 50 <-> 60 Hz changes only as a geometry change: re-read the frame rate then.
    void setRefreshTimingOnGeometry(bool on) { refreshTimingOnGeometry_ = on; }
    // Our own default for an option: used unless the user has picked another value
    // (applies now, or as soon as the core defines the option).
    void preferDefault(const std::string& key, const std::string& value);

    // Called through the static libretro callbacks.
    bool environment(unsigned cmd, void* data);
    void videoRefresh(const void* data, unsigned w, unsigned h, size_t pitch);
    void audioSample(int16_t l, int16_t r);
    size_t audioBatch(const int16_t* data, size_t frames);
    int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id);
    CoreHost& host() { return host_; }

private:
    struct Api {
        void (*init)();
        void (*deinit)();
        unsigned (*api_version)();
        void (*get_system_info)(retro_system_info*);
        void (*get_system_av_info)(retro_system_av_info*);
        void (*set_environment)(retro_environment_t);
        void (*set_video_refresh)(retro_video_refresh_t);
        void (*set_audio_sample)(retro_audio_sample_t);
        void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
        void (*set_input_poll)(retro_input_poll_t);
        void (*set_input_state)(retro_input_state_t);
        void (*set_controller_port_device)(unsigned, unsigned);
        void (*reset)();
        void (*run)();
        size_t (*serialize_size)();
        bool (*serialize)(void*, size_t);
        bool (*unserialize)(const void*, size_t);
        bool (*load_game)(const retro_game_info*);
        void (*unload_game)();
        void* (*get_memory_data)(unsigned);
        size_t (*get_memory_size)(unsigned);
    };

    void defineOption(const char* key, const char* desc, const char* def, std::vector<std::string> values,
                      std::vector<std::string> labels = {});
    void loadOptionOverrides();
    void saveOptions() const;

    CoreHost& host_;
    void* lib_ = nullptr;
    Api api_{};
    bool initialized_ = false;
    bool gameLoaded_ = false;

    std::string systemDir_, saveDir_, corePath_;
    std::filesystem::path optionsFile_;
    std::map<std::string, std::string> optionOverrides_;
    std::map<std::string, std::string> forced_;
    std::map<std::string, std::string> preferred_;
    std::map<std::string, CoreOption> options_;
    bool optionsDirty_ = false;

    std::string libraryName_, libraryVersion_;
    bool needFullpath_ = false;
    std::vector<uint8_t> gameData_;
    std::string gamePathStr_;

    retro_system_av_info av_{};
    retro_pixel_format pixelFormat_ = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_frame_time_callback frameTimeCb_{};
    bool hwRequested_ = false;
    bool hwActive_ = false;
    std::string hwError_;
    HwRender hw_;
    retro_hw_render_callback hwCallback_{};
    void resizeHwFramebuffer();
    bool shutdownRequested_ = false;
    unsigned rotation_ = 0;
    bool refreshTimingOnGeometry_ = false;
    std::vector<int16_t> audioBuf_;
};
