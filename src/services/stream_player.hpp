#pragma once

#include "models/types.hpp"

#include <gtk/gtk.h>
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace iptv {

enum class PlayerState {
    Stopped,
    Connecting,
    Playing,
    Reconnecting,
    Error
};

struct PlayerStatus {
    PlayerState state = PlayerState::Stopped;
    Channel channel;
    int reconnect_attempts = 0;
    uint64_t frames_rendered = 0;
    std::string message;
};

class StreamPlayer {
public:
    using StatusCallback = std::function<void(const PlayerStatus&)>;
    using FullscreenToggleCallback = std::function<void()>;

    StreamPlayer();
    ~StreamPlayer();

    void configure(const AppConfig& cfg);
    void set_status_callback(StatusCallback cb);
    void set_fullscreen_toggle_callback(FullscreenToggleCallback cb);

    // Returns the GTK widget containing the video surface
    GtkWidget* widget() const;

    void play_channel(const Channel& channel);
    void stop();
    void set_muted(bool muted);
    bool is_muted() const;

    PlayerStatus current_status() const;

    static std::vector<std::string> build_ffmpeg_play_args(
        const std::string& stream_url,
        int width = 1280,
        int height = 720);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv
