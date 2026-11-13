#pragma once
#include <cstdint>
#include <memory>
#include <string>

// Discord status ("Rich Presence"): while Nintendall runs, the player's Discord profile shows
// "Playing Nintendall" with the game, its console and its box cover. It talks to the Discord app
// on the same computer (its local IPC pipe / socket), on a thread of its own: if Discord isn't
// running, nothing happens, and it connects when Discord starts. Nothing goes over the internet
// from Nintendall itself.
class DiscordStatus {
public:
    struct Activity {
        std::string details;     // first line: the game ("Super Mario World")
        std::string state;       // second line: the console ("Super Nintendo")
        std::string largeImage;  // picture: an https URL (the box cover)
        std::string largeText;   // shown on hovering the picture
        std::string smallImage;  // small round picture in the corner
        std::string smallText;
        int64_t startTime = 0;   // Unix time the game started (Discord shows the time played); 0 = none
        bool operator==(const Activity& o) const {
            return details == o.details && state == o.state && largeImage == o.largeImage && largeText == o.largeText &&
                   smallImage == o.smallImage && smallText == o.smallText && startTime == o.startTime;
        }
    };

    ~DiscordStatus();
    // appId: the Discord application's ID (its name is what Discord shows after "Playing").
    void start(const std::string& appId);
    void stop();  // disconnects (the status goes away)
    bool running() const { return shared_ != nullptr; }
    // The status to show (sent when it changes; cheap to call every frame).
    void set(const Activity& a);

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    Activity last_;
    bool haveLast_ = false;
};
