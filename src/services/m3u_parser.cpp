#include "services/m3u_parser.hpp"

#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <thread>

namespace iptv {

namespace {

std::string trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

size_t find_ci(std::string_view haystack, std::string_view needle, size_t from = 0) {
    if (needle.empty() || haystack.size() < needle.size() || from > haystack.size() - needle.size()) {
        return std::string_view::npos;
    }
    const size_t limit = haystack.size() - needle.size();
    const unsigned char first_l = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(needle[0])));
    for (size_t i = from; i <= limit; ++i) {
        if (static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(haystack[i]))) == first_l) {
            bool match = true;
            for (size_t j = 1; j < needle.size(); ++j) {
                if (std::tolower(static_cast<unsigned char>(haystack[i + j])) !=
                    std::tolower(static_cast<unsigned char>(needle[j]))) {
                    match = false;
                    break;
                }
            }
            if (match) return i;
        }
    }
    return std::string_view::npos;
}

std::string extract_attribute(std::string_view line, std::string_view key) {
    // Search case-insensitively for key=" or key=' (e.g. tvg-ID="..." or tvg-id="...")
    size_t search_from = 0;
    while (search_from < line.size()) {
        size_t pos = find_ci(line, key, search_from);
        if (pos == std::string_view::npos) break;
        size_t after_key = pos + key.size();
        if (after_key + 1 < line.size() && line[after_key] == '=' &&
            (line[after_key + 1] == '"' || line[after_key + 1] == '\'')) {
            char quote = line[after_key + 1];
            size_t val_start = after_key + 2;
            size_t val_end = line.find(quote, val_start);
            if (val_end != std::string_view::npos) {
                return std::string(line.substr(val_start, val_end - val_start));
            }
        }
        search_from = after_key;
    }
    return {};
}

std::string derive_xtream_epg_url(const std::string& source_url) {
    // Automatically derive xmltv.php URL when given an Xtream Codes get.php?username=...&password=... URL
    size_t get_pos = find_ci(source_url, "/get.php?");
    if (get_pos == std::string_view::npos) {
        return {};
    }
    std::string base = source_url.substr(0, get_pos);
    std::string query = source_url.substr(get_pos + 9);

    auto extract_param = [&](std::string_view param_name) -> std::string {
        std::string prefix = std::string(param_name) + "=";
        size_t p = find_ci(query, prefix);
        if (p == std::string_view::npos) return {};
        p += prefix.size();
        size_t amp = query.find('&', p);
        return amp != std::string::npos ? query.substr(p, amp - p) : query.substr(p);
    };

    std::string username = extract_param("username");
    std::string password = extract_param("password");
    if (!username.empty() && !password.empty()) {
        return base + "/xmltv.php?username=" + username + "&password=" + password;
    }
    return {};
}

std::string resolve_url(const std::string& base_url, const std::string& target) {
    if (target.starts_with("http://") || target.starts_with("https://") ||
        target.starts_with("rtmp://") || target.starts_with("rtsp://") ||
        target.starts_with("udp://")  || target.starts_with("rtp://") ||
        target.starts_with("file://") || target.starts_with("/")) {
        return target;
    }
    if (base_url.empty()) return target;
    if (base_url.starts_with("/")) {
        char* dir = g_path_get_dirname(base_url.c_str());
        char* joined = g_build_filename(dir ? dir : ".", target.c_str(), nullptr);
        std::string out = joined ? joined : target;
        g_free(joined);
        g_free(dir);
        return out;
    }
    GUri* base = g_uri_parse(base_url.c_str(), G_URI_FLAGS_NONE, nullptr);
    if (!base) return target;
    GUri* resolved = g_uri_parse_relative(base, target.c_str(), G_URI_FLAGS_NONE, nullptr);
    g_uri_unref(base);
    if (!resolved) return target;
    char* str = g_uri_to_string(resolved);
    g_uri_unref(resolved);
    std::string out = str ? str : target;
    g_free(str);
    return out;
}

} // namespace

struct M3uParser::Impl {
    SoupSession* session = nullptr;

    Impl() {
        session = soup_session_new_with_options(
            "user-agent", "VLC/3.0.20 LibVLC/3.0.20",
            "timeout", 180,
            nullptr);
    }

    ~Impl() {
        if (session) {
            soup_session_abort(session);
            g_object_unref(session);
        }
    }
};

