#include "services/epg_manager.hpp"

#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>
#include <libxml/xmlreader.h>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace iptv {

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string maybe_gunzip(const std::string& raw) {
    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0x1f &&
        static_cast<unsigned char>(raw[1]) == 0x8b) {
        GZlibDecompressor* decomp = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
        GInputStream* mem_in = g_memory_input_stream_new_from_data(raw.data(), raw.size(), nullptr);
        GInputStream* conv_in = g_converter_input_stream_new(mem_in, G_CONVERTER(decomp));

        std::string out;
        char buf[32768];
        GError* err = nullptr;
        while (true) {
            gssize n = g_input_stream_read(conv_in, buf, sizeof(buf), nullptr, &err);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        if (err) g_error_free(err);
        g_object_unref(conv_in);
        g_object_unref(mem_in);
        g_object_unref(decomp);
        if (!out.empty()) return out;
    }
    return raw;
}

} // namespace

std::time_t EpgManager::parse_xmltv_timestamp(const std::string& ts) {
    if (ts.size() < 14) return 0;

    auto parse_digits = [&](size_t pos, size_t len) -> int {
        int val = 0;
        for (size_t i = 0; i < len && (pos + i) < ts.size(); ++i) {
            char c = ts[pos + i];
            if (c < '0' || c > '9') return 0;
            val = val * 10 + (c - '0');
        }
        return val;
    };

    std::tm tm_utc{};
    tm_utc.tm_year = parse_digits(0, 4) - 1900;
    tm_utc.tm_mon  = parse_digits(4, 2) - 1;
    tm_utc.tm_mday = parse_digits(6, 2);
    tm_utc.tm_hour = parse_digits(8, 2);
    tm_utc.tm_min  = parse_digits(10, 2);
    tm_utc.tm_sec  = parse_digits(12, 2);
    tm_utc.tm_isdst = 0;

    std::time_t epoch = timegm(&tm_utc);
    if (epoch == static_cast<std::time_t>(-1)) {
        return 0;
    }

    // Parse optional timezone offset, e.g. " +0200" or " -0500"
    size_t pos = 14;
    while (pos < ts.size() && std::isspace(static_cast<unsigned char>(ts[pos]))) {
        ++pos;
    }
    if (pos + 5 <= ts.size() && (ts[pos] == '+' || ts[pos] == '-')) {
        int sign = (ts[pos] == '-') ? -1 : 1;
        int tz_hr = parse_digits(pos + 1, 2);
        int tz_min = parse_digits(pos + 3, 2);
        int offset_seconds = sign * (tz_hr * 3600 + tz_min * 60);
        epoch -= offset_seconds;
    }
    return epoch;
}

struct EpgManager::Impl {
    std::string db_path;
    sqlite3* db = nullptr;
    mutable std::mutex mtx;

    SoupSession* session = nullptr;
    std::string epg_url;
    int interval_minutes = 15;
    guint timer_id = 0;

    std::string last_etag;
    std::string last_modified;
    std::time_t last_updated_ts = 0;
    std::time_t next_update_ts = 0;
    int total_programmes_count = 0;

    // Map lowercase display-name -> xmltv channel id
    std::unordered_map<std::string, std::string> display_name_to_id;

    UpdateCallback on_updated;
    bool fetching = false;

    explicit Impl(std::string path) : db_path(std::move(path)) {
        session = soup_session_new_with_options(
            "user-agent", "VLC/3.0.20 LibVLC/3.0.20",
            "timeout", 180,
            nullptr);
        init_db();
    }

    ~Impl() {
        if (timer_id > 0) {
            g_source_remove(timer_id);
            timer_id = 0;
        }
        if (session) {
            soup_session_abort(session);
            g_object_unref(session);
        }
        std::lock_guard<std::mutex> lock(mtx);
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
    }

