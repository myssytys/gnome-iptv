#include "services/config_store.hpp"

#include <glib.h>

namespace iptv {

ConfigStore::ConfigStore() {
    const char* user_cfg = g_get_user_config_dir();
    const char* user_data = g_get_user_data_dir();
    config_dir_ = std::string(user_cfg ? user_cfg : "/tmp") + "/gnome-iptv";
    data_dir_ = std::string(user_data ? user_data : "/tmp") + "/gnome-iptv";
    config_file_ = config_dir_ + "/settings.ini";

    g_mkdir_with_parents(config_dir_.c_str(), 0755);
    g_mkdir_with_parents(data_dir_.c_str(), 0755);
}

AppConfig ConfigStore::load() const {
    AppConfig cfg;
    const char* videos_dir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
    if (videos_dir && *videos_dir) {
        cfg.recording_dir = std::string(videos_dir) + "/IPTV";
    } else {
        const char* home = g_get_home_dir();
        cfg.recording_dir = std::string(home ? home : "/tmp") + "/Videos/IPTV";
    }
    g_mkdir_with_parents(cfg.recording_dir.c_str(), 0755);

    GKeyFile* kf = g_key_file_new();
    GError* err = nullptr;
    if (g_key_file_load_from_file(kf, config_file_.c_str(), G_KEY_FILE_NONE, &err)) {
        auto get_str = [&](const char* group, const char* key, std::string& target) {
            GError* e = nullptr;
            char* val = g_key_file_get_string(kf, group, key, &e);
            if (!e && val && *val) {
                target = val;
            }
            if (e) g_error_free(e);
            g_free(val);
        };
        auto get_int = [&](const char* group, const char* key, int& target) {
            GError* e = nullptr;
            int val = g_key_file_get_integer(kf, group, key, &e);
            if (!e) {
                target = val;
            }
            if (e) g_error_free(e);
        };
        auto get_bool = [&](const char* group, const char* key, bool& target) {
            GError* e = nullptr;
            gboolean val = g_key_file_get_boolean(kf, group, key, &e);
            if (!e) {
                target = (val != FALSE);
            }
            if (e) g_error_free(e);
        };
        auto get_double = [&](const char* group, const char* key, double& target) {
            GError* e = nullptr;
            double val = g_key_file_get_double(kf, group, key, &e);
            if (!e) {
                target = val;
            }
            if (e) g_error_free(e);
        };

        get_str("Playlist", "m3u_url", cfg.m3u_url);
        get_str("EPG", "epg_url", cfg.epg_url);
        get_int("EPG", "update_interval_minutes", cfg.epg_update_interval_minutes);
        get_str("Recording", "recording_dir", cfg.recording_dir);
        get_str("Recording", "recording_format", cfg.recording_format);
        get_bool("Stream", "auto_reconnect", cfg.auto_reconnect);
        get_int("Stream", "reconnect_delay_seconds", cfg.reconnect_delay_seconds);
        get_int("Stream", "watchdog_timeout_seconds", cfg.watchdog_timeout_seconds);
        get_bool("Stream", "use_ffmpeg_player", cfg.use_ffmpeg_player);
        get_bool("Stream", "mute_audio", cfg.mute_audio);
        get_double("Stream", "volume", cfg.volume);
    } else if (err) {
        g_error_free(err);
    }
    g_key_file_free(kf);

    if (cfg.epg_update_interval_minutes < 1) cfg.epg_update_interval_minutes = 15;
    if (cfg.reconnect_delay_seconds < 1) cfg.reconnect_delay_seconds = 2;
    if (cfg.watchdog_timeout_seconds < 3) cfg.watchdog_timeout_seconds = 10;
    if (cfg.volume < 0.0) cfg.volume = 0.0;
    if (cfg.volume > 1.0) cfg.volume = 1.0;
    if (cfg.recording_format != "mkv" && cfg.recording_format != "ts") {
        cfg.recording_format = "ts";
    }
    g_mkdir_with_parents(cfg.recording_dir.c_str(), 0755);
    return cfg;
}

bool ConfigStore::save(const AppConfig& cfg) const {
    GKeyFile* kf = g_key_file_new();
    g_key_file_set_string(kf, "Playlist", "m3u_url", cfg.m3u_url.c_str());
    g_key_file_set_string(kf, "EPG", "epg_url", cfg.epg_url.c_str());
    g_key_file_set_integer(kf, "EPG", "update_interval_minutes", cfg.epg_update_interval_minutes);
    g_key_file_set_string(kf, "Recording", "recording_dir", cfg.recording_dir.c_str());
    g_key_file_set_string(kf, "Recording", "recording_format", cfg.recording_format.c_str());
    g_key_file_set_boolean(kf, "Stream", "auto_reconnect", cfg.auto_reconnect ? TRUE : FALSE);
    g_key_file_set_integer(kf, "Stream", "reconnect_delay_seconds", cfg.reconnect_delay_seconds);
    g_key_file_set_integer(kf, "Stream", "watchdog_timeout_seconds", cfg.watchdog_timeout_seconds);
    g_key_file_set_boolean(kf, "Stream", "use_ffmpeg_player", cfg.use_ffmpeg_player ? TRUE : FALSE);
    g_key_file_set_boolean(kf, "Stream", "mute_audio", cfg.mute_audio ? TRUE : FALSE);
    g_key_file_set_double(kf, "Stream", "volume", cfg.volume);

    g_mkdir_with_parents(cfg.recording_dir.c_str(), 0755);

    GError* err = nullptr;
    gboolean ok = g_key_file_save_to_file(kf, config_file_.c_str(), &err);
    if (err) {
        g_error_free(err);
    }
    g_key_file_free(kf);
    return ok != FALSE;
}

} // namespace iptv

