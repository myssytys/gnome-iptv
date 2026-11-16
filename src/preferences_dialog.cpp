#include "preferences_dialog.hpp"

namespace iptv {

namespace {

struct PrefsState {
    GtkWindow* parent = nullptr;
    AppConfig cfg;
    PreferencesDialog::SaveCallback on_save;
    AdwEntryRow* m3u_row = nullptr;
    AdwEntryRow* epg_row = nullptr;
    AdwSpinRow* epg_interval_row = nullptr;
    AdwSwitchRow* auto_reconnect_row = nullptr;
    AdwSpinRow* reconnect_delay_row = nullptr;
    AdwSpinRow* watchdog_row = nullptr;
    AdwEntryRow* rec_dir_row = nullptr;
    AdwComboRow* rec_fmt_row = nullptr;

    void collect() {
        if (m3u_row) {
            const char* t = gtk_editable_get_text(GTK_EDITABLE(m3u_row));
            cfg.m3u_url = t ? t : "";
        }
        if (epg_row) {
            const char* t = gtk_editable_get_text(GTK_EDITABLE(epg_row));
            cfg.epg_url = t ? t : "";
        }
        if (epg_interval_row) {
            cfg.epg_update_interval_minutes = static_cast<int>(adw_spin_row_get_value(epg_interval_row));
        }
        if (auto_reconnect_row) {
            cfg.auto_reconnect = adw_switch_row_get_active(auto_reconnect_row) != FALSE;
        }
        if (reconnect_delay_row) {
            cfg.reconnect_delay_seconds = static_cast<int>(adw_spin_row_get_value(reconnect_delay_row));
        }
        if (watchdog_row) {
            cfg.watchdog_timeout_seconds = static_cast<int>(adw_spin_row_get_value(watchdog_row));
        }
        if (rec_dir_row) {
            const char* t = gtk_editable_get_text(GTK_EDITABLE(rec_dir_row));
            cfg.recording_dir = t ? t : "";
        }
        if (rec_fmt_row) {
            guint sel = adw_combo_row_get_selected(rec_fmt_row);
            cfg.recording_format = (sel == 1) ? "ts" : "mkv";
        }
    }
};

} // namespace

void PreferencesDialog::present(GtkWindow* parent, const AppConfig& current_cfg, SaveCallback on_save) {
    auto* st = new PrefsState{parent, current_cfg, std::move(on_save)};

    AdwDialog* dialog = adw_preferences_dialog_new();
    adw_dialog_set_title(dialog, "IPTV & EPG Preferences");
    adw_dialog_set_content_width(dialog, 620);
    adw_dialog_set_content_height(dialog, 580);

    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "General");
    adw_preferences_page_set_icon_name(page, "preferences-system-symbolic");

    // 1. Playlist & EPG Group
    AdwPreferencesGroup* sources_grp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(sources_grp, "HTTPS or Local M3U Playlist & XMLTV EPG");
    adw_preferences_group_set_description(
        sources_grp,
        "Configure an HTTPS .m3u URL or browse for a local .m3u / .m3u8 file, and set the full EPG XMLTV feed.");

