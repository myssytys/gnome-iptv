#include "services/stream_player.hpp"

#include <glib.h>
#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace iptv {

std::vector<std::string> StreamPlayer::build_ffmpeg_play_args(
    const std::string& stream_url,
    int /*width*/,
    int /*height*/) {
    std::vector<std::string> args = {
        "ffmpeg",
        "-nostdin",
        "-hide_banner",
        "-loglevel", "error",
        "-fflags", "+genpts+igndts+discardcorrupt"
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

    // Zero-transcode (-c copy) MPEG-TS stream pipe into GTK4's hardware-accelerated
    // GStreamer/GL/DMABUF MediaStream sink
    args.insert(args.end(), {
        "-probesize", "1500000",
        "-analyzeduration", "1500000",
        "-i", stream_url,
        "-map", "0:v:0?",
        "-map", "0:a:0?",
        "-c", "copy",
        "-mpegts_flags", "+resend_headers+pat_pmt_at_frames",
        "-flush_packets", "1",
        "-f", "mpegts",
        "pipe:1"
    });
    return args;
}

struct StreamPlayer::Impl {
    mutable std::mutex mtx;
    AppConfig config;
    StatusCallback status_cb;
    FullscreenToggleCallback fullscreen_cb;

    GtkWidget* overlay = nullptr;
    GtkWidget* offload = nullptr;
    GtkWidget* picture = nullptr;
    GtkWidget* status_box = nullptr;
    GtkWidget* spinner = nullptr;
    GtkWidget* status_label = nullptr;

    GtkMediaFile* media_file = nullptr;
    GInputStream* pending_stream = nullptr;
    bool media_realized = false;

    std::atomic<PlayerState> state{PlayerState::Stopped};
    std::atomic<bool> user_stopped{true};
    std::atomic<int> reconnect_attempts{0};
    std::atomic<uint64_t> frames_rendered{0};
    std::atomic<std::time_t> last_frame_time{0};
    std::atomic<int> session_gen{0};
    std::atomic<bool> muted{false};
    std::atomic<double> user_volume{1.0};
    std::atomic<double> current_gain{1.0};

    Channel current_channel;
    std::string status_message = "Select a channel or load an M3U playlist to begin.";

    GSubprocess* video_proc = nullptr;
    guint watchdog_timer_id = 0;
    guint reconnect_timer_id = 0;
    guint fade_timer_id = 0;

    Impl() {
        overlay = gtk_overlay_new();
        g_object_ref_sink(overlay);
        gtk_widget_set_hexpand(overlay, TRUE);
        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_widget_add_css_class(overlay, "osd");

        picture = gtk_picture_new();
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_hexpand(picture, TRUE);
        gtk_widget_set_vexpand(picture, TRUE);

        // Enable GTK 4.14+ hardware graphics offload (direct Wayland DMABUF scanout)
        offload = gtk_graphics_offload_new(picture);
        gtk_graphics_offload_set_enabled(GTK_GRAPHICS_OFFLOAD(offload), GTK_GRAPHICS_OFFLOAD_ENABLED);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), offload);

        g_signal_connect(
            picture, "realize",
            G_CALLBACK(+[](GtkWidget* w, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->media_file && !self->media_realized) {
                    if (GtkNative* nat = gtk_widget_get_native(w)) {
                        if (GdkSurface* surf = gtk_native_get_surface(nat)) {
                            gtk_media_stream_realize(GTK_MEDIA_STREAM(self->media_file), surf);
                            self->media_realized = true;
                            if (self->pending_stream) {
                                gtk_media_file_set_input_stream(self->media_file, self->pending_stream);
                                g_object_unref(self->pending_stream);
                                self->pending_stream = nullptr;
                            }
                        }
                    }
                }
            }),
            this);

        g_signal_connect(
            picture, "unrealize",
            G_CALLBACK(+[](GtkWidget* w, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->media_file && self->media_realized) {
                    if (GtkNative* nat = gtk_widget_get_native(w)) {
                        if (GdkSurface* surf = gtk_native_get_surface(nat)) {
                            gtk_media_stream_unrealize(GTK_MEDIA_STREAM(self->media_file), surf);
                        }
                    }
                    self->media_realized = false;
                }
            }),
            this);

        // Double-click on video surface toggles fullscreen
        GtkGesture* dbl_click = gtk_gesture_click_new();
        g_signal_connect(
            dbl_click, "released",
            G_CALLBACK(+[](GtkGestureClick*, int n_press, double, double, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (n_press == 2 && self->fullscreen_cb) {
                    self->fullscreen_cb();
                }
            }),
            this);
        gtk_widget_add_controller(overlay, GTK_EVENT_CONTROLLER(dbl_click));

        status_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(status_box, GTK_ALIGN_CENTER);
        gtk_widget_set_can_target(status_box, FALSE);

        spinner = gtk_spinner_new();
        gtk_widget_set_size_request(spinner, 36, 36);
        gtk_widget_set_visible(spinner, FALSE);

        status_label = gtk_label_new(status_message.c_str());
        gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
        gtk_widget_add_css_class(status_label, "title-4");

        gtk_box_append(GTK_BOX(status_box), spinner);
        gtk_box_append(GTK_BOX(status_box), status_label);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), status_box);

        watchdog_timer_id = g_timeout_add_seconds(
            1,
            [](gpointer data) -> gboolean {
                static_cast<Impl*>(data)->on_watchdog_tick();
                return G_SOURCE_CONTINUE;
            },
            this);
    }

    ~Impl() {
        if (watchdog_timer_id > 0) {
            g_source_remove(watchdog_timer_id);
            watchdog_timer_id = 0;
        }
        if (reconnect_timer_id > 0) {
            g_source_remove(reconnect_timer_id);
            reconnect_timer_id = 0;
        }
        if (fade_timer_id > 0) {
            g_source_remove(fade_timer_id);
            fade_timer_id = 0;
        }
        stop_internal();
        if (overlay) {
            g_object_unref(overlay);
            overlay = nullptr;
        }
    }

    // Keep audio streaming ON (muted = FALSE) at all times so GStreamer's audio clock and
    // pipeline never pause or reconfigure, while controlling output amplitude via stream volume (0.0..1.0).
    void apply_stream_gain(double gain) {
        double clamped = std::clamp(gain, 0.0, 1.0);
        current_gain.store(clamped);
        if (media_file) {
            GtkMediaStream* ms = GTK_MEDIA_STREAM(media_file);
            gtk_media_stream_set_muted(ms, FALSE);
            gtk_media_stream_set_volume(ms, clamped);
        }
    }

    void start_gain_fade(double target_gain) {
        if (fade_timer_id > 0) {
            g_source_remove(fade_timer_id);
            fade_timer_id = 0;
        }
        double target = std::clamp(target_gain, 0.0, 1.0);
        if (!media_file) {
            current_gain.store(target);
            return;
        }
        auto* target_ptr = new std::pair<Impl*, double>(this, target);
        fade_timer_id = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            20,
            [](gpointer data) -> gboolean {
                auto* p = static_cast<std::pair<Impl*, double>*>(data);
                Impl* self = p->first;
                double tgt = p->second;
                double cur = self->current_gain.load();
                double diff = tgt - cur;
                if (std::abs(diff) <= 0.06) {
                    self->apply_stream_gain(tgt);
                    self->fade_timer_id = 0;
                    return G_SOURCE_REMOVE;
                }
                double step = (diff > 0.0) ? std::max(0.05, diff * 0.28) : std::min(-0.05, diff * 0.28);
                self->apply_stream_gain(cur + step);
                return G_SOURCE_CONTINUE;
            },
            target_ptr,
            +[](gpointer data) { delete static_cast<std::pair<Impl*, double>*>(data); });
    }

    PlayerStatus snapshot_locked() const {
        PlayerStatus st;
        st.state = state.load();
        st.channel = current_channel;
        st.reconnect_attempts = reconnect_attempts.load();
        st.frames_rendered = frames_rendered.load();
        st.message = status_message;
        return st;
    }

    void emit_status() {
        PlayerStatus st;
        StatusCallback cb;
        {
            std::lock_guard<std::mutex> lock(mtx);
            st = snapshot_locked();
            cb = status_cb;
        }
        g_idle_add(
            [](gpointer data) -> gboolean {
                auto* pack = static_cast<std::tuple<Impl*, StatusCallback, PlayerStatus>*>(data);
                Impl* self = std::get<0>(*pack);
                const PlayerStatus& s = std::get<2>(*pack);

                if (self->status_label && GTK_IS_LABEL(self->status_label)) {
                    gtk_label_set_text(GTK_LABEL(self->status_label), s.message.c_str());
                }
                if (self->spinner && GTK_IS_SPINNER(self->spinner)) {
                    bool spinning = (s.state == PlayerState::Connecting || s.state == PlayerState::Reconnecting);
                    gtk_widget_set_visible(self->spinner, spinning ? TRUE : FALSE);
                    if (spinning) gtk_spinner_start(GTK_SPINNER(self->spinner));
                    else gtk_spinner_stop(GTK_SPINNER(self->spinner));
                }
                if (self->status_box && GTK_IS_WIDGET(self->status_box)) {
                    gtk_widget_set_visible(self->status_box, (s.state != PlayerState::Playing) ? TRUE : FALSE);
                }

                if (std::get<1>(*pack)) {
                    std::get<1>(*pack)(s);
                }
                delete pack;
                return G_SOURCE_REMOVE;
            },
            new std::tuple<Impl*, StatusCallback, PlayerStatus>(this, std::move(cb), std::move(st)));
    }

    void teardown_media_file() {
        if (pending_stream) {
            g_object_unref(pending_stream);
            pending_stream = nullptr;
        }
        if (media_file) {
            g_signal_handlers_disconnect_by_data(media_file, this);
            gtk_media_stream_pause(GTK_MEDIA_STREAM(media_file));
            if (media_realized && picture && gtk_widget_get_realized(picture)) {
                if (GtkNative* nat = gtk_widget_get_native(picture)) {
                    if (GdkSurface* surf = gtk_native_get_surface(nat)) {
                        gtk_media_stream_unrealize(GTK_MEDIA_STREAM(media_file), surf);
                    }
                }
            }
            media_realized = false;
            if (picture && GTK_IS_PICTURE(picture)) {
                gtk_picture_set_paintable(GTK_PICTURE(picture), nullptr);
            }
            gtk_media_file_clear(media_file);
            g_object_unref(media_file);
            media_file = nullptr;
        }
    }

    void spawn_ffmpeg_stream(int gen) {
        std::string url;
        std::string ch_name;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (user_stopped.load() || gen != session_gen.load()) return;
            url = current_channel.stream_url;
            ch_name = current_channel.name;
        }

        teardown_media_file();

        std::vector<std::string> args = build_ffmpeg_play_args(url);
        std::vector<const char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        GError* err = nullptr;
        GSubprocess* sub = g_subprocess_newv(
            argv.data(),
            static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE),
            &err);

        if (!sub) {
            {
                std::lock_guard<std::mutex> lock(mtx);
                status_message = err ? err->message : "Failed to launch ffmpeg stream transport";
            }
            if (err) g_error_free(err);
            schedule_reconnect(gen);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (video_proc) {
                g_subprocess_force_exit(video_proc);
                g_object_unref(video_proc);
            }
            video_proc = static_cast<GSubprocess*>(g_object_ref(sub));
            last_frame_time.store(std::time(nullptr));
        }

        GInputStream* stdout_pipe = g_subprocess_get_stdout_pipe(sub);

        // Create empty GtkMediaFile first, realize on GdkSurface so GtkGstSink gets hardware
        // OpenGL / DMABUF context, and THEN attach the input stream.
        // Audio streaming stays ON (muted = FALSE); gain is faded via stream volume (0.0..1.0).
        media_file = GTK_MEDIA_FILE(gtk_media_file_new());
        double initial_gain = muted.load() ? 0.0 : std::clamp(user_volume.load(), 0.0, 1.0);
        apply_stream_gain(initial_gain);
        gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(media_file));

        g_signal_connect(
            media_file, "notify::prepared",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                GtkMediaStream* ms = GTK_MEDIA_STREAM(obj);
                if (gtk_media_stream_is_prepared(ms) && !self->user_stopped.load()) {
                    double gain = self->muted.load() ? 0.0 : std::clamp(self->user_volume.load(), 0.0, 1.0);
                    self->apply_stream_gain(gain);
                    gtk_media_stream_play(ms);
                    self->last_frame_time.store(std::time(nullptr));
                    self->state.store(PlayerState::Playing);
                    {
                        std::lock_guard<std::mutex> lock(self->mtx);
                        self->status_message = "Playing: " + self->current_channel.name;
                    }
                    self->emit_status();
                }
            }),
            this);

        // Fired by GdkPaintable (GtkGstPaintable) on every hardware-decoded video frame
        g_signal_connect(
            media_file, "invalidate-contents",
            G_CALLBACK(+[](GdkPaintable*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                self->last_frame_time.store(std::time(nullptr));
                self->frames_rendered.fetch_add(1);
                if (self->state.load() != PlayerState::Playing && !self->user_stopped.load()) {
                    self->state.store(PlayerState::Playing);
                    self->emit_status();
                }
            }),
            this);

        g_signal_connect(
            media_file, "notify::error",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                GtkMediaStream* ms = GTK_MEDIA_STREAM(obj);
                if (gtk_media_stream_get_error(ms) && !self->user_stopped.load()) {
                    self->schedule_reconnect(self->session_gen.load());
                }
            }),
            this);

        g_signal_connect(
            media_file, "notify::ended",
            G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                GtkMediaStream* ms = GTK_MEDIA_STREAM(obj);
                if (gtk_media_stream_get_ended(ms) && !self->user_stopped.load()) {
                    self->schedule_reconnect(self->session_gen.load());
                }
            }),
            this);

        if (picture && gtk_widget_get_realized(picture)) {
            if (GtkNative* nat = gtk_widget_get_native(picture)) {
                if (GdkSurface* surf = gtk_native_get_surface(nat)) {
                    gtk_media_stream_realize(GTK_MEDIA_STREAM(media_file), surf);
                    media_realized = true;
                }
            }
            gtk_media_file_set_input_stream(media_file, stdout_pipe);
        } else {
            // Defer set_input_stream until picture realizes so hardware GL context is created properly
            pending_stream = static_cast<GInputStream*>(g_object_ref(stdout_pipe));
        }

        // Also watch the ffmpeg subprocess itself in case the connection drops
        auto* wait_ctx = new std::pair<Impl*, int>(this, gen);
        g_subprocess_wait_async(
            sub,
            nullptr,
            +[](GObject* source, GAsyncResult* res, gpointer user_data) {
                auto* p = static_cast<std::pair<Impl*, int>*>(user_data);
                g_subprocess_wait_finish(G_SUBPROCESS(source), res, nullptr);
                Impl* self = p->first;
                int g = p->second;
                delete p;
                if (!self->user_stopped.load() && g == self->session_gen.load()) {
                    self->schedule_reconnect(g);
                }
            },
            wait_ctx);
        g_object_unref(sub);
    }

    void schedule_reconnect(int gen) {
        if (user_stopped.load() || gen != session_gen.load()) return;

        int next_gen = session_gen.fetch_add(1) + 1;

        bool allow_reconnect = true;
        int delay = 2;
        std::string ch_name;
        {
            std::lock_guard<std::mutex> lock(mtx);
            allow_reconnect = config.auto_reconnect;
            delay = std::max(1, config.reconnect_delay_seconds);
            ch_name = current_channel.name;
            if (video_proc) {
                g_subprocess_force_exit(video_proc);
                g_object_unref(video_proc);
                video_proc = nullptr;
            }
        }
        teardown_media_file();

        if (!allow_reconnect) {
            state.store(PlayerState::Error);
            {
                std::lock_guard<std::mutex> lock(mtx);
                status_message = "Stream disconnected: " + ch_name;
            }
            emit_status();
            return;
        }

        int attempt = reconnect_attempts.fetch_add(1) + 1;
        state.store(PlayerState::Reconnecting);
        {
            std::lock_guard<std::mutex> lock(mtx);
            status_message = "Stream interrupted — reconnecting to " + ch_name +
                             " (attempt #" + std::to_string(attempt) + ")...";
        }
        emit_status();

        if (reconnect_timer_id > 0) {
            g_source_remove(reconnect_timer_id);
            reconnect_timer_id = 0;
        }
        auto* timer_ctx = new std::pair<Impl*, int>(this, next_gen);
        reconnect_timer_id = g_timeout_add_seconds_full(
            G_PRIORITY_DEFAULT,
            static_cast<guint>(delay),
            [](gpointer data) -> gboolean {
                auto* p = static_cast<std::pair<Impl*, int>*>(data);
                Impl* self = p->first;
                int g = p->second;
                self->reconnect_timer_id = 0;
                if (!self->user_stopped.load() && g == self->session_gen.load()) {
                    self->spawn_ffmpeg_stream(g);
                }
                return G_SOURCE_REMOVE;
            },
            timer_ctx,
            +[](gpointer data) { delete static_cast<std::pair<Impl*, int>*>(data); });
    }

    void on_watchdog_tick() {
        PlayerState st = state.load();
        if (user_stopped.load() || (st != PlayerState::Playing && st != PlayerState::Connecting)) {
            return;
        }
        std::time_t last_f = last_frame_time.load();
        std::time_t now = std::time(nullptr);
        int timeout_sec = 10;
        {
            std::lock_guard<std::mutex> lock(mtx);
            timeout_sec = std::max(4, config.watchdog_timeout_seconds);
        }
        if (last_f > 0 && (now - last_f) >= timeout_sec) {
            schedule_reconnect(session_gen.load());
        }
    }

    void stop_internal() {
        user_stopped.store(true);
        state.store(PlayerState::Stopped);
        session_gen.fetch_add(1);

        if (reconnect_timer_id > 0) {
            g_source_remove(reconnect_timer_id);
            reconnect_timer_id = 0;
        }
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (video_proc) {
                g_subprocess_force_exit(video_proc);
                g_object_unref(video_proc);
                video_proc = nullptr;
            }
            status_message = "Playback stopped.";
        }
        teardown_media_file();
        emit_status();
    }
};