M3uParser::M3uParser() : impl_(new Impl()) {}

M3uParser::~M3uParser() {
    delete impl_;
}

M3uParseResult M3uParser::parse_content(const std::string& content, const std::string& source_url) {
    M3uParseResult result;
    std::istringstream iss(content);
    std::string line;

    bool is_hls_media_or_master = false;
    bool has_extinf = false;

    Channel pending;
    bool has_pending = false;
    std::string current_group = "General";
    int auto_number = 1;

    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty()) continue;

        if (line.starts_with("#EXTM3U")) {
            std::string tvg = extract_attribute(line, "url-tvg");
            if (tvg.empty()) tvg = extract_attribute(line, "x-tvg-url");
            if (tvg.empty()) tvg = extract_attribute(line, "tvg-url");
            if (!tvg.empty()) {
                // If comma-separated list of EPG URLs, pick first
                size_t comma = tvg.find(',');
                result.discovered_epg_url = trim(comma != std::string::npos ? tvg.substr(0, comma) : tvg);
            }
            continue;
        }

        if (line.starts_with("#EXT-X-TARGETDURATION") ||
            line.starts_with("#EXT-X-MEDIA-SEQUENCE") ||
            line.starts_with("#EXT-X-STREAM-INF")) {
            is_hls_media_or_master = true;
        }

        if (line.starts_with("#EXTGRP:")) {
            std::string grp = trim(line.substr(8));
            if (!grp.empty()) {
                current_group = grp;
                if (has_pending && pending.group.empty()) {
                    pending.group = grp;
                }
            }
            continue;
        }

        if (line.starts_with("#EXTINF:")) {
            has_extinf = true;
            pending = Channel{};
            has_pending = true;

            pending.id = extract_attribute(line, "tvg-id");
            pending.name = extract_attribute(line, "tvg-name");
            pending.logo_url = extract_attribute(line, "tvg-logo");
            pending.group = extract_attribute(line, "group-title");
            std::string chno = extract_attribute(line, "tvg-chno");
            if (!chno.empty()) {
                try { pending.number = std::stoi(chno); } catch (...) {}
            }

            // Title after last comma outside quotes (with rfind(',') fallback if attribute quotes are unbalanced)
            bool in_quotes = false;
            size_t comma_pos = std::string::npos;
            for (size_t i = 0; i < line.size(); ++i) {
                if (line[i] == '"') in_quotes = !in_quotes;
                else if (line[i] == ',' && !in_quotes) comma_pos = i;
            }
            if (comma_pos == std::string::npos) {
                comma_pos = line.rfind(',');
            }
            if (comma_pos != std::string::npos && comma_pos + 1 < line.size()) {
                std::string comma_title = trim(line.substr(comma_pos + 1));
                if (!comma_title.empty()) {
                    pending.name = comma_title;
                }
            }
            if (pending.group.empty()) {
                pending.group = current_group;
            }
            continue;
        }

        if (line.starts_with("#")) {
            continue;
        }

        // Non-comment line -> stream URL
        if (is_hls_media_or_master && !source_url.empty()) {
            // This file itself is a direct HLS stream .m3u8 rather than a multi-channel playlist!
            Channel direct_ch;
            direct_ch.number = 1;
            direct_ch.id = "direct-stream";
            direct_ch.name = "Live Stream";
            // Extract a readable name from source_url if possible
            size_t slash = source_url.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < source_url.size()) {
                std::string fname = source_url.substr(slash + 1);
                if (!fname.empty()) direct_ch.name = "Direct Stream (" + fname + ")";
            }
            direct_ch.group = "Direct Stream";
            direct_ch.stream_url = source_url;
            result.channels.clear();
            result.channels.push_back(std::move(direct_ch));
            result.success = true;
            return result;
        }

        Channel ch = has_pending ? std::move(pending) : Channel{};
        has_pending = false;

        ch.stream_url = resolve_url(source_url, line);
        if (ch.number <= 0) {
            ch.number = auto_number;
        }
        ++auto_number;

        if (ch.name.empty()) {
            ch.name = "Channel " + std::to_string(ch.number);
        }
        if (ch.id.empty()) {
            ch.id = ch.name;
        }
        if (ch.group.empty()) {
            ch.group = current_group;
        }

        result.channels.push_back(std::move(ch));
    }

    if (result.discovered_epg_url.empty() && !source_url.empty()) {
        result.discovered_epg_url = derive_xtream_epg_url(source_url);
    }

    // If no channels were parsed and source_url is non-empty, treat source_url as a direct stream URL
    if (result.channels.empty() && !source_url.empty() && !has_extinf) {
        Channel direct_ch;
        direct_ch.number = 1;
        direct_ch.id = "direct-stream";
        direct_ch.name = "Direct Channel Stream";
        direct_ch.group = "Direct Stream";
        direct_ch.stream_url = source_url;
        result.channels.push_back(std::move(direct_ch));
    }

    result.success = !result.channels.empty();
    if (!result.success) {
        result.error_message = "No valid IPTV channels found in M3U resource.";
    }
    return result;
}

struct FetchContext {
    std::string url;
    M3uParser::Callback callback;
};

struct ParsedDispatch {
    M3uParser::Callback callback;
    M3uParseResult result;
};

void M3uParser::fetch_and_parse_async(const std::string& raw_url, Callback cb) {
    std::string url = trim(raw_url);
    if (url.empty()) {
        M3uParseResult res;
        res.error_message = "M3U URL is empty.";
        cb(std::move(res));
        return;
    }

    if (url.starts_with("~/")) {
        const char* home = g_get_home_dir();
        if (home) {
            url = std::string(home) + url.substr(1);
        }
    }

    // Support local files (absolute path, file:// URI, or relative file path)
    bool is_remote = url.starts_with("http://") || url.starts_with("https://");
    if (!is_remote) {
        std::string path = url;
        if (path.starts_with("file://")) {
            GFile* gf = g_file_new_for_uri(url.c_str());
            char* local_p = g_file_get_path(gf);
            if (local_p) {
                path = local_p;
                g_free(local_p);
            }
            g_object_unref(gf);
        } else if (! g_path_is_absolute(path.c_str())) {
            char* cwd = g_get_current_dir();
            char* abs_p = g_build_filename(cwd ? cwd : ".", path.c_str(), nullptr);
            if (abs_p) {
                path = abs_p;
                g_free(abs_p);
            }
            g_free(cwd);
        }
        char* contents = nullptr;
        gsize len = 0;
        GError* err = nullptr;
        if (g_file_get_contents(path.c_str(), &contents, &len, &err)) {
            std::string str(contents, len);
            g_free(contents);
            M3uParseResult res = parse_content(str, path);
            cb(std::move(res));
        } else {
            M3uParseResult res;
            res.error_message = err ? err->message : "Failed to read local M3U file.";
            if (err) g_error_free(err);
            cb(std::move(res));
        }
        return;
    }

    SoupMessage* msg = soup_message_new("GET", url.c_str());
    if (!msg) {
        M3uParseResult res;
        res.error_message = "Invalid M3U URL: " + url;
        cb(std::move(res));
        return;
    }

    auto* ctx = new FetchContext{url, std::move(cb)};
    soup_session_send_and_read_async(
        impl_->session,
        msg,
        G_PRIORITY_DEFAULT,
        nullptr,
        [](GObject* source, GAsyncResult* async_res, gpointer user_data) {
            auto* c = static_cast<FetchContext*>(user_data);
            GError* err = nullptr;
            GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), async_res, &err);
            if (err || !bytes) {
                M3uParseResult res;
                res.error_message = err ? err->message : "Network request failed.";
                if (err) g_error_free(err);
                c->callback(std::move(res));
                delete c;
                return;
            }

            gsize size = 0;
            const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
            std::string body(data ? data : "", size);
            g_bytes_unref(bytes);

            std::string req_url = std::move(c->url);
            M3uParser::Callback cb_fn = std::move(c->callback);
            delete c;

            // Parse large M3U playlists in a background worker thread so the UI never freezes
            std::thread([body = std::move(body), req_url = std::move(req_url), cb_fn = std::move(cb_fn)]() mutable {
                M3uParseResult res = M3uParser::parse_content(body, req_url);
                g_idle_add(
                    [](gpointer data) -> gboolean {
                        auto* pack = static_cast<ParsedDispatch*>(data);
                        if (pack->callback) {
                            pack->callback(std::move(pack->result));
                        }
                        delete pack;
                        return G_SOURCE_REMOVE;
                    },
                    new ParsedDispatch{std::move(cb_fn), std::move(res)});
            }).detach();
        },
        ctx);
    g_object_unref(msg);
}

} // namespace iptv

