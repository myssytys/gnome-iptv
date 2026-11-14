#pragma once

#include <ctime>
#include <string>
#include <vector>

namespace iptv {

struct Channel {
    std::string id;          // tvg-id or fallback normalized name
    std::string name;        // Channel display name
    std::string group;       // group-title
    std::string logo_url;    // tvg-logo
    std::string stream_url;  // HTTPS or HTTP stream URL
    int number = 0;          // tvg-chno or index
};

struct EpgProgramme {
    std::string channel_id;  // XMLTV channel id
    std::string title;
    std::string subtitle;
    std::string description;
    std::string category;
    std::time_t start_time = 0;
    std::time_t stop_time = 0;

    bool is_currently_airing(std::time_t now = 0) const;
    double progress_fraction(std::time_t now = 0) const;
    std::string formatted_time_range() const;
};

struct ScheduledRecording {
    int id = 0;
    std::string channel_id;
    std::string channel_name;
    std::string stream_url;
    std::string programme_title;
    std::time_t start_time = 0;
    std::time_t stop_time = 0;
    bool active = false;
    bool completed = false;
};

struct AppConfig {
    std::string m3u_url;
    std::string epg_url;
    std::string recording_dir;
    std::string recording_format = "ts"; // "ts" or "mkv"
    int epg_update_interval_minutes = 15;
    int reconnect_delay_seconds = 2;
    int watchdog_timeout_seconds = 10;
    bool auto_reconnect = true;
    bool use_ffmpeg_player = true; // true = ffmpeg streaming pipe, false = GtkVideo native
    bool mute_audio = false;
    double volume = 1.0;
};

std::string format_timestamp_hhmm(std::time_t t);
std::string format_timestamp_full(std::time_t t);
std::string format_duration_hms(std::time_t seconds);
std::string sanitize_filename(const std::string& input);

} // namespace iptv