StreamPlayer::StreamPlayer() : impl_(new Impl()) {}

StreamPlayer::~StreamPlayer() {
    delete impl_;
}

void StreamPlayer::configure(const AppConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->config = cfg;
    impl_->muted.store(cfg.mute_audio);
    impl_->user_volume.store(std::clamp(cfg.volume, 0.0, 1.0));
    double target_gain = cfg.mute_audio ? 0.0 : std::clamp(cfg.volume, 0.0, 1.0);
    impl_->apply_stream_gain(target_gain);
}

void StreamPlayer::set_status_callback(StatusCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->status_cb = std::move(cb);
}

void StreamPlayer::set_fullscreen_toggle_callback(FullscreenToggleCallback cb) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->fullscreen_cb = std::move(cb);
}

GtkWidget* StreamPlayer::widget() const {
    return impl_->overlay;
}

void StreamPlayer::play_channel(const Channel& channel) {
    if (channel.stream_url.empty()) return;

    impl_->stop_internal();

    int gen = impl_->session_gen.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->current_channel = channel;
        impl_->status_message = "Connecting to " + channel.name + "...";
    }
    impl_->user_stopped.store(false);
    impl_->reconnect_attempts.store(0);
    impl_->frames_rendered.store(0);
    impl_->last_frame_time.store(std::time(nullptr));
    impl_->state.store(PlayerState::Connecting);

    impl_->emit_status();
    impl_->spawn_ffmpeg_stream(gen);
}

