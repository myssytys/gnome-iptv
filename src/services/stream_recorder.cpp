#include "services/stream_recorder.hpp"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace iptv {

std::vector<std::string> StreamRecorder::build_ffmpeg_record_args(
    const std::string& stream_url,
    const std::string& output_target,
    const std::string& format) {
    std::vector<std::string> args = {
        "ffmpeg",
        "-nostdin",
        "-hide_banner",
        "-loglevel", "warning",
        "-y"
    };

    if (stream_url.starts_with("http://") || stream_url.starts_with("https://")) {
        args.insert(args.end(), {
            "-user_agent", "VLC/3.0.20 LibVLC/3.0.20",
            "-reconnect", "1",
            "-reconnect_at_eof", "1",
            "-reconnect_streamed", "1",
            "-reconnect_on_network_error", "1",
            "-reconnect_on_http_error", "4xx,5xx",
            "-reconnect_delay_max", "10",
            "-rw_timeout", "10000000"
        });
    } else if (stream_url.starts_with("file://") || stream_url.starts_with("/")) {
        args.insert(args.end(), {"-re"});
    }

    args.insert(args.end(), {
        "-i", stream_url,
        "-map", "0:v?",
        "-map", "0:a?",
        "-c", "copy",
        "-f", format,
        output_target
    });
    return args;
}

struct StreamRecorder::Impl {
    mutable std::mutex mtx;
    AppConfig config;
    StateCallback state_cb;

    std::atomic<bool> active{false};
    std::atomic<bool> user_stopped{false};
    std::atomic<bool> reconnecting{false};
    std::atomic<int> reconnect_attempts{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<std::time_t> started_at{0};
    std::atomic<std::time_t> last_byte_time{0};
    std::atomic<int> session_generation{0};

    Channel current_channel;
    std::string current_output_path;
    std::string status_text;

    GSubprocess* proc = nullptr;
    std::ofstream out_stream;
    guint tick_timer_id = 0;
    guint reconnect_timer_id = 0;

    std::vector<ScheduledRecording> schedules;
    int next_schedule_id = 1;
    int active_schedule_id = 0;

    Impl() {
        tick_timer_id = g_timeout_add_seconds(
            1,
            [](gpointer data) -> gboolean {
                static_cast<Impl*>(data)->on_tick();
                return G_SOURCE_CONTINUE;
            },
            this);
    }

    ~Impl() {
        if (tick_timer_id > 0) {
            g_source_remove(tick_timer_id);
            tick_timer_id = 0;
        }
        if (reconnect_timer_id > 0) {
            g_source_remove(reconnect_timer_id);
            reconnect_timer_id = 0;
        }
        stop_internal();
    }

    RecordingStats snapshot_locked() const {
        RecordingStats st;
        st.is_recording = active.load();
        st.reconnecting = reconnecting.load();
        st.reconnect_attempts = reconnect_attempts.load();
        st.channel_name = current_channel.name;
        st.stream_url = current_channel.stream_url;
        st.output_path = current_output_path;
        st.started_at = started_at.load();
        st.elapsed_seconds = st.is_recording && st.started_at > 0
                                 ? (std::time(nullptr) - st.started_at)
                                 : 0;
        st.bytes_written = bytes_written.load();
        st.status_text = status_text;
        return st;
    }

    void emit_state() {
        RecordingStats st;
        StateCallback cb;
        {
            std::lock_guard<std::mutex> lock(mtx);
            st = snapshot_locked();
            cb = state_cb;
        }
        if (cb) {
            auto* heap_pair = new std::pair<StateCallback, RecordingStats>(std::move(cb), std::move(st));
            g_idle_add(
                [](gpointer data) -> gboolean {
                    auto* p = static_cast<std::pair<StateCallback, RecordingStats>*>(data);
                    p->first(p->second);
                    delete p;
                    return G_SOURCE_REMOVE;
                },
                heap_pair);
        }
    }

    void spawn_ffmpeg_pipeline(int gen) {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!active.load() || user_stopped.load() || gen != session_generation.load()) {
                return;
            }
            url = current_channel.stream_url;
        }