    void init_db() {
        std::lock_guard<std::mutex> lock(mtx);
        if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
            db = nullptr;
            return;
        }
        const char* schema = R"SQL(
            PRAGMA journal_mode=WAL;
            PRAGMA synchronous=NORMAL;
            CREATE TABLE IF NOT EXISTS epg_channels (
                id TEXT PRIMARY KEY,
                display_name TEXT
            );
            CREATE TABLE IF NOT EXISTS epg_programmes (
                channel_id TEXT,
                start_time INTEGER,
                stop_time INTEGER,
                title TEXT,
                subtitle TEXT,
                description TEXT,
                category TEXT,
                PRIMARY KEY (channel_id, start_time)
            );
            CREATE INDEX IF NOT EXISTS idx_epg_prog_channel_time
                ON epg_programmes(channel_id, start_time, stop_time);
            CREATE INDEX IF NOT EXISTS idx_epg_prog_channel_nocase
                ON epg_programmes(channel_id COLLATE NOCASE, stop_time, start_time);
        )SQL";
        char* err_msg = nullptr;
        sqlite3_exec(db, schema, nullptr, nullptr, &err_msg);
        if (err_msg) sqlite3_free(err_msg);

        // Load existing channel alias map and count
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT id, display_name FROM epg_channels;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* cid = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* dname = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                if (cid && dname) {
                    display_name_to_id[to_lower(dname)] = cid;
                    display_name_to_id[to_lower(cid)] = cid;
                }
            }
            sqlite3_finalize(stmt);
        }

        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM epg_programmes;", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                total_programmes_count = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }

    std::string resolve_channel_id_locked(const Channel& ch) const {
        if (!ch.id.empty()) {
            std::string lid = to_lower(ch.id);
            auto it = display_name_to_id.find(lid);
            if (it != display_name_to_id.end()) return to_lower(it->second);
            return lid;
        }
        if (!ch.name.empty()) {
            std::string lname = to_lower(ch.name);
            auto it = display_name_to_id.find(lname);
            if (it != display_name_to_id.end()) return to_lower(it->second);
            return lname;
        }
        return {};
    }
};

EpgManager::EpgManager(const std::string& db_path) : impl_(new Impl(db_path)) {}

EpgManager::~EpgManager() {
    delete impl_;
}

void EpgManager::set_on_updated(UpdateCallback cb) {
    impl_->on_updated = std::move(cb);
}

void EpgManager::configure_auto_update(const std::string& epg_url, int interval_minutes) {
    impl_->epg_url = epg_url;
    impl_->interval_minutes = std::max(1, interval_minutes);

    if (impl_->timer_id > 0) {
        g_source_remove(impl_->timer_id);
        impl_->timer_id = 0;
    }

    if (!impl_->epg_url.empty()) {
        guint interval_sec = static_cast<guint>(impl_->interval_minutes * 60);
        {
            std::lock_guard<std::mutex> lock(impl_->mtx);
            impl_->next_update_ts = std::time(nullptr) + static_cast<std::time_t>(interval_sec);
        }
        impl_->timer_id = g_timeout_add_seconds(
            interval_sec,
            [](gpointer data) -> gboolean {
                auto* self = static_cast<EpgManager*>(data);
                {
                    std::lock_guard<std::mutex> lock(self->impl_->mtx);
                    self->impl_->next_update_ts =
                        std::time(nullptr) + static_cast<std::time_t>(self->impl_->interval_minutes * 60);
                }
                self->refresh_now("", false);
                return G_SOURCE_CONTINUE;
            },
            this);
    } else {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->next_update_ts = 0;
    }
}

EpgUpdateStatus EpgManager::load_from_memory(const std::string& raw_bytes) {
    EpgUpdateStatus status;
    std::string xml_data = maybe_gunzip(raw_bytes);
    if (xml_data.empty()) {
        status.message = "Empty EPG XML payload.";
        return status;
    }

    xmlTextReaderPtr reader = xmlReaderForMemory(
        xml_data.data(),
        static_cast<int>(xml_data.size()),
        "epg.xml",
        nullptr,
        XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NONET);
    if (!reader) {
        status.message = "Failed to initialize XMLTV reader.";
        return status;
    }

    std::vector<std::pair<std::string, std::string>> parsed_channels;
    std::vector<EpgProgramme> parsed_programmes;

    std::string current_channel_id;
    std::string current_display_name;
    bool in_channel = false;

    EpgProgramme current_prog;
    bool in_programme = false;
    std::string active_tag;

    int ret = xmlTextReaderRead(reader);
    while (ret == 1) {
        int node_type = xmlTextReaderNodeType(reader);
        const xmlChar* raw_name = xmlTextReaderConstName(reader);
        std::string name = raw_name ? reinterpret_cast<const char*>(raw_name) : "";

        if (node_type == XML_READER_TYPE_ELEMENT) {
            bool is_empty = xmlTextReaderIsEmptyElement(reader) == 1;
            if (name == "channel") {
                in_channel = !is_empty;
                current_display_name.clear();
                xmlChar* id_attr = xmlTextReaderGetAttribute(reader, BAD_CAST "id");
                current_channel_id = id_attr ? reinterpret_cast<const char*>(id_attr) : "";
                if (id_attr) xmlFree(id_attr);
            } else if (name == "programme") {
                in_programme = !is_empty;
                current_prog = EpgProgramme{};
                xmlChar* ch_attr = xmlTextReaderGetAttribute(reader, BAD_CAST "channel");
                xmlChar* start_attr = xmlTextReaderGetAttribute(reader, BAD_CAST "start");
                xmlChar* stop_attr = xmlTextReaderGetAttribute(reader, BAD_CAST "stop");
                if (ch_attr) {
                    current_prog.channel_id = reinterpret_cast<const char*>(ch_attr);
                    xmlFree(ch_attr);
                }
                if (start_attr) {
                    current_prog.start_time = parse_xmltv_timestamp(reinterpret_cast<const char*>(start_attr));
                    xmlFree(start_attr);
                }
                if (stop_attr) {
                    current_prog.stop_time = parse_xmltv_timestamp(reinterpret_cast<const char*>(stop_attr));
                    xmlFree(stop_attr);
                }
            } else if (in_channel || in_programme) {
                active_tag = name;
            }
        } else if (node_type == XML_READER_TYPE_TEXT || node_type == XML_READER_TYPE_CDATA) {
            const xmlChar* val = xmlTextReaderConstValue(reader);
            if (val && !active_tag.empty()) {
                std::string text = reinterpret_cast<const char*>(val);
                if (in_channel && active_tag == "display-name") {
                    if (current_display_name.empty()) {
                        current_display_name = text;
                    }
                    parsed_channels.emplace_back(current_channel_id, text);
                } else if (in_programme) {
                    if (active_tag == "title" && current_prog.title.empty()) {
                        current_prog.title = text;
                    } else if (active_tag == "sub-title" && current_prog.subtitle.empty()) {
                        current_prog.subtitle = text;
                    } else if (active_tag == "desc" && current_prog.description.empty()) {
                        current_prog.description = text;
                    } else if (active_tag == "category" && current_prog.category.empty()) {
                        current_prog.category = text;
                    }
                }
            }
        } else if (node_type == XML_READER_TYPE_END_ELEMENT) {
            if (name == "channel") {
                if (!current_channel_id.empty() && current_display_name.empty()) {
                    parsed_channels.emplace_back(current_channel_id, current_channel_id);
                }
                in_channel = false;
                active_tag.clear();
            } else if (name == "programme") {
                if (!current_prog.channel_id.empty() && current_prog.start_time > 0 && !current_prog.title.empty()) {
                    if (current_prog.stop_time <= current_prog.start_time) {
                        current_prog.stop_time = current_prog.start_time + 1800;
                    }
                    parsed_programmes.push_back(std::move(current_prog));
                }
                in_programme = false;
                active_tag.clear();
            } else if (name == active_tag) {
                active_tag.clear();
            }
        }
        ret = xmlTextReaderRead(reader);
    }
    xmlFreeTextReader(reader);

    // Commit into SQLite
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->db) {
            sqlite3_exec(impl_->db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

            // Prune programmes older than 24 hours ago
            std::time_t cutoff = std::time(nullptr) - 86400;
            std::string prune_sql = "DELETE FROM epg_programmes WHERE stop_time < " + std::to_string(cutoff) + ";";
            sqlite3_exec(impl_->db, prune_sql.c_str(), nullptr, nullptr, nullptr);

            sqlite3_stmt* ch_stmt = nullptr;
            sqlite3_prepare_v2(impl_->db,
                "INSERT OR REPLACE INTO epg_channels (id, display_name) VALUES (?1, ?2);",
                -1, &ch_stmt, nullptr);
            for (const auto& [cid, dname] : parsed_channels) {
                if (cid.empty()) continue;
                impl_->display_name_to_id[to_lower(cid)] = cid;
                if (!dname.empty()) {
                    impl_->display_name_to_id[to_lower(dname)] = cid;
                }
                if (ch_stmt) {
                    sqlite3_bind_text(ch_stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(ch_stmt, 2, dname.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(ch_stmt);
                    sqlite3_reset(ch_stmt);
                }
            }
            if (ch_stmt) sqlite3_finalize(ch_stmt);

            sqlite3_stmt* del_overlap_stmt = nullptr;
            sqlite3_prepare_v2(impl_->db,
                "DELETE FROM epg_programmes WHERE channel_id = ?1 AND start_time < ?3 AND stop_time > ?2;",
                -1, &del_overlap_stmt, nullptr);

            sqlite3_stmt* prog_stmt = nullptr;
            sqlite3_prepare_v2(impl_->db,
                "INSERT OR REPLACE INTO epg_programmes "
                "(channel_id, start_time, stop_time, title, subtitle, description, category) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);",
                -1, &prog_stmt, nullptr);
            if (prog_stmt) {
                for (const auto& p : parsed_programmes) {
                    std::string norm_cid = to_lower(p.channel_id);
                    if (del_overlap_stmt) {
                        sqlite3_bind_text(del_overlap_stmt, 1, norm_cid.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int64(del_overlap_stmt, 2, static_cast<sqlite3_int64>(p.start_time));
                        sqlite3_bind_int64(del_overlap_stmt, 3, static_cast<sqlite3_int64>(p.stop_time));
                        sqlite3_step(del_overlap_stmt);
                        sqlite3_reset(del_overlap_stmt);
                    }
                    sqlite3_bind_text(prog_stmt, 1, norm_cid.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(prog_stmt, 2, static_cast<sqlite3_int64>(p.start_time));
                    sqlite3_bind_int64(prog_stmt, 3, static_cast<sqlite3_int64>(p.stop_time));
                    sqlite3_bind_text(prog_stmt, 4, p.title.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(prog_stmt, 5, p.subtitle.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(prog_stmt, 6, p.description.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(prog_stmt, 7, p.category.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(prog_stmt);
                    sqlite3_reset(prog_stmt);
                }
                sqlite3_finalize(prog_stmt);
            }
            if (del_overlap_stmt) sqlite3_finalize(del_overlap_stmt);

            sqlite3_exec(impl_->db, "COMMIT;", nullptr, nullptr, nullptr);

            sqlite3_stmt* cnt_stmt = nullptr;
            if (sqlite3_prepare_v2(impl_->db, "SELECT COUNT(*) FROM epg_programmes;", -1, &cnt_stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(cnt_stmt) == SQLITE_ROW) {
                    impl_->total_programmes_count = sqlite3_column_int(cnt_stmt, 0);
                }
                sqlite3_finalize(cnt_stmt);
            }
        }
        impl_->last_updated_ts = std::time(nullptr);
    }

    status.success = true;
    status.channels_parsed = static_cast<int>(parsed_channels.size());
    status.programmes_parsed = static_cast<int>(parsed_programmes.size());
    status.updated_at = impl_->last_updated_ts;
    status.message = "Loaded " + std::to_string(status.programmes_parsed) + " EPG programmes across " +
                     std::to_string(status.channels_parsed) + " channel definitions.";
    return status;
}

struct EpgAsyncNotify {
    EpgManager::UpdateCallback cb;
    EpgUpdateStatus status;
};

static gboolean notify_on_main_thread(gpointer data) {
    auto* pack = static_cast<EpgAsyncNotify*>(data);
    if (pack->cb) {
        pack->cb(pack->status);
    }
    delete pack;
    return G_SOURCE_REMOVE;
}

struct EpgHttpContext {
    EpgManager* self = nullptr;
    SoupMessage* msg = nullptr;
};

void EpgManager::refresh_now(const std::string& override_url, bool force) {
    std::string url = override_url.empty() ? impl_->epg_url : override_url;
    if (url.empty()) {
        if (impl_->on_updated) {
            EpgUpdateStatus st;
            st.success = false;
            st.message = "No EPG URL configured.";
            impl_->on_updated(st);
        }
        return;
    }

    if (impl_->fetching) {
        return;
    }

    // Local file support (for local testing or file:// URLs)
    if (url.starts_with("/") || url.starts_with("file://")) {
        std::string path = url;
        if (path.starts_with("file://")) {
            GFile* gf = g_file_new_for_uri(url.c_str());
            char* local_p = g_file_get_path(gf);
            if (local_p) {
                path = local_p;
                g_free(local_p);
            }
            g_object_unref(gf);
        }
        impl_->fetching = true;
        std::thread([this, path]() {
            char* contents = nullptr;
            gsize len = 0;
            GError* err = nullptr;
            EpgUpdateStatus st;
            if (g_file_get_contents(path.c_str(), &contents, &len, &err)) {
                std::string raw(contents, len);
                g_free(contents);
                st = this->load_from_memory(raw);
            } else {
                st.success = false;
                st.message = err ? err->message : "Failed to read local EPG file.";
                if (err) g_error_free(err);
            }
            impl_->fetching = false;
            if (impl_->on_updated) {
                g_idle_add(notify_on_main_thread, new EpgAsyncNotify{impl_->on_updated, st});
            }
        }).detach();
        return;
    }

    SoupMessage* msg = soup_message_new("GET", url.c_str());
    if (!msg) {
        if (impl_->on_updated) {
            EpgUpdateStatus st;
            st.success = false;
            st.message = "Invalid EPG URL: " + url;
            impl_->on_updated(st);
        }
        return;
    }

    SoupMessageHeaders* req_hdrs = soup_message_get_request_headers(msg);
    if (!force) {
        if (!impl_->last_etag.empty()) {
            soup_message_headers_append(req_hdrs, "If-None-Match", impl_->last_etag.c_str());
        }
        if (!impl_->last_modified.empty()) {
            soup_message_headers_append(req_hdrs, "If-Modified-Since", impl_->last_modified.c_str());
        }
    }

    impl_->fetching = true;
    auto* ctx = new EpgHttpContext{this, static_cast<SoupMessage*>(g_object_ref(msg))};
    soup_session_send_and_read_async(
        impl_->session,
        msg,
        G_PRIORITY_DEFAULT,
        nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* c = static_cast<EpgHttpContext*>(user_data);
            EpgManager* self = c->self;
            SoupMessage* message = c->msg;
            delete c;

            GError* err = nullptr;
            GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &err);
            SoupStatus http_status = soup_message_get_status(message);

            if (http_status == SOUP_STATUS_NOT_MODIFIED) {
                if (bytes) g_bytes_unref(bytes);
                if (err) g_error_free(err);
                g_object_unref(message);
                self->impl_->fetching = false;
                self->impl_->last_updated_ts = std::time(nullptr);
                if (self->impl_->on_updated) {
                    EpgUpdateStatus st;
                    st.success = true;
                    st.not_modified = true;
                    st.updated_at = self->impl_->last_updated_ts;
                    st.programmes_parsed = self->impl_->total_programmes_count;
                    st.message = "EPG is up to date (HTTP 304 Not Modified).";
                    self->impl_->on_updated(st);
                }
                return;
            }

            if (err || !bytes || !SOUP_STATUS_IS_SUCCESSFUL(http_status)) {
                EpgUpdateStatus st;
                st.success = false;
                st.message = err ? err->message
                                 : ("EPG HTTP error " + std::to_string(static_cast<int>(http_status)));
                if (err) g_error_free(err);
                if (bytes) g_bytes_unref(bytes);
                g_object_unref(message);
                self->impl_->fetching = false;
                if (self->impl_->on_updated) {
                    self->impl_->on_updated(st);
                }
                return;
            }

            SoupMessageHeaders* resp_hdrs = soup_message_get_response_headers(message);
            const char* etag = soup_message_headers_get_one(resp_hdrs, "ETag");
            const char* lmod = soup_message_headers_get_one(resp_hdrs, "Last-Modified");
            if (etag) self->impl_->last_etag = etag;
            if (lmod) self->impl_->last_modified = lmod;
            g_object_unref(message);

            gsize sz = 0;
            const char* raw_ptr = static_cast<const char*>(g_bytes_get_data(bytes, &sz));
            std::string payload(raw_ptr ? raw_ptr : "", sz);
            g_bytes_unref(bytes);

            // Parse in background worker thread so UI never stutters
            std::thread([self, payload = std::move(payload)]() {
                EpgUpdateStatus st = self->load_from_memory(payload);
                self->impl_->fetching = false;
                if (self->impl_->on_updated) {
                    g_idle_add(notify_on_main_thread, new EpgAsyncNotify{self->impl_->on_updated, st});
                }
            }).detach();
        },
        ctx);
    g_object_unref(msg);
}

bool EpgManager::get_now_and_next(const Channel& ch, EpgProgramme& out_now, EpgProgramme& out_next) const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) return false;

    std::string cid = impl_->resolve_channel_id_locked(ch);
    std::time_t now = std::time(nullptr);

    const char* sql =
        "SELECT channel_id, start_time, stop_time, title, subtitle, description, category "
        "FROM epg_programmes "
        "WHERE channel_id = ?1 AND stop_time > ?2 "
        "ORDER BY start_time ASC LIMIT 2;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(now));

    bool found_any = false;
    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EpgProgramme p;
        const char* c0 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        p.channel_id = c0 ? c0 : "";
        p.start_time = static_cast<std::time_t>(sqlite3_column_int64(stmt, 1));
        p.stop_time  = static_cast<std::time_t>(sqlite3_column_int64(stmt, 2));
        const char* c3 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* c4 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* c5 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* c6 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        p.title       = c3 ? c3 : "";
        p.subtitle    = c4 ? c4 : "";
        p.description = c5 ? c5 : "";
        p.category    = c6 ? c6 : "";

        if (idx == 0) {
            out_now = std::move(p);
            found_any = true;
        } else if (idx == 1) {
            out_next = std::move(p);
        }
        ++idx;
    }
    sqlite3_finalize(stmt);
    return found_any;
}

std::vector<EpgProgramme> EpgManager::get_channel_schedule(const Channel& ch, std::time_t from_time, int limit) const {
    std::vector<EpgProgramme> list;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) return list;

    std::string cid = impl_->resolve_channel_id_locked(ch);
    if (from_time == 0) {
        from_time = std::time(nullptr);
    }

    const char* sql =
        "SELECT channel_id, start_time, stop_time, title, subtitle, description, category "
        "FROM epg_programmes "
        "WHERE channel_id = ?1 AND stop_time > ?2 "
        "ORDER BY start_time ASC LIMIT ?3;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return list;
    }
    sqlite3_bind_text(stmt, 1, cid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, static_cast<sqlite3_int64>(from_time));
    sqlite3_bind_int(stmt, 3, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EpgProgramme p;
        const char* c0 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        p.channel_id = c0 ? c0 : "";
        p.start_time = static_cast<std::time_t>(sqlite3_column_int64(stmt, 1));
        p.stop_time  = static_cast<std::time_t>(sqlite3_column_int64(stmt, 2));
        const char* c3 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* c4 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* c5 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* c6 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        p.title       = c3 ? c3 : "";
        p.subtitle    = c4 ? c4 : "";
        p.description = c5 ? c5 : "";
        p.category    = c6 ? c6 : "";
        list.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    return list;
}

std::vector<EpgProgramme> EpgManager::search_programmes(const std::string& query, int limit) const {
    std::vector<EpgProgramme> list;
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (!impl_->db) return list;

    std::time_t now = std::time(nullptr);
    std::string pattern = "%" + query + "%";

    const char* sql =
        "SELECT channel_id, start_time, stop_time, title, subtitle, description, category "
        "FROM epg_programmes "
        "WHERE stop_time > ?1 AND (title LIKE ?2 OR description LIKE ?2 OR category LIKE ?2 OR channel_id LIKE ?2) "
        "ORDER BY start_time ASC LIMIT ?3;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return list;
    }
    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(now));
    sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        EpgProgramme p;
        const char* c0 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        p.channel_id = c0 ? c0 : "";
        p.start_time = static_cast<std::time_t>(sqlite3_column_int64(stmt, 1));
        p.stop_time  = static_cast<std::time_t>(sqlite3_column_int64(stmt, 2));
        const char* c3 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        const char* c4 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
        const char* c5 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
        const char* c6 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        p.title       = c3 ? c3 : "";
        p.subtitle    = c4 ? c4 : "";
        p.description = c5 ? c5 : "";
        p.category    = c6 ? c6 : "";
        list.push_back(std::move(p));
    }
    sqlite3_finalize(stmt);
    return list;
}

std::time_t EpgManager::last_updated() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->last_updated_ts;
}

std::time_t EpgManager::next_update_time() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->next_update_ts;
}

bool EpgManager::is_fetching() const {
    return impl_->fetching;
}

int EpgManager::total_programmes() const {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->total_programmes_count;
}

} // namespace iptv