void StreamPlayer::stop() {
    impl_->stop_internal();
}

void StreamPlayer::set_muted(bool muted) {
    impl_->muted.store(muted);
    double target_gain = muted ? 0.0 : std::clamp(impl_->user_volume.load(), 0.0, 1.0);
    impl_->start_gain_fade(target_gain);
}

bool StreamPlayer::is_muted() const {
    return impl_->muted.load();
}

void StreamPlayer::set_volume(double volume) {
    double clamped = std::clamp(volume, 0.0, 1.0);
    impl_->user_volume.store(clamped);
    if (impl_->fade_timer_id > 0) {
        g_source_remove(impl_->fade_timer_id);
        impl_->fade_timer_id = 0;
    }
    if (clamped > 0.0) {
        impl_->muted.store(false);
    }
    impl_->apply_stream_gain(impl_->muted.load() ? 0.0 : clamped);
}

double StreamPlayer::volume() const {
    return impl_->user_volume.load();
}

void StreamPlayer::set_fullscreen_active(bool fullscreen) {
    if (impl_->offload) {
        // In Wayland/Mutter, hardware direct scanout (graphics offload) on a fullscreen
        // surface can cause Wayland compositor shell panels (like window-list or top panel)
        // to lose tracking or become occluded. We dynamically disable offload during fullscreen
        // and re-enable it when exiting fullscreen.
        gtk_graphics_offload_set_enabled(
            GTK_GRAPHICS_OFFLOAD(impl_->offload),
            fullscreen ? GTK_GRAPHICS_OFFLOAD_DISABLED : GTK_GRAPHICS_OFFLOAD_ENABLED);
    }
}

PlayerStatus StreamPlayer::current_status() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->snapshot_locked();
}

} // namespace iptv