        std::vector<std::string> args = build_ffmpeg_record_args(url, "pipe:1", "mpegts");
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv.push_back(a.c_str());
        }
        argv.push_back(nullptr);

        GError* err = nullptr;
        GSubprocess* sub = g_subprocess_newv(
            argv.data(),
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
            &err);

        if (!sub) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                status_text = err ? err->message : "Failed to launch ffmpeg recorder";
            }
            if (err) g_error_free(err);
            schedule_reconnect(gen);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (proc) {
                g_subprocess_force_exit(proc);
                g_object_unref(proc);
            }
            proc = static_cast<GSubprocess*>(g_object_ref(sub));
            last_byte_time.store(std::time(nullptr));
        }

        GInputStream* stdout_stream = g_subprocess_get_stdout_pipe(sub);
        g_object_ref(stdout_stream);

        // Background thread reads MPEG-TS packets from ffmpeg stdout and appends to output file
        std::thread([this, sub, stdout_stream, gen]() {
            char buffer[65536];
            while (active.load() && !user_stopped.load() && gen == session_generation.load()) {
                GError* read_err = nullptr;
                gssize n = g_input_stream_read(stdout_stream, buffer, sizeof(buffer), nullptr, &read_err);
                if (read_err) {
                    g_error_free(read_err);
                    break;
                }
                if (n <= 0) {
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    if (out_stream.is_open()) {
                        out_stream.write(buffer, n);
                        out_stream.flush();
                    }
                    if (reconnecting.load()) {
                        reconnecting.store(false);
                        status_text = "Recording " + current_channel.name;
                    }
                }
                bytes_written.fetch_add(static_cast<uint64_t>(n));
                last_byte_time.store(std::time(nullptr));
            }

            g_object_unref(stdout_stream);
            g_object_unref(sub);

            if (active.load() && !user_stopped.load() && gen == session_generation.load()) {
                g_idle_add(
                    [](gpointer data) -> gboolean {
                        auto* pair = static_cast<std::pair<Impl*, int>*>(data);
                        pair->first->schedule_reconnect(pair->second);
                        delete pair;
                        return G_SOURCE_REMOVE;
                    },
                    new std::pair<Impl*, int>(this, gen));
            }
        }).detach();
    }

    void schedule_reconnect(int gen) {
        if (!active.load() || user_stopped.load() || gen != session_generation.load()) {
            return;
        }
        bool allow_reconnect = true;
        int delay = 2;
        {
            std::lock_guard<std::mutex> lock(mtx);
            allow_reconnect = config.auto_reconnect;
            delay = std::max(1, config.reconnect_delay_seconds);
            if (proc) {
                g_subprocess_force_exit(proc);
                g_object_unref(proc);
                proc = nullptr;
            }
        }

        if (!allow_reconnect) {
            stop_internal();
            return;
        }

        int attempt = reconnect_attempts.fetch_add(1) + 1;
        reconnecting.store(true);
        {
            std::lock_guard<std::mutex> lock(mtx);
            status_text = "Stream interrupted — reconnecting recorder to " +
                          current_channel.name + " (attempt #" + std::to_string(attempt) + ")...";
        }
        emit_state();

        if (reconnect_timer_id > 0) {
            g_source_remove(reconnect_timer_id);
            reconnect_timer_id = 0;
        }
        reconnect_timer_id = g_timeout_add_seconds(
            static_cast<guint>(delay),
            [](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                self->reconnect_timer_id = 0;
                if (self->active.load() && !self->user_stopped.load()) {
                    self->spawn_ffmpeg_pipeline(self->session_generation.load());
                }
                return G_SOURCE_REMOVE;
            },
            this);
    }

    void stop_internal() {
        user_stopped.store(true);
        active.store(false);
        reconnecting.store(false);
        session_generation.fetch_add(1);

        if (reconnect_timer_id > 0) {
            g_source_remove(reconnect_timer_id);
            reconnect_timer_id = 0;
        }

        std::string finished_path;
        std::string target_format;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (proc) {
                g_subprocess_force_exit(proc);
                g_object_unref(proc);
                proc = nullptr;
            }
            if (out_stream.is_open()) {
                out_stream.flush();
                out_stream.close();
            }
            finished_path = current_output_path;
            target_format = config.recording_format;
            status_text = "Recording saved to " + finished_path;
        }

        // If user selected MKV format, remux the captured .ts stream into .mkv
        if (target_format == "mkv" && finished_path.ends_with(".mkv")) {
            std::string tmp_ts = finished_path + ".part.ts";
            if (g_file_test(finished_path.c_str(), G_FILE_TEST_EXISTS)) {
                g_rename(finished_path.c_str(), tmp_ts.c_str());
                const char* remux_argv[] = {
                    "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                    "-i", tmp_ts.c_str(), "-c", "copy", finished_path.c_str(), nullptr
                };
                GSubprocess* remux = g_subprocess_newv(remux_argv, G_SUBPROCESS_FLAGS_NONE, nullptr);
                if (remux) {
                    g_subprocess_wait(remux, nullptr, nullptr);
                    g_object_unref(remux);
                    g_unlink(tmp_ts.c_str());
                } else {
                    g_rename(tmp_ts.c_str(), finished_path.c_str());
                }
            }
        }

        emit_state();
    }

    void on_tick() {
        std::time_t now = std::time(nullptr);

        // Check watchdog for active recording
        if (active.load() && !user_stopped.load() && !reconnecting.load()) {
            std::time_t last_b = last_byte_time.load();
            int timeout_sec = 10;
            {
                std::lock_guard<std::mutex> lock(mtx);
                timeout_sec = std::max(4, config.watchdog_timeout_seconds);
            }
            if (last_b > 0 && (now - last_b) >= timeout_sec) {
                // Stream stalled! Force ffmpeg exit so reader thread triggers reconnect
                std::lock_guard<std::mutex> lock(mtx);
                if (proc) {
                    g_subprocess_force_exit(proc);
                }
            }
            emit_state();
        }

        // Check EPG scheduled recordings
        std::vector<ScheduledRecording> to_start;
        bool should_stop_scheduled = false;
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (auto& s : schedules) {
                if (!s.completed && !s.active && now >= s.start_time && now < s.stop_time) {
                    if (!active.load()) {
                        s.active = true;
                        active_schedule_id = s.id;
                        to_start.push_back(s);
                    }
                } else if (s.active && now >= s.stop_time) {
                    s.active = false;
                    s.completed = true;
                    if (active_schedule_id == s.id) {
                        active_schedule_id = 0;
                        should_stop_scheduled = true;
                    }
                }
            }
        }

        if (should_stop_scheduled) {
            stop_internal();
        }
        for (const auto& s : to_start) {
            Channel ch;
            ch.id = s.channel_id;
            ch.name = s.channel_name;
            ch.stream_url = s.stream_url;
            start_recording_impl(ch, s.programme_title);
        }
    }

    bool start_recording_impl(const Channel& channel, const std::string& programme_title) {
        if (channel.stream_url.empty()) return false;
        if (active.load()) {
            stop_internal();
        }

        std::string rec_dir;
        std::string ext;
        {
            std::lock_guard<std::mutex> lock(mtx);
            rec_dir = config.recording_dir;
            ext = (config.recording_format == "mkv") ? ".mkv" : ".ts";
        }
        if (rec_dir.empty()) rec_dir = "/tmp";
        g_mkdir_with_parents(rec_dir.c_str(), 0755);

        std::time_t now = std::time(nullptr);
        std::tm local_tm{};
        localtime_r(&now, &local_tm);
        std::ostringstream ts_oss;
        ts_oss << std::put_time(&local_tm, "%Y%m%d_%H%M%S");

        std::string base_name = sanitize_filename(channel.name);
        if (!programme_title.empty()) {
            base_name += "_" + sanitize_filename(programme_title);
        }
        std::string full_path = rec_dir + "/" + base_name + "_" + ts_oss.str() + ext;

        int gen = session_generation.fetch_add(1) + 1;
        {
            std::lock_guard<std::mutex> lock(mtx);
            current_channel = channel;
            current_output_path = full_path;
            out_stream.open(full_path, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!out_stream.is_open()) {
                status_text = "Failed to open recording file: " + full_path;
                return false;
            }
            status_text = "Recording " + channel.name;
        }

        user_stopped.store(false);
        reconnecting.store(false);
        reconnect_attempts.store(0);
        bytes_written.store(0);
        started_at.store(now);
        last_byte_time.store(now);
        active.store(true);

        spawn_ffmpeg_pipeline(gen);
        emit_state();
        return true;
    }
};

