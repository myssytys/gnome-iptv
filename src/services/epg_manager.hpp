#pragma once

#include "models/types.hpp"

#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace iptv {

struct EpgUpdateStatus {
    bool success = false;
    bool not_modified = false;
    int channels_parsed = 0;
    int programmes_parsed = 0;
    std::time_t updated_at = 0;
    std::string message;
};

class EpgManager {
public:
    using UpdateCallback = std::function<void(const EpgUpdateStatus&)>;

    explicit EpgManager(const std::string& db_path);
    ~EpgManager();

    // Configure URL and periodic refresh interval (in minutes)
    void configure_auto_update(const std::string& epg_url, int interval_minutes);

    // Trigger an immediate EPG fetch & update from the configured (or provided) URL
    void refresh_now(const std::string& override_url = "", bool force = false);

    // Register callback invoked on the GTK main thread whenever EPG updates
    void set_on_updated(UpdateCallback cb);

    // Parse XMLTV content synchronously (also handles gzip magic bytes automatically)
    EpgUpdateStatus load_from_memory(const std::string& raw_bytes);

    // Query EPG data for a channel (matches tvg-id or channel display name)
    bool get_now_and_next(const Channel& ch, EpgProgramme& out_now, EpgProgramme& out_next) const;
    std::vector<EpgProgramme> get_channel_schedule(const Channel& ch, std::time_t from_time = 0, int limit = 200) const;
    std::vector<EpgProgramme> search_programmes(const std::string& query, int limit = 200) const;

    std::time_t last_updated() const;
    std::time_t next_update_time() const;
    bool is_fetching() const;
    int total_programmes() const;

    // Parse XMLTV timestamp "YYYYMMDDHHMMSS +HHMM" -> time_t
    static std::time_t parse_xmltv_timestamp(const std::string& ts);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv

