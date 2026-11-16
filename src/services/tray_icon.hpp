#pragma once

#include <functional>
#include <string>

namespace iptv {

class TrayIcon {
public:
    struct Callbacks {
        std::function<void()> on_toggle_window;
        std::function<void()> on_show_window;
        std::function<void()> on_toggle_mute;
        std::function<void()> on_stop_playback;
        std::function<void()> on_quit;
        std::function<void(int)> on_scroll_volume;
    };

    TrayIcon();
    ~TrayIcon();

    void start(Callbacks cbs);
    void update_state(bool window_visible, bool muted, const std::string& now_playing = "");

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv
