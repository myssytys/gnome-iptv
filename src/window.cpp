#include "window.hpp"
#include "preferences_dialog.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace iptv {

namespace {

std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains_ci_lower_needle(std::string_view haystack, std::string_view lower_needle) {
    if (lower_needle.empty()) return true;
    if (haystack.size() < lower_needle.size()) return false;
    const size_t limit = haystack.size() - lower_needle.size();
    const unsigned char first_c = static_cast<unsigned char>(lower_needle[0]);
    for (size_t i = 0; i <= limit; ++i) {
        if (static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(haystack[i]))) == first_c) {
            bool match = true;
            for (size_t j = 1; j < lower_needle.size(); ++j) {
                if (static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(haystack[i + j]))) !=
                    static_cast<unsigned char>(lower_needle[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
    }
    return false;
}

std::string format_bytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        oss << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GB";
    } else if (bytes >= 1024ULL * 1024ULL) {
        oss << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024ULL) {
        oss << (static_cast<double>(bytes) / 1024.0) << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

void clear_list_box(GtkListBox* box) {
    if (!box) return;
    while (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(box))) {
        gtk_list_box_remove(box, child);
    }
}

} // namespace

struct MainWindow::Impl {
    AdwApplication* app = nullptr;
    AdwApplicationWindow* window = nullptr;
    AdwToastOverlay* toast_overlay = nullptr;
    AdwNavigationSplitView* split_view = nullptr;

    // Services
    ConfigStore config_store;
    AppConfig config;
    M3uParser m3u_parser;
    EpgManager* epg_manager = nullptr;
    StreamPlayer player;
    StreamRecorder recorder;

    // Data state
    std::vector<Channel> all_channels;
    std::vector<Channel> filtered_channels;
    std::vector<std::string> group_names;
    std::unordered_map<std::string, size_t> channel_by_name_idx;
    std::unordered_map<std::string, size_t> channel_by_id_idx;
    Channel selected_channel;
    bool has_selected_channel = false;

    // Sidebar widgets
    GtkEntry* m3u_url_entry = nullptr;
    GtkButton* connect_btn = nullptr;
    GtkEntry* epg_url_entry = nullptr;
    GtkButton* epg_connect_btn = nullptr;
    GtkSpinButton* epg_interval_spin = nullptr;
    GtkLabel* epg_timer_countdown_label = nullptr;
    GtkSearchEntry* channel_search_entry = nullptr;
    GtkDropDown* group_dropdown = nullptr;
    GtkListBox* channel_list_box = nullptr;
    GtkLabel* epg_footer_label = nullptr;

    // Content widgets
    AdwHeaderBar* content_header = nullptr;
    AdwViewStack* view_stack = nullptr;
    GtkWidget* bottom_epg_box = nullptr;
    GtkLabel* active_channel_title = nullptr;
    GtkLabel* player_state_badge = nullptr;
    GtkButton* prev_channel_btn = nullptr;
    GtkButton* next_channel_btn = nullptr;
    GtkButton* play_reconnect_btn = nullptr;
    GtkButton* stop_btn = nullptr;
    GtkToggleButton* mute_btn = nullptr;
    GtkWidget* volume_knob_area = nullptr;
    GtkScale* volume_scale = nullptr;
    GtkLabel* volume_pct_label = nullptr;
    double knob_drag_start_vol = 1.0;
    bool suppress_volume_signal = false;
    GtkButton* record_btn = nullptr;
    GtkButton* fullscreen_btn = nullptr;
    GtkLabel* record_status_label = nullptr;
    bool is_fullscreen = false;

    // Live Now / Next EPG card & Programme Browser
    GtkLabel* now_title_label = nullptr;
    GtkLabel* now_time_label = nullptr;
    GtkProgressBar* now_progress_bar = nullptr;
    GtkLabel* now_desc_label = nullptr;
    GtkLabel* next_title_label = nullptr;
    GtkButton* epg_prev_btn = nullptr;
    GtkButton* epg_now_btn = nullptr;
    GtkButton* epg_next_btn = nullptr;
    std::vector<EpgProgramme> active_channel_programmes;
    size_t epg_browse_index = 0;

    // Full schedule list for selected channel
    GtkListBox* channel_epg_list_box = nullptr;

    // Global EPG search & scheduled recordings tab
    GtkSearchEntry* global_epg_search_entry = nullptr;
    GtkListBox* global_epg_list_box = nullptr;
    GtkListBox* scheduled_rec_list_box = nullptr;

    guint epg_ui_tick_id = 0;
    int ui_tick_counter = 0;
    bool suppress_group_signal = false;
    bool suppress_epg_spin_signal = false;

    Impl(AdwApplication* application, const std::string& cli_m3u, const std::string& cli_epg)
        : app(application) {
        config = config_store.load();
        if (!cli_m3u.empty()) config.m3u_url = cli_m3u;
        if (!cli_epg.empty()) config.epg_url = cli_epg;
        config_store.save(config);

        epg_manager = new EpgManager(config_store.db_path());
        epg_manager->set_active_channels(all_channels);
        player.configure(config);
        recorder.configure(config);

        build_ui();
        wire_callbacks();

        // Start periodic EPG refresh timer
        epg_manager->configure_auto_update(config.epg_url, config.epg_update_interval_minutes);
        update_epg_timer_countdown_ui();

        // 1-second timer for live EPG countdown display + 15-second EPG progress bar update
        epg_ui_tick_id = g_timeout_add_seconds(
            1,
            [](gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                self->update_epg_timer_countdown_ui();
                self->ui_tick_counter = (self->ui_tick_counter + 1) % 15;
                if (self->ui_tick_counter == 0) {
                    self->update_active_channel_epg_ui();
                }
                return G_SOURCE_CONTINUE;
            },
            this);

        if (epg_manager->total_programmes() > 0) {
            std::string footer = "EPG: " + std::to_string(epg_manager->total_programmes()) + " programmes (cached)";
            gtk_label_set_text(epg_footer_label, footer.c_str());
        }

        // Auto-connect on startup if M3U URL is configured
        if (!config.m3u_url.empty()) {
            load_m3u_from_url(config.m3u_url);
        }
        if (!config.epg_url.empty()) {
            load_epg_from_url(config.epg_url, false);
        }
    }

    ~Impl() {
        if (epg_ui_tick_id > 0) {
            g_source_remove(epg_ui_tick_id);
            epg_ui_tick_id = 0;
        }
        player.stop();
        recorder.stop_recording();
        delete epg_manager;
    }

    void show_toast(const std::string& text) {
        if (!toast_overlay) return;
        AdwToast* toast = adw_toast_new(text.c_str());
        adw_toast_set_timeout(toast, 4);
        adw_toast_overlay_add_toast(toast_overlay, toast);
    }

    void set_fullscreen(bool full) {
        is_fullscreen = full;
        if (full) {
            gtk_window_fullscreen(GTK_WINDOW(window));
            adw_navigation_split_view_set_collapsed(split_view, TRUE);
            adw_navigation_split_view_set_show_content(split_view, TRUE);
            if (content_header) {
                gtk_widget_set_visible(GTK_WIDGET(content_header), FALSE);
            }
            if (bottom_epg_box) {
                gtk_widget_set_visible(bottom_epg_box, FALSE);
            }
            if (fullscreen_btn) {
                gtk_button_set_icon_name(fullscreen_btn, "view-restore-symbolic");
                gtk_widget_set_tooltip_text(GTK_WIDGET(fullscreen_btn), "Exit Fullscreen (F11 / Esc / Double-Click)");
            }
        } else {
            gtk_window_unfullscreen(GTK_WINDOW(window));
            adw_navigation_split_view_set_collapsed(split_view, FALSE);
            if (content_header) {
                gtk_widget_set_visible(GTK_WIDGET(content_header), TRUE);
            }
            if (bottom_epg_box) {
                gtk_widget_set_visible(bottom_epg_box, TRUE);
            }
            if (fullscreen_btn) {
                gtk_button_set_icon_name(fullscreen_btn, "view-fullscreen-symbolic");
                gtk_widget_set_tooltip_text(GTK_WIDGET(fullscreen_btn), "Toggle Fullscreen (F11 / F / Double-Click)");
            }
        }
    }

    void toggle_fullscreen() {
        set_fullscreen(!is_fullscreen);
    }

    void step_channel(int delta) {
        const auto& list = !filtered_channels.empty() ? filtered_channels : all_channels;
        if (list.empty()) return;

        size_t current_idx = 0;
        bool found = false;
        if (has_selected_channel) {
            for (size_t i = 0; i < list.size(); ++i) {
                if (list[i].stream_url == selected_channel.stream_url &&
                    list[i].name == selected_channel.name) {
                    current_idx = i;
                    found = true;
                    break;
                }
            }
        }

        size_t next_idx = 0;
        if (found) {
            int n = static_cast<int>(list.size());
            int target = (static_cast<int>(current_idx) + delta) % n;
            if (target < 0) target += n;
            next_idx = static_cast<size_t>(target);
        }

        if (channel_list_box) {
            if (GtkListBoxRow* row = gtk_list_box_get_row_at_index(channel_list_box, static_cast<int>(next_idx))) {
                gtk_list_box_select_row(channel_list_box, row);
                gtk_widget_grab_focus(GTK_WIDGET(row));
            }
        }
        select_and_play_channel(list[next_idx]);
    }

    void step_epg_programme(int delta) {
        if (active_channel_programmes.empty()) return;
        int n = static_cast<int>(active_channel_programmes.size());
        int next = std::clamp(static_cast<int>(epg_browse_index) + delta, 0, n - 1);
        epg_browse_index = static_cast<size_t>(next);
        render_browsed_epg_card();
    }

    void reset_epg_browse_to_now() {
        std::time_t now = std::time(nullptr);
        epg_browse_index = 0;
        for (size_t i = 0; i < active_channel_programmes.size(); ++i) {
            if (active_channel_programmes[i].stop_time > now) {
                epg_browse_index = i;
                break;
            }
        }
        render_browsed_epg_card();
    }

    static const char* volume_icon_for_level(double effective_vol) {
        if (effective_vol <= 0.005) return "audio-volume-muted-symbolic";
        if (effective_vol < 0.34) return "audio-volume-low-symbolic";
        if (effective_vol < 0.67) return "audio-volume-medium-symbolic";
        return "audio-volume-high-symbolic";
    }

    double effective_volume() const {
        return config.mute_audio ? 0.0 : std::clamp(config.volume, 0.0, 1.0);
    }

    void sync_volume_controls_ui() {
        suppress_volume_signal = true;
        double eff = effective_volume();
        int pct = static_cast<int>(std::round(eff * 100.0));

        if (mute_btn) {
            gtk_toggle_button_set_active(mute_btn, (eff <= 0.005) ? TRUE : FALSE);
            gtk_button_set_icon_name(GTK_BUTTON(mute_btn), volume_icon_for_level(eff));
        }
        if (volume_scale) {
            gtk_range_set_value(GTK_RANGE(volume_scale), eff * 100.0);
        }
        if (volume_pct_label) {
            std::string txt = std::to_string(pct) + "%";
            gtk_label_set_text(volume_pct_label, txt.c_str());
        }
        if (volume_knob_area) {
            gtk_widget_queue_draw(volume_knob_area);
        }
        suppress_volume_signal = false;
    }

    void set_audio_volume_from_knob(double vol) {
        double clamped = std::clamp(vol, 0.0, 1.0);
        config.volume = clamped;
        config.mute_audio = (clamped <= 0.005);
        config_store.save(config);
        player.set_volume(clamped);
        sync_volume_controls_ui();
    }

    void prompt_open_local_m3u() {
        GtkFileDialog* dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Open Local .m3u / .m3u8 Playlist");

        GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);

        GtkFileFilter* m3u_filter = gtk_file_filter_new();
        gtk_file_filter_set_name(m3u_filter, "M3U Playlists (*.m3u, *.m3u8)");
        gtk_file_filter_add_pattern(m3u_filter, "*.m3u");
        gtk_file_filter_add_pattern(m3u_filter, "*.m3u8");
        gtk_file_filter_add_pattern(m3u_filter, "*.M3U");
        gtk_file_filter_add_pattern(m3u_filter, "*.M3U8");
        gtk_file_filter_add_mime_type(m3u_filter, "audio/x-mpegurl");
        gtk_file_filter_add_mime_type(m3u_filter, "application/vnd.apple.mpegurl");
        g_list_store_append(filters, m3u_filter);
        g_object_unref(m3u_filter);

        GtkFileFilter* all_filter = gtk_file_filter_new();
        gtk_file_filter_set_name(all_filter, "All Files (*)");
        gtk_file_filter_add_pattern(all_filter, "*");
        g_list_store_append(filters, all_filter);
        g_object_unref(all_filter);

        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        g_object_unref(filters);

        gtk_file_dialog_open(
            dialog,
            GTK_WINDOW(window),
            nullptr,
            +[](GObject* source, GAsyncResult* res, gpointer user_data) {
                auto* self = static_cast<Impl*>(user_data);
                GtkFileDialog* dlg = GTK_FILE_DIALOG(source);
                GError* err = nullptr;
                GFile* file = gtk_file_dialog_open_finish(dlg, res, &err);
                if (file) {
                    char* local_path = g_file_get_path(file);
                    if (local_path) {
                        std::string path_str = local_path;
                        g_free(local_path);
                        gtk_editable_set_text(GTK_EDITABLE(self->m3u_url_entry), path_str.c_str());
                        self->config.m3u_url = path_str;
                        self->config_store.save(self->config);
                        self->load_m3u_from_url(path_str);
                    }
                    g_object_unref(file);
                } else if (err) {
                    g_error_free(err);
                }
                g_object_unref(dlg);
            },
            this);
    }

    void build_ui() {
        window = ADW_APPLICATION_WINDOW(adw_application_window_new(GTK_APPLICATION(app)));
        gtk_window_set_title(GTK_WINDOW(window), "GNOME IPTV Player & Recorder");
        gtk_window_set_default_size(GTK_WINDOW(window), 1280, 820);

        toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
        split_view = ADW_NAVIGATION_SPLIT_VIEW(adw_navigation_split_view_new());
        adw_navigation_split_view_set_min_sidebar_width(split_view, 320);
        adw_navigation_split_view_set_max_sidebar_width(split_view, 420);

        // ================= SIDEBAR PAGE =================
        AdwToolbarView* sidebar_toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
        AdwHeaderBar* sidebar_header = ADW_HEADER_BAR(adw_header_bar_new());

        GtkWidget* open_file_btn = gtk_button_new_from_icon_name("document-open-symbolic");
        gtk_widget_set_tooltip_text(open_file_btn, "Open Local .m3u / .m3u8 File...");
        g_signal_connect(
            open_file_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->prompt_open_local_m3u();
            }),
            this);
        adw_header_bar_pack_start(sidebar_header, open_file_btn);

        GtkWidget* refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
        gtk_widget_set_tooltip_text(refresh_btn, "Reload M3U Playlist & Refresh EPG Now");
        g_signal_connect(
            refresh_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                const char* text = gtk_editable_get_text(GTK_EDITABLE(self->m3u_url_entry));
                if (text && *text) {
                    self->config.m3u_url = text;
                    self->config_store.save(self->config);
                    self->load_m3u_from_url(self->config.m3u_url);
                }
                if (self->epg_url_entry) {
                    const char* epg_text = gtk_editable_get_text(GTK_EDITABLE(self->epg_url_entry));
                    if (epg_text) {
                        self->config.epg_url = epg_text;
                        self->config_store.save(self->config);
                    }
                }
                if (!self->config.epg_url.empty()) {
                    self->load_epg_from_url(self->config.epg_url, true);
                }
            }),
            this);
        adw_header_bar_pack_start(sidebar_header, refresh_btn);

        GtkWidget* prefs_btn = gtk_button_new_from_icon_name("emblem-system-symbolic");
        gtk_widget_set_tooltip_text(prefs_btn, "IPTV, EPG & Recording Preferences");
        g_signal_connect(
            prefs_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                PreferencesDialog::present(
                    GTK_WINDOW(self->window),
                    self->config,
                    [self](const AppConfig& updated) {
                        bool m3u_changed = (updated.m3u_url != self->config.m3u_url);
                        bool epg_changed = (updated.epg_url != self->config.epg_url) ||
                                           (updated.epg_update_interval_minutes != self->config.epg_update_interval_minutes);
                        self->config = updated;
                        self->config_store.save(self->config);
                        self->player.configure(self->config);
                        self->recorder.configure(self->config);

                        gtk_editable_set_text(GTK_EDITABLE(self->m3u_url_entry), self->config.m3u_url.c_str());
                        if (self->epg_url_entry) {
                            gtk_editable_set_text(GTK_EDITABLE(self->epg_url_entry), self->config.epg_url.c_str());
                        }
                        if (self->epg_interval_spin) {
                            self->suppress_epg_spin_signal = true;
                            gtk_spin_button_set_value(self->epg_interval_spin, self->config.epg_update_interval_minutes);
                            self->suppress_epg_spin_signal = false;
                        }
                        self->epg_manager->configure_auto_update(
                            self->config.epg_url, self->config.epg_update_interval_minutes);
                        self->update_epg_timer_countdown_ui();

                        if (m3u_changed && !self->config.m3u_url.empty()) {
                            self->load_m3u_from_url(self->config.m3u_url);
                        }
                        if (epg_changed && !self->config.epg_url.empty()) {
                            self->load_epg_from_url(self->config.epg_url, true);
                        }
                    });
            }),
            this);
        adw_header_bar_pack_end(sidebar_header, prefs_btn);
        adw_toolbar_view_add_top_bar(sidebar_toolbar, GTK_WIDGET(sidebar_header));

        GtkWidget* sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_start(sidebar_box, 10);
        gtk_widget_set_margin_end(sidebar_box, 10);
        gtk_widget_set_margin_top(sidebar_box, 8);
        gtk_widget_set_margin_bottom(sidebar_box, 8);

        // Quick-connect HTTPS or local .m3u bar
        GtkWidget* url_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        m3u_url_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(m3u_url_entry, "https://.../playlist.m3u or /path/to/file.m3u");
        gtk_editable_set_text(GTK_EDITABLE(m3u_url_entry), config.m3u_url.c_str());
        gtk_widget_set_tooltip_text(GTK_WIDGET(m3u_url_entry), "M3U / M3U8 Playlist URL or Local Path");
        gtk_widget_set_hexpand(GTK_WIDGET(m3u_url_entry), TRUE);

        GtkWidget* browse_m3u_btn = gtk_button_new_from_icon_name("folder-open-symbolic");
        gtk_widget_set_tooltip_text(browse_m3u_btn, "Browse Local .m3u File...");
        g_signal_connect(
            browse_m3u_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->prompt_open_local_m3u();
            }),
            this);

        connect_btn = GTK_BUTTON(gtk_button_new_with_label("Load M3U"));
        gtk_widget_add_css_class(GTK_WIDGET(connect_btn), "suggested-action");

        auto connect_cb = +[](GtkWidget*, gpointer data) {
            auto* self = static_cast<Impl*>(data);
            const char* text = gtk_editable_get_text(GTK_EDITABLE(self->m3u_url_entry));
            std::string url = text ? text : "";
            if (!url.empty()) {
                self->config.m3u_url = url;
                self->config_store.save(self->config);
                self->load_m3u_from_url(url);
            }
        };
        g_signal_connect(connect_btn, "clicked", G_CALLBACK(connect_cb), this);
        g_signal_connect(m3u_url_entry, "activate", G_CALLBACK(connect_cb), this);

        gtk_box_append(GTK_BOX(url_row), GTK_WIDGET(m3u_url_entry));
        gtk_box_append(GTK_BOX(url_row), browse_m3u_btn);
        gtk_box_append(GTK_BOX(url_row), GTK_WIDGET(connect_btn));
        gtk_box_append(GTK_BOX(sidebar_box), url_row);

        // EPG URL input bar
        GtkWidget* epg_url_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        epg_url_entry = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_placeholder_text(epg_url_entry, "http://.../xmltv.php?... or EPG .xml URL");
        gtk_editable_set_text(GTK_EDITABLE(epg_url_entry), config.epg_url.c_str());
        gtk_widget_set_tooltip_text(GTK_WIDGET(epg_url_entry), "XMLTV EPG URL (.xml / .xml.gz / xmltv.php)");
        gtk_widget_set_hexpand(GTK_WIDGET(epg_url_entry), TRUE);

        epg_connect_btn = GTK_BUTTON(gtk_button_new_with_label("Load EPG"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(epg_connect_btn), "Fetch EPG from URL Now & Start Auto-Update Timer");

        auto epg_connect_cb = +[](GtkWidget*, gpointer data) {
            auto* self = static_cast<Impl*>(data);
            const char* text = gtk_editable_get_text(GTK_EDITABLE(self->epg_url_entry));
            std::string url = text ? text : "";
            self->load_epg_from_url(url, true);
        };
        g_signal_connect(epg_connect_btn, "clicked", G_CALLBACK(epg_connect_cb), this);
        g_signal_connect(epg_url_entry, "activate", G_CALLBACK(epg_connect_cb), this);

        gtk_box_append(GTK_BOX(epg_url_row), GTK_WIDGET(epg_url_entry));
        gtk_box_append(GTK_BOX(epg_url_row), GTK_WIDGET(epg_connect_btn));
        gtk_box_append(GTK_BOX(sidebar_box), epg_url_row);

        // EPG Auto-Update Timer interval & live countdown row
        GtkWidget* epg_timer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget* timer_lbl = gtk_label_new("EPG Timer (min):");
        gtk_widget_add_css_class(timer_lbl, "caption");
        gtk_widget_add_css_class(timer_lbl, "dim-label");

        epg_interval_spin = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(1.0, 360.0, 1.0));
        gtk_spin_button_set_value(epg_interval_spin, config.epg_update_interval_minutes);
        gtk_widget_set_tooltip_text(
            GTK_WIDGET(epg_interval_spin),
            "Automatic EPG update timer interval in minutes (1–360 min)");
        g_signal_connect(
            epg_interval_spin, "value-changed",
            G_CALLBACK(+[](GtkSpinButton* spin, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->suppress_epg_spin_signal) return;
                int mins = gtk_spin_button_get_value_as_int(spin);
                self->config.epg_update_interval_minutes = std::max(1, mins);
                self->config_store.save(self->config);
                self->epg_manager->configure_auto_update(
                    self->config.epg_url, self->config.epg_update_interval_minutes);
                self->update_epg_timer_countdown_ui();
            }),
            this);

        epg_timer_countdown_label = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(epg_timer_countdown_label), "caption");
        gtk_widget_add_css_class(GTK_WIDGET(epg_timer_countdown_label), "dim-label");
        gtk_widget_set_hexpand(GTK_WIDGET(epg_timer_countdown_label), TRUE);
        gtk_widget_set_halign(GTK_WIDGET(epg_timer_countdown_label), GTK_ALIGN_END);
        gtk_label_set_ellipsize(epg_timer_countdown_label, PANGO_ELLIPSIZE_END);

        gtk_box_append(GTK_BOX(epg_timer_row), timer_lbl);
        gtk_box_append(GTK_BOX(epg_timer_row), GTK_WIDGET(epg_interval_spin));
        gtk_box_append(GTK_BOX(epg_timer_row), GTK_WIDGET(epg_timer_countdown_label));
        gtk_box_append(GTK_BOX(sidebar_box), epg_timer_row);

        // Support drag-and-drop of a local .m3u / .m3u8 file onto the window
        GtkDropTarget* drop_target = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(
            drop_target, "drop",
            G_CALLBACK((+[](GtkDropTarget*, const GValue* value, double, double, gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
                    GFile* file = G_FILE(g_value_get_object(value));
                    if (file) {
                        char* local_path = g_file_get_path(file);
                        if (local_path) {
                            std::string path_str = local_path;
                            g_free(local_path);
                            gtk_editable_set_text(GTK_EDITABLE(self->m3u_url_entry), path_str.c_str());
                            self->config.m3u_url = path_str;
                            self->config_store.save(self->config);
                            self->load_m3u_from_url(path_str);
                            return TRUE;
                        }
                    }
                }
                return FALSE;
            })),
            this);
        gtk_widget_add_controller(GTK_WIDGET(window), GTK_EVENT_CONTROLLER(drop_target));

        // Search & Group filter
        channel_search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
        gtk_search_entry_set_placeholder_text(channel_search_entry, "Filter channels...");
        g_signal_connect(
            channel_search_entry, "search-changed",
            G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                static_cast<Impl*>(data)->rebuild_channel_list();
            }),
            this);
        gtk_box_append(GTK_BOX(sidebar_box), GTK_WIDGET(channel_search_entry));

        const char* default_groups[] = {"All Groups", nullptr};
        group_dropdown = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(default_groups));
        g_signal_connect(
            group_dropdown, "notify::selected",
            G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (!self->suppress_group_signal) {
                    self->rebuild_channel_list();
                    self->rebuild_global_epg_search();
                }
            }),
            this);
        gtk_box_append(GTK_BOX(sidebar_box), GTK_WIDGET(group_dropdown));

        // Scrollable channel list
        GtkWidget* ch_scroll = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(ch_scroll, TRUE);
        channel_list_box = GTK_LIST_BOX(gtk_list_box_new());
        gtk_widget_add_css_class(GTK_WIDGET(channel_list_box), "navigation-sidebar");
        gtk_list_box_set_selection_mode(channel_list_box, GTK_SELECTION_SINGLE);
        g_signal_connect(
            channel_list_box, "row-activated",
            G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (!row) return;
                int idx = gtk_list_box_row_get_index(row);
                if (idx >= 0 && static_cast<size_t>(idx) < self->filtered_channels.size()) {
                    self->select_and_play_channel(self->filtered_channels[static_cast<size_t>(idx)]);
                }
            }),
            this);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(ch_scroll), GTK_WIDGET(channel_list_box));
        gtk_box_append(GTK_BOX(sidebar_box), ch_scroll);

        epg_footer_label = GTK_LABEL(gtk_label_new("EPG: Not loaded yet"));
        gtk_widget_add_css_class(GTK_WIDGET(epg_footer_label), "dim-label");
        gtk_widget_add_css_class(GTK_WIDGET(epg_footer_label), "caption");
        gtk_label_set_ellipsize(epg_footer_label, PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(sidebar_box), GTK_WIDGET(epg_footer_label));

        adw_toolbar_view_set_content(sidebar_toolbar, sidebar_box);
        AdwNavigationPage* sidebar_nav = adw_navigation_page_new(GTK_WIDGET(sidebar_toolbar), "Channels");
        adw_navigation_split_view_set_sidebar(split_view, sidebar_nav);

        // ================= CONTENT PAGE =================
        AdwToolbarView* content_toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
        content_header = ADW_HEADER_BAR(adw_header_bar_new());

        // Linked Backward / Forward channel browse buttons in header bar
        GtkWidget* hdr_nav_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(hdr_nav_box, "linked");

        GtkWidget* hdr_prev_btn = gtk_button_new_from_icon_name("go-previous-symbolic");
        gtk_widget_set_tooltip_text(hdr_prev_btn, "Previous Channel (Page Up)");
        g_signal_connect(
            hdr_prev_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->step_channel(-1);
            }),
            this);

        GtkWidget* hdr_next_btn = gtk_button_new_from_icon_name("go-next-symbolic");
        gtk_widget_set_tooltip_text(hdr_next_btn, "Next Channel (Page Down)");
        g_signal_connect(
            hdr_next_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->step_channel(1);
            }),
            this);

        gtk_box_append(GTK_BOX(hdr_nav_box), hdr_prev_btn);
        gtk_box_append(GTK_BOX(hdr_nav_box), hdr_next_btn);
        adw_header_bar_pack_start(content_header, hdr_nav_box);

        view_stack = ADW_VIEW_STACK(adw_view_stack_new());
        AdwViewSwitcher* switcher = ADW_VIEW_SWITCHER(adw_view_switcher_new());
        adw_view_switcher_set_stack(switcher, view_stack);
        adw_view_switcher_set_policy(switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);
        adw_header_bar_set_title_widget(content_header, GTK_WIDGET(switcher));
        adw_toolbar_view_add_top_bar(content_toolbar, GTK_WIDGET(content_header));

        // ---- TAB 1: Live Player & Channel EPG ----
        GtkWidget* player_tab_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
        gtk_paned_set_position(GTK_PANED(player_tab_paned), 430);

        GtkWidget* top_player_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_vexpand(player.widget(), TRUE);
        gtk_box_append(GTK_BOX(top_player_box), player.widget());

        // Transport & Recording Control Bar
        GtkWidget* controls_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(controls_bar, 12);
        gtk_widget_set_margin_end(controls_bar, 12);
        gtk_widget_set_margin_top(controls_bar, 8);
        gtk_widget_set_margin_bottom(controls_bar, 8);

        active_channel_title = GTK_LABEL(gtk_label_new("No Channel Selected"));
        gtk_widget_add_css_class(GTK_WIDGET(active_channel_title), "title-4");
        gtk_label_set_ellipsize(active_channel_title, PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(GTK_WIDGET(active_channel_title), TRUE);
        gtk_widget_set_halign(GTK_WIDGET(active_channel_title), GTK_ALIGN_START);

        player_state_badge = GTK_LABEL(gtk_label_new("Idle"));
        gtk_widget_add_css_class(GTK_WIDGET(player_state_badge), "caption");
        gtk_widget_add_css_class(GTK_WIDGET(player_state_badge), "dim-label");

        prev_channel_btn = GTK_BUTTON(gtk_button_new_from_icon_name("media-skip-backward-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(prev_channel_btn), "Previous Channel (Backward)");
        g_signal_connect(
            prev_channel_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->step_channel(-1);
            }),
            this);

        play_reconnect_btn = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-start-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(play_reconnect_btn), "Play / Reconnect Stream Now");
        g_signal_connect(
            play_reconnect_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->has_selected_channel) {
                    self->player.play_channel(self->selected_channel);
                }
            }),
            this);

        stop_btn = GTK_BUTTON(gtk_button_new_from_icon_name("media-playback-stop-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(stop_btn), "Stop Playback");
        g_signal_connect(
            stop_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->player.stop();
            }),
            this);

        next_channel_btn = GTK_BUTTON(gtk_button_new_from_icon_name("media-skip-forward-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(next_channel_btn), "Next Channel (Forward)");
        g_signal_connect(
            next_channel_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->step_channel(1);
            }),
            this);

        // Audio Volume Knob & Smooth Stream Volume Fade Box
        // Keeps the GStreamer audio stream active at all times while fading the stream gain (0%..100%).
        GtkWidget* audio_vol_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_valign(audio_vol_box, GTK_ALIGN_CENTER);

        mute_btn = GTK_TOGGLE_BUTTON(gtk_toggle_button_new());
        gtk_button_set_icon_name(GTK_BUTTON(mute_btn), volume_icon_for_level(effective_volume()));
        gtk_toggle_button_set_active(mute_btn, (effective_volume() <= 0.005) ? TRUE : FALSE);
        gtk_widget_set_tooltip_text(
            GTK_WIDGET(mute_btn),
            "Fade Audio Stream Volume Out / In (Audio streaming stays ON)");
        g_signal_connect(
            mute_btn, "toggled",
            G_CALLBACK(+[](GtkToggleButton* btn, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->suppress_volume_signal) return;
                bool m = gtk_toggle_button_get_active(btn) != FALSE;
                if (!m && self->config.volume <= 0.01) {
                    self->config.volume = 0.75;
                    self->player.set_volume(self->config.volume);
                }
                self->config.mute_audio = m;
                self->config_store.save(self->config);
                self->player.set_muted(m);
                self->sync_volume_controls_ui();
            }),
            this);

        // Rotary Volume Knob (32x32 Cairo dial: drag up/down/left/right or scroll wheel)
        volume_knob_area = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(volume_knob_area), 32);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(volume_knob_area), 32);
        gtk_widget_set_tooltip_text(
            volume_knob_area,
            "Rotary Volume Knob — Drag or Scroll Wheel to Fade Audio Stream Volume (0%–100%)");
        gtk_drawing_area_set_draw_func(
            GTK_DRAWING_AREA(volume_knob_area),
            +[](GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                double vol = self->effective_volume();
                double cx = width * 0.5;
                double cy = height * 0.5;
                double radius = std::min(width, height) * 0.38;

                constexpr double kPi = 3.14159265358979323846;
                double start_angle = 0.75 * kPi; // 135 deg (bottom-left)
                double sweep_angle = 1.50 * kPi; // 270 deg sweep to 405 deg (bottom-right)
                double end_angle = start_angle + sweep_angle;
                double val_angle = start_angle + vol * sweep_angle;

                // Track background arc
                cairo_set_line_width(cr, 3.2);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                cairo_set_source_rgba(cr, 0.55, 0.58, 0.62, 0.35);
                cairo_arc(cr, cx, cy, radius + 2.5, start_angle, end_angle);
                cairo_stroke(cr);

                // Active volume arc
                if (vol > 0.005) {
                    cairo_set_source_rgba(cr, 0.21, 0.52, 0.89, 0.95);
                    cairo_arc(cr, cx, cy, radius + 2.5, start_angle, val_angle);
                    cairo_stroke(cr);
                }

                // Knob body
                cairo_arc(cr, cx, cy, radius - 1.0, 0.0, 2.0 * kPi);
                cairo_set_source_rgba(cr, 0.22, 0.24, 0.27, 0.95);
                cairo_fill_preserve(cr);
                cairo_set_line_width(cr, 1.2);
                cairo_set_source_rgba(cr, 0.75, 0.78, 0.82, 0.45);
                cairo_stroke(cr);

                // Knob pointer notch
                double inner_r = (radius - 1.0) * 0.25;
                double outer_r = (radius - 1.0) * 0.82;
                cairo_set_line_width(cr, 2.0);
                if (vol <= 0.005) {
                    cairo_set_source_rgba(cr, 0.85, 0.32, 0.32, 0.95);
                } else {
                    cairo_set_source_rgba(cr, 0.95, 0.97, 1.0, 0.95);
                }
                cairo_move_to(cr, cx + std::cos(val_angle) * inner_r, cy + std::sin(val_angle) * inner_r);
                cairo_line_to(cr, cx + std::cos(val_angle) * outer_r, cy + std::sin(val_angle) * outer_r);
                cairo_stroke(cr);
            },
            this,
            nullptr);

        GtkGesture* knob_drag = gtk_gesture_drag_new();
        g_signal_connect(
            knob_drag, "drag-begin",
            G_CALLBACK(+[](GtkGestureDrag*, double, double, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                self->knob_drag_start_vol = self->effective_volume();
            }),
            this);
        g_signal_connect(
            knob_drag, "drag-update",
            G_CALLBACK(+[](GtkGestureDrag*, double offset_x, double offset_y, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                double delta = (-offset_y + offset_x * 0.5) / 110.0;
                self->set_audio_volume_from_knob(self->knob_drag_start_vol + delta);
            }),
            this);
        gtk_widget_add_controller(volume_knob_area, GTK_EVENT_CONTROLLER(knob_drag));

        GtkEventController* knob_scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
        g_signal_connect(
            knob_scroll, "scroll",
            G_CALLBACK(+[](GtkEventControllerScroll*, double, double dy, gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                double step = (dy < 0.0) ? 0.05 : -0.05;
                self->set_audio_volume_from_knob(self->effective_volume() + step);
                return TRUE;
            }),
            this);
        gtk_widget_add_controller(volume_knob_area, knob_scroll);

        // Smooth Volume Slider (0%..100%) synced with the rotary knob
        volume_scale = GTK_SCALE(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 100.0, 1.0));
        gtk_scale_set_draw_value(volume_scale, FALSE);
        gtk_widget_set_size_request(GTK_WIDGET(volume_scale), 95, -1);
        gtk_range_set_value(GTK_RANGE(volume_scale), effective_volume() * 100.0);
        gtk_widget_set_tooltip_text(
            GTK_WIDGET(volume_scale),
            "Audio Stream Volume (0% = Faded Away while Audio Stream Stays On, 100% = Full)");
        g_signal_connect(
            volume_scale, "value-changed",
            G_CALLBACK(+[](GtkRange* range, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->suppress_volume_signal) return;
                double pct = gtk_range_get_value(range);
                self->set_audio_volume_from_knob(pct / 100.0);
            }),
            this);

        int init_pct = static_cast<int>(std::round(effective_volume() * 100.0));
        std::string init_pct_str = std::to_string(init_pct) + "%";
        volume_pct_label = GTK_LABEL(gtk_label_new(init_pct_str.c_str()));
        gtk_widget_add_css_class(GTK_WIDGET(volume_pct_label), "caption");
        gtk_widget_add_css_class(GTK_WIDGET(volume_pct_label), "numeric");
        gtk_widget_set_size_request(GTK_WIDGET(volume_pct_label), 38, -1);

        gtk_box_append(GTK_BOX(audio_vol_box), GTK_WIDGET(mute_btn));
        gtk_box_append(GTK_BOX(audio_vol_box), volume_knob_area);
        gtk_box_append(GTK_BOX(audio_vol_box), GTK_WIDGET(volume_scale));
        gtk_box_append(GTK_BOX(audio_vol_box), GTK_WIDGET(volume_pct_label));

        record_btn = GTK_BUTTON(gtk_button_new_with_label("Record Stream"));
        gtk_button_set_icon_name(record_btn, "media-record-symbolic");
        gtk_widget_set_tooltip_text(GTK_WIDGET(record_btn), "Start/Stop FFmpeg Stream Recording");
        g_signal_connect(
            record_btn, "clicked",
            G_CALLBACK((+[](GtkButton*, gpointer data) {
                auto* self = static_cast<Impl*>(data);
                if (self->recorder.is_recording()) {
                    self->recorder.stop_recording();
                } else if (self->has_selected_channel) {
                    EpgProgramme now_p{};
                    EpgProgramme next_p{};
                    std::string prog_title;
                    if (self->epg_manager->get_now_and_next(self->selected_channel, now_p, next_p)) {
                        prog_title = now_p.title;
                    }
                    if (self->recorder.start_recording(self->selected_channel, prog_title)) {
                        self->show_toast("Started recording " + self->selected_channel.name);
                    }
                } else {
                    self->show_toast("Select or connect to a channel first to record.");
                }
            })),
            this);

        record_status_label = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(record_status_label), "caption");

        fullscreen_btn = GTK_BUTTON(gtk_button_new_from_icon_name("view-fullscreen-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(fullscreen_btn), "Toggle Fullscreen (F11 / F / Double-Click)");
        g_signal_connect(
            fullscreen_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->toggle_fullscreen();
            }),
            this);

        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(active_channel_title));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(player_state_badge));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(record_status_label));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(prev_channel_btn));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(play_reconnect_btn));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(stop_btn));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(next_channel_btn));
        gtk_box_append(GTK_BOX(controls_bar), audio_vol_box);
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(record_btn));
        gtk_box_append(GTK_BOX(controls_bar), GTK_WIDGET(fullscreen_btn));
        gtk_box_append(GTK_BOX(top_player_box), controls_bar);

        gtk_paned_set_start_child(GTK_PANED(player_tab_paned), top_player_box);

        // Bottom half: Live Now/Next EPG Card + Full Channel EPG Schedule
        bottom_epg_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_widget_set_margin_start(bottom_epg_box, 14);
        gtk_widget_set_margin_end(bottom_epg_box, 14);
        gtk_widget_set_margin_top(bottom_epg_box, 10);
        gtk_widget_set_margin_bottom(bottom_epg_box, 10);

        GtkWidget* now_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_add_css_class(now_card, "card");
        gtk_widget_set_margin_bottom(now_card, 4);

        GtkWidget* now_inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(now_inner, 12);
        gtk_widget_set_margin_end(now_inner, 12);
        gtk_widget_set_margin_top(now_inner, 10);
        gtk_widget_set_margin_bottom(now_inner, 10);

        GtkWidget* now_header_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        now_title_label = GTK_LABEL(gtk_label_new("Now Playing: No EPG Data"));
        gtk_widget_add_css_class(GTK_WIDGET(now_title_label), "heading");
        gtk_widget_set_hexpand(GTK_WIDGET(now_title_label), TRUE);
        gtk_widget_set_halign(GTK_WIDGET(now_title_label), GTK_ALIGN_START);
        gtk_label_set_ellipsize(now_title_label, PANGO_ELLIPSIZE_END);

        now_time_label = GTK_LABEL(gtk_label_new(""));
        gtk_widget_add_css_class(GTK_WIDGET(now_time_label), "caption");
        gtk_widget_add_css_class(GTK_WIDGET(now_time_label), "dim-label");

        // Linked Backward / Now / Forward Programme Browse buttons
        GtkWidget* epg_browse_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(epg_browse_box, "linked");

        epg_prev_btn = GTK_BUTTON(gtk_button_new_from_icon_name("go-previous-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(epg_prev_btn), "Browse Previous Programme");
        g_signal_connect(
            epg_prev_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->step_epg_programme(-1);
            }),
            this);

        epg_now_btn = GTK_BUTTON(gtk_button_new_with_label("Live"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(epg_now_btn), "Jump to Currently Airing Programme");
        g_signal_connect(
            epg_now_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->reset_epg_browse_to_now();
            }),
            this);

        epg_next_btn = GTK_BUTTON(gtk_button_new_from_icon_name("go-next-symbolic"));
        gtk_widget_set_tooltip_text(GTK_WIDGET(epg_next_btn), "Browse Next Programme");
        g_signal_connect(
            epg_next_btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                static_cast<Impl*>(data)->step_epg_programme(1);
            }),
            this);

        gtk_box_append(GTK_BOX(epg_browse_box), GTK_WIDGET(epg_prev_btn));
        gtk_box_append(GTK_BOX(epg_browse_box), GTK_WIDGET(epg_now_btn));
        gtk_box_append(GTK_BOX(epg_browse_box), GTK_WIDGET(epg_next_btn));

        gtk_box_append(GTK_BOX(now_header_row), GTK_WIDGET(now_title_label));
        gtk_box_append(GTK_BOX(now_header_row), GTK_WIDGET(now_time_label));
        gtk_box_append(GTK_BOX(now_header_row), epg_browse_box);
        gtk_box_append(GTK_BOX(now_inner), now_header_row);

        now_progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
        gtk_box_append(GTK_BOX(now_inner), GTK_WIDGET(now_progress_bar));

        now_desc_label = GTK_LABEL(gtk_label_new("Configure an XMLTV EPG URL in Preferences to view full programme guides."));
        gtk_label_set_wrap(now_desc_label, TRUE);
        gtk_widget_set_halign(GTK_WIDGET(now_desc_label), GTK_ALIGN_START);
        gtk_widget_add_css_class(GTK_WIDGET(now_desc_label), "body");
        gtk_box_append(GTK_BOX(now_inner), GTK_WIDGET(now_desc_label));

        next_title_label = GTK_LABEL(gtk_label_new("Next: —"));
        gtk_widget_set_halign(GTK_WIDGET(next_title_label), GTK_ALIGN_START);
        gtk_widget_add_css_class(GTK_WIDGET(next_title_label), "caption");
        gtk_widget_add_css_class(GTK_WIDGET(next_title_label), "dim-label");
        gtk_box_append(GTK_BOX(now_inner), GTK_WIDGET(next_title_label));

        gtk_box_append(GTK_BOX(now_card), now_inner);
        gtk_box_append(GTK_BOX(bottom_epg_box), now_card);

        GtkWidget* sched_hdr = gtk_label_new("Upcoming Channel EPG Schedule");
        gtk_widget_add_css_class(sched_hdr, "heading");
        gtk_widget_set_halign(sched_hdr, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(bottom_epg_box), sched_hdr);

        GtkWidget* epg_scroll = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(epg_scroll, TRUE);
        channel_epg_list_box = GTK_LIST_BOX(gtk_list_box_new());
        gtk_widget_add_css_class(GTK_WIDGET(channel_epg_list_box), "boxed-list");
        gtk_list_box_set_selection_mode(channel_epg_list_box, GTK_SELECTION_NONE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(epg_scroll), GTK_WIDGET(channel_epg_list_box));
        gtk_box_append(GTK_BOX(bottom_epg_box), epg_scroll);

        gtk_paned_set_end_child(GTK_PANED(player_tab_paned), bottom_epg_box);

        adw_view_stack_add_titled_with_icon(
            view_stack, player_tab_paned, "player", "Live TV & Channel EPG", "video-display-symbolic");

        // ---- TAB 2: Full EPG Guide Search & Scheduled Recordings ----
        GtkWidget* guide_tab_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_start(guide_tab_box, 16);
        gtk_widget_set_margin_end(guide_tab_box, 16);
        gtk_widget_set_margin_top(guide_tab_box, 12);
        gtk_widget_set_margin_bottom(guide_tab_box, 12);

        global_epg_search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
        gtk_search_entry_set_placeholder_text(
            global_epg_search_entry, "Search programmes across full EPG (title, description, category, channel)...");
        g_signal_connect(
            global_epg_search_entry, "search-changed",
            G_CALLBACK(+[](GtkSearchEntry*, gpointer data) {
                static_cast<Impl*>(data)->rebuild_global_epg_search();
            }),
            this);
        gtk_box_append(GTK_BOX(guide_tab_box), GTK_WIDGET(global_epg_search_entry));

        GtkWidget* global_scroll = gtk_scrolled_window_new();
        gtk_widget_set_vexpand(global_scroll, TRUE);
        global_epg_list_box = GTK_LIST_BOX(gtk_list_box_new());
        gtk_widget_add_css_class(GTK_WIDGET(global_epg_list_box), "boxed-list");
        gtk_list_box_set_selection_mode(global_epg_list_box, GTK_SELECTION_NONE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(global_scroll), GTK_WIDGET(global_epg_list_box));
        gtk_box_append(GTK_BOX(guide_tab_box), global_scroll);

        GtkWidget* rec_sched_hdr = gtk_label_new("Scheduled EPG Recordings");
        gtk_widget_add_css_class(rec_sched_hdr, "heading");
        gtk_widget_set_halign(rec_sched_hdr, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(guide_tab_box), rec_sched_hdr);

        GtkWidget* rec_sched_scroll = gtk_scrolled_window_new();
        gtk_widget_set_size_request(rec_sched_scroll, -1, 150);
        scheduled_rec_list_box = GTK_LIST_BOX(gtk_list_box_new());
        gtk_widget_add_css_class(GTK_WIDGET(scheduled_rec_list_box), "boxed-list");
        gtk_list_box_set_selection_mode(scheduled_rec_list_box, GTK_SELECTION_NONE);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(rec_sched_scroll), GTK_WIDGET(scheduled_rec_list_box));
        gtk_box_append(GTK_BOX(guide_tab_box), rec_sched_scroll);

        adw_view_stack_add_titled_with_icon(
            view_stack, guide_tab_box, "epg-guide", "Full EPG & DVR Timers", "view-list-symbolic");

        adw_toolbar_view_set_content(content_toolbar, GTK_WIDGET(view_stack));
        AdwNavigationPage* content_nav = adw_navigation_page_new(GTK_WIDGET(content_toolbar), "Live TV & EPG");
        adw_navigation_split_view_set_content(split_view, content_nav);

        adw_toast_overlay_set_child(toast_overlay, GTK_WIDGET(split_view));
        adw_application_window_set_content(window, GTK_WIDGET(toast_overlay));

        // Keyboard shortcuts: F11 / F to toggle fullscreen, Escape to exit fullscreen,
        // PageUp / PageDown to browse channels backward / forward
        GtkEventController* key_ctrl = gtk_event_controller_key_new();
        g_signal_connect(
            key_ctrl, "key-pressed",
            G_CALLBACK((+[](GtkEventControllerKey*, guint keyval, guint, GdkModifierType, gpointer data) -> gboolean {
                auto* self = static_cast<Impl*>(data);
                if (keyval == GDK_KEY_F11) {
                    self->toggle_fullscreen();
                    return TRUE;
                }
                if (keyval == GDK_KEY_Escape && self->is_fullscreen) {
                    self->set_fullscreen(false);
                    return TRUE;
                }
                if (keyval == GDK_KEY_Page_Up) {
                    self->step_channel(-1);
                    return TRUE;
                }
                if (keyval == GDK_KEY_Page_Down) {
                    self->step_channel(1);
                    return TRUE;
                }
                if (keyval == GDK_KEY_f || keyval == GDK_KEY_F) {
                    GtkWidget* focus = gtk_window_get_focus(GTK_WINDOW(self->window));
                    if (!focus || !GTK_IS_EDITABLE(focus)) {
                        self->toggle_fullscreen();
                        return TRUE;
                    }
                }
                return FALSE;
            })),
            this);
        gtk_widget_add_controller(GTK_WIDGET(window), key_ctrl);
    }

    void wire_callbacks() {
        player.set_fullscreen_toggle_callback([this]() {
            toggle_fullscreen();
        });
        player.set_status_callback([this](const PlayerStatus& st) {
            std::string badge;
            switch (st.state) {
                case PlayerState::Stopped: badge = "Stopped"; break;
                case PlayerState::Connecting: badge = "Connecting..."; break;
                case PlayerState::Playing: badge = "Live"; break;
                case PlayerState::Reconnecting:
                    badge = "Reconnecting (#" + std::to_string(st.reconnect_attempts) + ")...";
                    break;
                case PlayerState::Error: badge = "Disconnected"; break;
            }
            gtk_label_set_text(player_state_badge, badge.c_str());
        });

        recorder.set_state_callback([this](const RecordingStats& st) {
            if (st.is_recording) {
                gtk_button_set_label(record_btn, "Stop Recording");
                gtk_widget_add_css_class(GTK_WIDGET(record_btn), "destructive-action");
                std::string info = "REC " + format_duration_hms(st.elapsed_seconds) +
                                   " (" + format_bytes(st.bytes_written) + ")";
                if (st.reconnecting) {
                    info += " [Reconnecting #" + std::to_string(st.reconnect_attempts) + "]";
                }
                gtk_label_set_text(record_status_label, info.c_str());
            } else {
                gtk_button_set_label(record_btn, "Record Stream");
                gtk_widget_remove_css_class(GTK_WIDGET(record_btn), "destructive-action");
                gtk_label_set_text(record_status_label, "");
                if (!st.output_path.empty() && st.bytes_written > 0) {
                    show_toast("Saved recording: " + st.output_path);
                }
                rebuild_scheduled_recordings_list();
            }
        });

        epg_manager->set_on_updated([this](const EpgUpdateStatus& st) {
            if (epg_connect_btn) {
                gtk_widget_set_sensitive(GTK_WIDGET(epg_connect_btn), TRUE);
                gtk_button_set_label(epg_connect_btn, "Load EPG");
            }
            update_epg_timer_countdown_ui();
            if (st.success) {
                std::string footer = "EPG: " + std::to_string(epg_manager->total_programmes()) +
                                     " programmes • Updated " + format_timestamp_hhmm(st.updated_at);
                gtk_label_set_text(epg_footer_label, footer.c_str());
                rebuild_channel_list();
                update_active_channel_epg_ui();
                rebuild_global_epg_search();
                if (!st.not_modified) {
                    show_toast(st.message);
                }
            } else {
                gtk_label_set_text(epg_footer_label, ("EPG Error: " + st.message).c_str());
                show_toast("EPG Error: " + st.message);
            }
        });
    }

    void update_epg_timer_countdown_ui() {
        if (!epg_timer_countdown_label || !epg_manager) return;

        if (epg_manager->is_fetching()) {
            gtk_label_set_text(epg_timer_countdown_label, "Updating EPG...");
            return;
        }
        if (config.epg_url.empty()) {
            gtk_label_set_text(epg_timer_countdown_label, "Timer idle");
            return;
        }

        std::time_t next_ts = epg_manager->next_update_time();
        std::time_t now = std::time(nullptr);
        if (next_ts > now) {
            std::time_t rem = next_ts - now;
            std::string txt = "Next update: " + format_duration_hms(rem);
            gtk_label_set_text(epg_timer_countdown_label, txt.c_str());
        } else {
            gtk_label_set_text(epg_timer_countdown_label, "Update due...");
        }
    }

    void load_epg_from_url(const std::string& url, bool force) {
        config.epg_url = url;
        config_store.save(config);
        if (epg_url_entry) {
            const char* cur = gtk_editable_get_text(GTK_EDITABLE(epg_url_entry));
            if (!cur || config.epg_url != cur) {
                gtk_editable_set_text(GTK_EDITABLE(epg_url_entry), config.epg_url.c_str());
            }
        }
        epg_manager->configure_auto_update(config.epg_url, config.epg_update_interval_minutes);
        update_epg_timer_countdown_ui();

        if (config.epg_url.empty()) {
            show_toast("Cleared EPG URL; auto-update timer paused.");
            return;
        }

        if (epg_connect_btn) {
            gtk_widget_set_sensitive(GTK_WIDGET(epg_connect_btn), FALSE);
            gtk_button_set_label(epg_connect_btn, "Updating...");
        }
        gtk_label_set_text(epg_footer_label, "EPG: Downloading & parsing XMLTV feed...");
        epg_manager->refresh_now(config.epg_url, force);
        update_epg_timer_countdown_ui();
    }

    void load_m3u_from_url(const std::string& url) {
        gtk_widget_set_sensitive(GTK_WIDGET(connect_btn), FALSE);
        gtk_button_set_label(connect_btn, "Loading...");

        m3u_parser.fetch_and_parse_async(url, [this](M3uParseResult res) {
            gtk_widget_set_sensitive(GTK_WIDGET(connect_btn), TRUE);
            gtk_button_set_label(connect_btn, "Load M3U");

            if (!res.success) {
                show_toast("M3U Error: " + res.error_message);
                return;
            }

            all_channels = std::move(res.channels);
            channel_by_name_idx.clear();
            channel_by_id_idx.clear();
            for (size_t i = 0; i < all_channels.size(); ++i) {
                const auto& ch = all_channels[i];
                if (!ch.name.empty()) {
                    channel_by_name_idx.try_emplace(ch.name, i);
                }
                if (!ch.id.empty() && ch.id != ch.name) {
                    channel_by_id_idx.try_emplace(to_lower_copy(ch.id), i);
                }
            }
            epg_manager->set_active_channels(all_channels);
            show_toast("Connected: Loaded " + std::to_string(all_channels.size()) + " channel(s).");

            if (epg_manager->total_programmes() > 0) {
                std::string footer = "EPG: " + std::to_string(epg_manager->total_programmes()) +
                                     " programmes (M3U channels)";
                gtk_label_set_text(epg_footer_label, footer.c_str());
            }

            // If M3U header or Xtream Codes URL provided an EPG URL and user has no EPG URL set, adopt it
            if (!res.discovered_epg_url.empty() && config.epg_url.empty()) {
                load_epg_from_url(res.discovered_epg_url, true);
            }

            rebuild_groups_dropdown();
            rebuild_channel_list();
            rebuild_global_epg_search();

            // Auto-select and play the first channel if only 1 channel or none selected yet
            if (!all_channels.empty() && !has_selected_channel) {
                select_and_play_channel(all_channels.front());
            }
        });
    }

    void rebuild_groups_dropdown() {
        std::set<std::string> unique_groups;
        for (const auto& ch : all_channels) {
            if (!ch.group.empty()) unique_groups.insert(ch.group);
        }

        group_names.clear();
        group_names.push_back("All Groups");
        for (const auto& g : unique_groups) {
            group_names.push_back(g);
        }

        std::vector<const char*> c_strs;
        for (const auto& g : group_names) c_strs.push_back(g.c_str());
        c_strs.push_back(nullptr);

        suppress_group_signal = true;
        GtkStringList* model = gtk_string_list_new(c_strs.data());
        gtk_drop_down_set_model(group_dropdown, G_LIST_MODEL(model));
        g_object_unref(model);
        gtk_drop_down_set_selected(group_dropdown, 0);
        suppress_group_signal = false;
    }

    void rebuild_channel_list() {
        clear_list_box(channel_list_box);
        filtered_channels.clear();

        const char* q_raw = gtk_editable_get_text(GTK_EDITABLE(channel_search_entry));
        std::string query = to_lower_copy(q_raw ? q_raw : "");

        guint grp_idx = gtk_drop_down_get_selected(group_dropdown);
        std::string selected_grp = (grp_idx > 0 && grp_idx < group_names.size())
                                       ? group_names[grp_idx]
                                       : "";

        constexpr size_t MAX_FILTERED_CHANNELS = 5000;
        for (const auto& ch : all_channels) {
            if (!selected_grp.empty() && ch.group != selected_grp) {
                continue;
            }
            if (!query.empty()) {
                if (!contains_ci_lower_needle(ch.name, query) &&
                    !contains_ci_lower_needle(ch.group, query) &&
                    !contains_ci_lower_needle(ch.id, query)) {
                    continue;
                }
            }
            filtered_channels.push_back(ch);
            if (filtered_channels.size() >= MAX_FILTERED_CHANNELS) {
                break;
            }
        }

        bool has_epg = epg_manager->total_programmes() > 0;
        constexpr size_t MAX_VISIBLE_ROWS = 300;
        size_t count = 0;

        for (const auto& ch : filtered_channels) {
            if (count >= MAX_VISIBLE_ROWS) {
                break;
            }
            ++count;

            GtkWidget* row_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
            gtk_widget_set_margin_start(row_box, 8);
            gtk_widget_set_margin_end(row_box, 8);
            gtk_widget_set_margin_top(row_box, 6);
            gtk_widget_set_margin_bottom(row_box, 6);

            std::string title = std::to_string(ch.number) + ". " + ch.name;
            GtkWidget* name_lbl = gtk_label_new(title.c_str());
            gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
            gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
            gtk_widget_add_css_class(name_lbl, "heading");
            gtk_box_append(GTK_BOX(row_box), name_lbl);

            EpgProgramme now_p{};
            EpgProgramme next_p{};
            if (has_epg && epg_manager->get_now_and_next(ch, now_p, next_p)) {
                std::string sub = "Now: " + now_p.title + " (" + now_p.formatted_time_range() + ")";
                GtkWidget* epg_lbl = gtk_label_new(sub.c_str());
                gtk_widget_set_halign(epg_lbl, GTK_ALIGN_START);
                gtk_label_set_ellipsize(GTK_LABEL(epg_lbl), PANGO_ELLIPSIZE_END);
                gtk_widget_add_css_class(epg_lbl, "caption");
                gtk_widget_add_css_class(epg_lbl, "dim-label");
                gtk_box_append(GTK_BOX(row_box), epg_lbl);
            } else {
                GtkWidget* grp_lbl = gtk_label_new(ch.group.c_str());
                gtk_widget_set_halign(grp_lbl, GTK_ALIGN_START);
                gtk_widget_add_css_class(grp_lbl, "caption");
                gtk_widget_add_css_class(grp_lbl, "dim-label");
                gtk_box_append(GTK_BOX(row_box), grp_lbl);
            }

            gtk_list_box_append(channel_list_box, row_box);
        }
    }

    void select_and_play_channel(const Channel& ch) {
        selected_channel = ch;
        has_selected_channel = true;
        epg_browse_index = 0;
        gtk_label_set_text(active_channel_title, (ch.name + " [" + ch.group + "]").c_str());
        player.play_channel(ch);
        update_active_channel_epg_ui();
    }

    void render_browsed_epg_card() {
        if (!has_selected_channel) return;

        if (!active_channel_programmes.empty()) {
            if (epg_browse_index >= active_channel_programmes.size()) {
                epg_browse_index = 0;
            }
            const auto& prog = active_channel_programmes[epg_browse_index];
            std::string prefix = (epg_browse_index == 0 && prog.is_currently_airing()) ? "Now: " : "Programme: ";
            std::string title_str = prefix + prog.title;
            if (!prog.category.empty()) {
                title_str += "  [" + prog.category + "]";
            }
            gtk_label_set_text(now_title_label, title_str.c_str());
            gtk_label_set_text(now_time_label, prog.formatted_time_range().c_str());
            gtk_progress_bar_set_fraction(now_progress_bar, prog.progress_fraction());
            gtk_label_set_text(now_desc_label,
                               prog.description.empty() ? "No programme description provided."
                                                        : prog.description.c_str());

            if (epg_browse_index + 1 < active_channel_programmes.size()) {
                const auto& next_p = active_channel_programmes[epg_browse_index + 1];
                std::string nxt = "Following (" + next_p.formatted_time_range() + "): " + next_p.title;
                gtk_label_set_text(next_title_label, nxt.c_str());
            } else {
                gtk_label_set_text(next_title_label, "Following: —");
            }

            if (epg_prev_btn) {
                gtk_widget_set_sensitive(GTK_WIDGET(epg_prev_btn), epg_browse_index > 0 ? TRUE : FALSE);
            }
            if (epg_now_btn) {
                gtk_widget_set_sensitive(GTK_WIDGET(epg_now_btn), epg_browse_index > 0 ? TRUE : FALSE);
            }
            if (epg_next_btn) {
                gtk_widget_set_sensitive(
                    GTK_WIDGET(epg_next_btn),
                    (epg_browse_index + 1 < active_channel_programmes.size()) ? TRUE : FALSE);
            }
        } else {
            gtk_label_set_text(now_title_label, ("Live Stream: " + selected_channel.name).c_str());
            gtk_label_set_text(now_time_label, "");
            gtk_progress_bar_set_fraction(now_progress_bar, 0.0);
            gtk_label_set_text(now_desc_label, ("URL: " + selected_channel.stream_url).c_str());
            gtk_label_set_text(next_title_label, "No matching XMLTV EPG entries for this channel.");
            if (epg_prev_btn) gtk_widget_set_sensitive(GTK_WIDGET(epg_prev_btn), FALSE);
            if (epg_now_btn) gtk_widget_set_sensitive(GTK_WIDGET(epg_now_btn), FALSE);
            if (epg_next_btn) gtk_widget_set_sensitive(GTK_WIDGET(epg_next_btn), FALSE);
        }
    }

    void update_active_channel_epg_ui() {
        if (!has_selected_channel) return;

        // Include recent & upcoming programmes (from 6 hours ago onward) so user can browse backward and forward
        std::time_t now = std::time(nullptr);
        auto all_sched = epg_manager->get_channel_schedule(selected_channel, now - 6 * 3600, 150);
        active_channel_programmes = all_sched;

        // Default epg_browse_index to currently airing programme if at 0
        if (epg_browse_index == 0 && !active_channel_programmes.empty()) {
            for (size_t i = 0; i < active_channel_programmes.size(); ++i) {
                if (active_channel_programmes[i].stop_time > now) {
                    epg_browse_index = i;
                    break;
                }
            }
        }
        render_browsed_epg_card();

        // Populate full upcoming schedule for this channel
        clear_list_box(channel_epg_list_box);
        auto schedule = epg_manager->get_channel_schedule(selected_channel, now, 100);
        for (const auto& prog : schedule) {
            AdwActionRow* row = ADW_ACTION_ROW(adw_action_row_new());
            std::string escaped_title = prog.formatted_time_range() + "  •  " + prog.title;
            char* markup_title = g_markup_escape_text(escaped_title.c_str(), -1);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), markup_title);
            g_free(markup_title);

            std::string sub = prog.description;
            if (!prog.category.empty()) {
                sub = "[" + prog.category + "] " + sub;
            }
            char* markup_sub = g_markup_escape_text(sub.c_str(), -1);
            adw_action_row_set_subtitle(row, markup_sub);
            g_free(markup_sub);

            GtkWidget* sched_btn = gtk_button_new_from_icon_name("media-record-symbolic");
            gtk_widget_set_valign(sched_btn, GTK_ALIGN_CENTER);
            gtk_widget_set_tooltip_text(sched_btn, "Schedule FFmpeg Recording for this Programme");

            struct SchedCtx {
                Impl* self;
                Channel ch;
                EpgProgramme prog;
            };
            auto* sctx = new SchedCtx{this, selected_channel, prog};
            g_signal_connect_data(
                sched_btn, "clicked",
                G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                    auto* c = static_cast<SchedCtx*>(user_data);
                    c->self->recorder.schedule_recording(c->ch, c->prog);
                    c->self->show_toast("Scheduled recording: " + c->prog.title + " (" + c->ch.name + ")");
                    c->self->rebuild_scheduled_recordings_list();
                }),
                sctx,
                +[](gpointer data, GClosure*) { delete static_cast<SchedCtx*>(data); },
                static_cast<GConnectFlags>(0));

            adw_action_row_add_suffix(row, sched_btn);
            gtk_list_box_append(channel_epg_list_box, GTK_WIDGET(row));
        }
    }

    bool find_m3u_channel_for_epg(const EpgProgramme& prog, Channel& out_ch) const {
        if (!prog.channel_name.empty()) {
            auto it = channel_by_name_idx.find(prog.channel_name);
            if (it != channel_by_name_idx.end() && it->second < all_channels.size()) {
                out_ch = all_channels[it->second];
                return true;
            }
        }
        if (!prog.channel_id.empty()) {
            auto it = channel_by_id_idx.find(to_lower_copy(prog.channel_id));
            if (it != channel_by_id_idx.end() && it->second < all_channels.size()) {
                out_ch = all_channels[it->second];
                return true;
            }
        }
        return false;
    }

    void rebuild_global_epg_search() {
        clear_list_box(global_epg_list_box);
        if (all_channels.empty()) {
            AdwActionRow* empty_row = ADW_ACTION_ROW(adw_action_row_new());
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(empty_row),
                "Load an .m3u playlist first to view EPG programmes for your M3U channels.");
            gtk_list_box_append(global_epg_list_box, GTK_WIDGET(empty_row));
            return;
        }

        const char* q = gtk_editable_get_text(GTK_EDITABLE(global_epg_search_entry));
        std::string query = q ? q : "";

        guint grp_idx = gtk_drop_down_get_selected(group_dropdown);
        std::string selected_grp = (grp_idx > 0 && grp_idx < group_names.size())
                                       ? group_names[grp_idx]
                                       : "";

        auto results = epg_manager->search_programmes(query, 150, selected_grp);
        for (const auto& prog : results) {
            AdwActionRow* row = ADW_ACTION_ROW(adw_action_row_new());
            std::string ch_display = prog.channel_name.empty() ? prog.channel_id : prog.channel_name;
            std::string t = "[" + ch_display + "]  " + prog.formatted_time_range() + "  —  " + prog.title;
            char* mt = g_markup_escape_text(t.c_str(), -1);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mt);
            g_free(mt);

            char* ms = g_markup_escape_text(prog.description.c_str(), -1);
            adw_action_row_set_subtitle(row, ms);
            g_free(ms);

            Channel matched_ch{};
            if (find_m3u_channel_for_epg(prog, matched_ch)) {
                GtkWidget* play_btn = gtk_button_new_from_icon_name("media-playback-start-symbolic");
                gtk_widget_set_valign(play_btn, GTK_ALIGN_CENTER);
                gtk_widget_set_tooltip_text(play_btn, "Watch this M3U Channel Live");

                struct PlayCtx {
                    Impl* self;
                    Channel ch;
                };
                auto* pctx = new PlayCtx{this, matched_ch};
                g_signal_connect_data(
                    play_btn, "clicked",
                    G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                        auto* c = static_cast<PlayCtx*>(user_data);
                        c->self->select_and_play_channel(c->ch);
                        adw_view_stack_set_visible_child_name(c->self->view_stack, "player");
                    }),
                    pctx,
                    +[](gpointer data, GClosure*) { delete static_cast<PlayCtx*>(data); },
                    static_cast<GConnectFlags>(0));
                adw_action_row_add_suffix(row, play_btn);

                GtkWidget* sched_btn = gtk_button_new_from_icon_name("media-record-symbolic");
                gtk_widget_set_valign(sched_btn, GTK_ALIGN_CENTER);
                gtk_widget_set_tooltip_text(sched_btn, "Schedule FFmpeg Recording for this Programme");

                struct GlobalSchedCtx {
                    Impl* self;
                    Channel ch;
                    EpgProgramme prog;
                };
                auto* sctx = new GlobalSchedCtx{this, matched_ch, prog};
                g_signal_connect_data(
                    sched_btn, "clicked",
                    G_CALLBACK(+[](GtkButton*, gpointer user_data) {
                        auto* c = static_cast<GlobalSchedCtx*>(user_data);
                        c->self->recorder.schedule_recording(c->ch, c->prog);
                        c->self->show_toast("Scheduled recording: " + c->prog.title + " (" + c->ch.name + ")");
                        c->self->rebuild_scheduled_recordings_list();
                    }),
                    sctx,
                    +[](gpointer data, GClosure*) { delete static_cast<GlobalSchedCtx*>(data); },
                    static_cast<GConnectFlags>(0));
                adw_action_row_add_suffix(row, sched_btn);
            }

            gtk_list_box_append(global_epg_list_box, GTK_WIDGET(row));
        }
    }

    void rebuild_scheduled_recordings_list() {
        clear_list_box(scheduled_rec_list_box);
        auto list = recorder.get_scheduled_recordings();
        for (const auto& item : list) {
            AdwActionRow* row = ADW_ACTION_ROW(adw_action_row_new());
            std::string status = item.completed ? "[Completed] "
                               : item.active    ? "[Recording Now] "
                                                : "[Scheduled] ";
            std::string title = status + item.channel_name + " — " + item.programme_title;
            char* mt = g_markup_escape_text(title.c_str(), -1);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mt);
            g_free(mt);

            std::string sub = format_timestamp_hhmm(item.start_time) + " – " + format_timestamp_hhmm(item.stop_time);
            adw_action_row_set_subtitle(row, sub.c_str());

            if (!item.completed) {
                GtkWidget* cancel_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
                gtk_widget_set_valign(cancel_btn, GTK_ALIGN_CENTER);
                int id = item.id;
                struct CancelCtx {
                    Impl* self;
                    int id;
                };
                auto* cctx = new CancelCtx{this, id};
                g_signal_connect_data(
                    cancel_btn, "clicked",
                    G_CALLBACK((+[](GtkButton*, gpointer data) {
                        auto* p = static_cast<CancelCtx*>(data);
                        p->self->recorder.cancel_scheduled_recording(p->id);
                        p->self->rebuild_scheduled_recordings_list();
                    })),
                    cctx,
                    +[](gpointer data, GClosure*) { delete static_cast<CancelCtx*>(data); },
                    static_cast<GConnectFlags>(0));
                adw_action_row_add_suffix(row, cancel_btn);
            }
            gtk_list_box_append(scheduled_rec_list_box, GTK_WIDGET(row));
        }
    }
};

MainWindow::MainWindow(AdwApplication* app,
                       const std::string& cli_m3u_url,
                       const std::string& cli_epg_url)
    : impl_(new Impl(app, cli_m3u_url, cli_epg_url)) {}

MainWindow::~MainWindow() {
    delete impl_;
}

GtkWindow* MainWindow::gtk_window() const {
    return GTK_WINDOW(impl_->window);
}

void MainWindow::present() {
    gtk_window_present(GTK_WINDOW(impl_->window));
}

} // namespace iptv