StreamRecorder::StreamRecorder() : impl_(new Impl()) {}

StreamRecorder::~StreamRecorder() {
    delete impl_;
}

void StreamRecorder::configure(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->config = cfg;
}

void StreamRecorder::set_state_callback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->state_cb = std::move(cb);
}

bool StreamRecorder::start_recording(const Channel& channel, const std::string& programme_title) {
    return impl_->start_recording_impl(channel, programme_title);
}

void StreamRecorder::stop_recording() {
    impl_->stop_internal();
}

bool StreamRecorder::is_recording() const {
    return impl_->active.load();
}

RecordingStats StreamRecorder::current_stats() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->snapshot_locked();
}

int StreamRecorder::schedule_recording(const Channel& channel, const EpgProgramme& prog) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    ScheduledRecording sr;
    sr.id = impl_->next_schedule_id++;
    sr.channel_id = channel.id;
    sr.channel_name = channel.name;
    sr.stream_url = channel.stream_url;
    sr.programme_title = prog.title;
    sr.start_time = prog.start_time;
    sr.stop_time = prog.stop_time;
    impl_->schedules.push_back(sr);
    return sr.id;
}

void StreamRecorder::cancel_scheduled_recording(int id) {
    bool stop_now = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        for (auto it = impl_->schedules.begin(); it != impl_->schedules.end(); ++it) {
            if (it->id == id) {
                if (it->active && impl_->active_schedule_id == id) {
                    stop_now = true;
                    impl_->active_schedule_id = 0;
                }
                impl_->schedules.erase(it);
                break;
            }
        }
    }
    if (stop_now) {
        stop_recording();
    }
}

std::vector<ScheduledRecording> StreamRecorder::get_scheduled_recordings() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->schedules;
}

} // namespace iptv