    st->m3u_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->m3u_row), "HTTPS URL or Local .m3u File Path");
    gtk_editable_set_text(GTK_EDITABLE(st->m3u_row), current_cfg.m3u_url.c_str());

    GtkWidget* browse_m3u_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_valign(browse_m3u_btn, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(browse_m3u_btn, "flat");
    gtk_widget_set_tooltip_text(browse_m3u_btn, "Select Local .m3u / .m3u8 File...");
    g_signal_connect(
        browse_m3u_btn, "clicked",
        G_CALLBACK((+[](GtkButton*, gpointer data) {
            auto* state = static_cast<PrefsState*>(data);
            GtkFileDialog* fd = gtk_file_dialog_new();
            gtk_file_dialog_set_title(fd, "Select Local .m3u / .m3u8 Playlist");

            GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
            GtkFileFilter* ff = gtk_file_filter_new();
            gtk_file_filter_set_name(ff, "M3U Playlists (*.m3u, *.m3u8)");
            gtk_file_filter_add_pattern(ff, "*.m3u");
            gtk_file_filter_add_pattern(ff, "*.m3u8");
            g_list_store_append(filters, ff);
            g_object_unref(ff);
            GtkFileFilter* all_f = gtk_file_filter_new();
            gtk_file_filter_set_name(all_f, "All Files (*)");
            gtk_file_filter_add_pattern(all_f, "*");
            g_list_store_append(filters, all_f);
            g_object_unref(all_f);
            gtk_file_dialog_set_filters(fd, G_LIST_MODEL(filters));
            g_object_unref(filters);

            gtk_file_dialog_open(
                fd, state->parent, nullptr,
                +[](GObject* src, GAsyncResult* res, gpointer ud) {
                    auto* s = static_cast<PrefsState*>(ud);
                    GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, nullptr);
                    if (file) {
                        char* p = g_file_get_path(file);
                        if (p) {
                            if (s->m3u_row) {
                                gtk_editable_set_text(GTK_EDITABLE(s->m3u_row), p);
                            }
                            g_free(p);
                        }
                        g_object_unref(file);
                    }
                    g_object_unref(src);
                },
                state);
        })),
        st);
    adw_entry_row_add_suffix(st->m3u_row, browse_m3u_btn);
    adw_preferences_group_add(sources_grp, GTK_WIDGET(st->m3u_row));

    st->epg_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->epg_row), "XMLTV EPG URL (.xml or .xml.gz)");
    gtk_editable_set_text(GTK_EDITABLE(st->epg_row), current_cfg.epg_url.c_str());
    adw_preferences_group_add(sources_grp, GTK_WIDGET(st->epg_row));

    st->epg_interval_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1.0, 360.0, 1.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->epg_interval_row), "EPG Auto-Update Frequency (Minutes)");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->epg_interval_row),
                                "Frequently fetches and updates full EPG schedule in background");
    adw_spin_row_set_value(st->epg_interval_row, current_cfg.epg_update_interval_minutes);
    adw_preferences_group_add(sources_grp, GTK_WIDGET(st->epg_interval_row));

    adw_preferences_page_add(page, sources_grp);

    // 2. Reconnection & Resilience Group
    AdwPreferencesGroup* net_grp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(net_grp, "Stream Resilience & Auto-Reconnect");
    adw_preferences_group_set_description(
        net_grp,
        "Automatically reconnects to the same stream URL when connection drops or stalls.");

    st->auto_reconnect_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->auto_reconnect_row),
                                  "Auto-Reconnect on Stream Break");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->auto_reconnect_row),
                                "Reconnects both live player and active FFmpeg recording to the same URL");
    adw_switch_row_set_active(st->auto_reconnect_row, current_cfg.auto_reconnect ? TRUE : FALSE);
    adw_preferences_group_add(net_grp, GTK_WIDGET(st->auto_reconnect_row));

    st->reconnect_delay_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1.0, 60.0, 1.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->reconnect_delay_row),
                                  "Reconnect Retry Delay (Seconds)");
    adw_spin_row_set_value(st->reconnect_delay_row, current_cfg.reconnect_delay_seconds);
    adw_preferences_group_add(net_grp, GTK_WIDGET(st->reconnect_delay_row));

    st->watchdog_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(4.0, 120.0, 1.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->watchdog_row),
                                  "Stream Stall Watchdog Timeout (Seconds)");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(st->watchdog_row),
                                "Restarts FFmpeg connection if no frames/packets arrive within timeout");
    adw_spin_row_set_value(st->watchdog_row, current_cfg.watchdog_timeout_seconds);
    adw_preferences_group_add(net_grp, GTK_WIDGET(st->watchdog_row));

    adw_preferences_page_add(page, net_grp);

    // 3. Stream Recording Group
    AdwPreferencesGroup* rec_grp = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(rec_grp, "FFmpeg Stream Recording");

    st->rec_dir_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->rec_dir_row), "Recording Output Directory");
    gtk_editable_set_text(GTK_EDITABLE(st->rec_dir_row), current_cfg.recording_dir.c_str());
    adw_preferences_group_add(rec_grp, GTK_WIDGET(st->rec_dir_row));

    const char* formats[] = {"Matroska (.mkv) — Record .ts & Auto-Remux to .mkv on Stop", "MPEG-TS (.ts) — Keep Raw .ts Without Remuxing", nullptr};
    GtkStringList* fmt_model = gtk_string_list_new(formats);
    st->rec_fmt_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(st->rec_fmt_row), "Container Format");
    adw_combo_row_set_model(st->rec_fmt_row, G_LIST_MODEL(fmt_model));
    g_object_unref(fmt_model);
    adw_combo_row_set_selected(st->rec_fmt_row, (current_cfg.recording_format == "ts") ? 1 : 0);
    adw_preferences_group_add(rec_grp, GTK_WIDGET(st->rec_fmt_row));

    adw_preferences_page_add(page, rec_grp);
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);

    g_signal_connect(
        dialog,
        "closed",
        G_CALLBACK(+[](AdwDialog*, gpointer user_data) {
            auto* state = static_cast<PrefsState*>(user_data);
            state->collect();
            if (state->on_save) {
                state->on_save(state->cfg);
            }
            delete state;
        }),
        st);

    adw_dialog_present(dialog, GTK_WIDGET(parent));
}

} // namespace iptv

