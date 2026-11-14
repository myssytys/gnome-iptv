#pragma once

#include "models/types.hpp"

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace iptv {

struct RecordingStats {
    bool is_recording = false;
    bool reconnecting = false;
    int reconnect_attempts = 0;
    std::string channel_name;
    std::string stream_url;
    std::string output_path;
    std::time_t started_at = 0;
    std::time_t elapsed_seconds = 0;
    uint64_t bytes_written = 0;
    std::string status_text;
};

class StreamRecorder {
public:
    using StateCallback = std::function<void(const RecordingStats&)>;

    StreamRecorder();
    ~StreamRecorder();

    void configure(const AppConfig& cfg);
    void set_state_callback(StateCallback cb);

    bool start_recording(const Channel& channel, const std::string& programme_title = "");
    void stop_recording();
    bool is_recording() const;
    RecordingStats current_stats() const;

    // Scheduled recordings management
    int schedule_recording(const Channel& channel, const EpgProgramme& prog);
    void cancel_scheduled_recording(int id);
    std::vector<ScheduledRecording> get_scheduled_recordings() const;

    // Helper to build the exact ffmpeg command line for inspection/logging
    static std::vector<std::string> build_ffmpeg_record_args(
        const std::string& stream_url,
        const std::string& output_target,
        const std::string& format = "mpegts");

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv

