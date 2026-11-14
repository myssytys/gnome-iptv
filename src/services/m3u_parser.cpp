#include "services/m3u_parser.hpp"

#include <glib.h>
#include <gio/gio.h>
#include <libsoup/soup.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string_view>
#include <thread>

namespace iptv {

namespace {

std::string_view trim_sv(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string trim(std::string_view s) {
    return std::string(trim_sv(s));
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

std::string_view extract_attribute_sv(std::string_view line, std::string_view key) {
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
                return line.substr(val_start, val_end - val_start);
            }
        }
        search_from = after_key;
    }
    return {};
}

std::string extract_attribute(std::string_view line, std::string_view key) {
    return std::string(extract_attribute_sv(line, key));
}

std::string_view extract_extinf_title_sv(std::string_view line) {
    bool in_quotes = false;
    size_t comma_pos = std::string_view::npos;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"') in_quotes = !in_quotes;
        else if (line[i] == ',' && !in_quotes) comma_pos = i;
    }
    if (comma_pos == std::string_view::npos) {
        comma_pos = line.rfind(',');
    }
    if (comma_pos != std::string_view::npos && comma_pos + 1 < line.size()) {
        return trim_sv(line.substr(comma_pos + 1));
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

namespace {

std::string to_lower_ascii(std::string_view sv) {
    std::string out(sv);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

Channel parse_slot_block_bytes(
    std::string_view block,
    const M3uChannelSlot& slot,
    const M3uPlaylistIndex& index) {
    Channel ch;
    ch.number = slot.number;
    if (slot.group_id < index.groups.size() && slot.group_id > 0) {
        ch.group = index.groups[slot.group_id];
    } else {
        ch.group = "General";
    }

    size_t pos = 0;
    while (pos < block.size()) {
        size_t eol = block.find('\n', pos);
        std::string_view raw_line = (eol != std::string_view::npos)
                                        ? block.substr(pos, eol - pos)
                                        : block.substr(pos);
        pos = (eol != std::string_view::npos) ? (eol + 1) : block.size();

        std::string_view line = trim_sv(raw_line);
        if (line.empty()) continue;

        if (line.starts_with("#EXTINF:")) {
            ch.id = extract_attribute(line, "tvg-id");
            ch.name = extract_attribute(line, "tvg-name");
            ch.logo_url = extract_attribute(line, "tvg-logo");
            std::string grp = extract_attribute(line, "group-title");
            if (!grp.empty()) ch.group = grp;

            std::string_view comma_title = extract_extinf_title_sv(line);
            if (!comma_title.empty()) {
                ch.name = std::string(comma_title);
            }
            continue;
        }
        if (line.starts_with("#EXTGRP:")) {
            std::string_view grp = trim_sv(line.substr(8));
            if (!grp.empty()) ch.group = std::string(grp);
            continue;
        }
        if (line.starts_with("#")) {
            continue;
        }
        ch.stream_url = resolve_url(index.source_url, std::string(line));
    }

    if (ch.name.empty()) {
        ch.name = "Channel " + std::to_string(ch.number);
    }
    if (ch.id.empty()) {
        ch.id = ch.name;
    }
    return ch;
}

} // namespace

M3uPlaylistIndex M3uParser::index_file(const std::string& file_path, const std::string& source_url) {
    M3uPlaylistIndex idx;
    idx.backing_file_path = file_path;
    idx.source_url = source_url.empty() ? file_path : source_url;
    idx.groups.push_back("All Groups");

    FILE* fp = std::fopen(file_path.c_str(), "rb");
    if (!fp) {
        idx.error_message = "Failed to open M3U file: " + file_path;
        return idx;
    }

    char io_buf[65536];
    std::setvbuf(fp, io_buf, _IOFBF, sizeof(io_buf));

    std::unordered_map<std::string, uint16_t> group_to_id;
    auto intern_group = [&](std::string_view grp_sv) -> uint16_t {
        if (grp_sv.empty()) grp_sv = "General";
        std::string g_str(grp_sv);
        auto it = group_to_id.find(g_str);
        if (it != group_to_id.end()) return it->second;
        if (idx.groups.size() >= 65530) return 0;
        uint16_t new_id = static_cast<uint16_t>(idx.groups.size());
        idx.groups.push_back(g_str);
        group_to_id.emplace(std::move(g_str), new_id);
        return new_id;
    };

    uint16_t current_group_id = intern_group("General");
    uint16_t pending_group_id = current_group_id;
    bool has_pending = false;
    uint64_t pending_offset = 0;
    int32_t pending_number = 0;
    std::string pending_tvg_id;
    std::string pending_name;

    bool is_hls_media_or_master = false;
    bool has_extinf = false;
    int32_t auto_number = 1;
    uint64_t current_offset = 0;

    char* line_buf = nullptr;
    size_t line_cap = 0;
    ssize_t nread = 0;

    while ((nread = ::getline(&line_buf, &line_cap, fp)) > 0) {
        uint64_t line_offset = current_offset;
        current_offset += static_cast<uint64_t>(nread);

        std::string_view line = trim_sv(std::string_view(line_buf, static_cast<size_t>(nread)));
        if (line.empty()) continue;

        if (line.starts_with("#EXTM3U")) {
            std::string tvg = extract_attribute(line, "url-tvg");
            if (tvg.empty()) tvg = extract_attribute(line, "x-tvg-url");
            if (tvg.empty()) tvg = extract_attribute(line, "tvg-url");
            if (!tvg.empty()) {
                size_t comma = tvg.find(',');
                idx.discovered_epg_url = trim(comma != std::string::npos ? tvg.substr(0, comma) : tvg);
            }
            continue;
        }

        if (line.starts_with("#EXT-X-TARGETDURATION") ||
            line.starts_with("#EXT-X-MEDIA-SEQUENCE") ||
            line.starts_with("#EXT-X-STREAM-INF")) {
            is_hls_media_or_master = true;
        }

        if (line.starts_with("#EXTGRP:")) {
            std::string_view grp = trim_sv(line.substr(8));
            if (!grp.empty()) {
                current_group_id = intern_group(grp);
                if (has_pending) pending_group_id = current_group_id;
            }
            continue;
        }

        if (line.starts_with("#EXTINF:")) {
            has_extinf = true;
            has_pending = true;
            pending_offset = line_offset;
            pending_number = 0;

            std::string_view grp_sv = extract_attribute_sv(line, "group-title");
            pending_group_id = !grp_sv.empty() ? intern_group(grp_sv) : current_group_id;

            std::string_view chno_sv = extract_attribute_sv(line, "tvg-chno");
            if (!chno_sv.empty()) {
                int val = 0;
                for (char c : chno_sv) {
                    if (c >= '0' && c <= '9') val = val * 10 + (c - '0');
                }
                if (val > 0) pending_number = val;
            }

            std::string_view tid_sv = extract_attribute_sv(line, "tvg-id");
            pending_tvg_id.assign(tid_sv.data(), tid_sv.size());

            // Only extract & keep name in memory during indexing if the channel has a tvg-id
            // (or for the first 4,096 channels in small playlists without tvg-id)
            if (!pending_tvg_id.empty() || idx.slots.size() < 4096) {
                std::string_view title_sv = extract_extinf_title_sv(line);
                if (title_sv.empty()) {
                    title_sv = extract_attribute_sv(line, "tvg-name");
                }
                pending_name.assign(title_sv.data(), title_sv.size());
            } else {
                pending_name.clear();
            }
            continue;
        }

        if (line.starts_with("#")) {
            continue;
        }

        if (is_hls_media_or_master && !idx.source_url.empty()) {
            break;
        }

        uint64_t start_off = has_pending ? pending_offset : line_offset;
        uint32_t byte_len = static_cast<uint32_t>(current_offset - start_off);
        int32_t ch_num = (has_pending && pending_number > 0) ? pending_number : auto_number;
        ++auto_number;
        uint16_t grp_id = has_pending ? pending_group_id : current_group_id;

        uint32_t slot_index = static_cast<uint32_t>(idx.slots.size());
        idx.slots.push_back(M3uChannelSlot{start_off, byte_len, ch_num, grp_id});

        if (has_pending && (!pending_tvg_id.empty() || slot_index < 4096)) {
            Channel epg_ch;
            epg_ch.number = ch_num;
            epg_ch.name = !pending_name.empty() ? pending_name : ("Channel " + std::to_string(ch_num));
            epg_ch.id = !pending_tvg_id.empty() ? pending_tvg_id : epg_ch.name;
            epg_ch.group = (grp_id < idx.groups.size()) ? idx.groups[grp_id] : "General";

            if (!pending_tvg_id.empty()) {
                idx.slot_by_tvg_id.try_emplace(to_lower_ascii(pending_tvg_id), slot_index);
            }
            if (!epg_ch.name.empty()) {
                idx.slot_by_name.try_emplace(epg_ch.name, slot_index);
            }
            idx.epg_channels.push_back(std::move(epg_ch));
        }

        has_pending = false;
        pending_number = 0;
        pending_tvg_id.clear();
        pending_name.clear();
    }

    if (line_buf) {
        std::free(line_buf);
    }
    std::fclose(fp);

    if (idx.discovered_epg_url.empty() && !idx.source_url.empty()) {
        idx.discovered_epg_url = derive_xtream_epg_url(idx.source_url);
    }

    // Handle direct single-stream .m3u8 URL
    if ((is_hls_media_or_master || (!has_extinf && idx.slots.empty())) && !idx.source_url.empty()) {
        std::string synth_m3u =
            "#EXTM3U\n#EXTINF:-1 tvg-id=\"direct-stream\" group-title=\"Direct Stream\",Live Stream\n" +
            idx.source_url + "\n";
        std::string tmp_direct = file_path + ".direct.m3u";
        g_file_set_contents(tmp_direct.c_str(), synth_m3u.c_str(), static_cast<gssize>(synth_m3u.size()), nullptr);
        return index_file(tmp_direct, idx.source_url);
    }

    idx.success = !idx.slots.empty();
    if (!idx.success) {
        idx.error_message = "No valid IPTV channels found in M3U resource.";
    }
    return idx;
}

struct IndexFetchContext {
    std::string url;
    std::string cache_file_path;
    M3uParser::IndexCallback callback;
};

struct IndexDispatch {
    M3uParser::IndexCallback callback;
    M3uPlaylistIndex index;
};

void M3uParser::fetch_and_index_async(
    const std::string& raw_url,
    const std::string& cache_file_path,
    IndexCallback cb) {
    std::string url = trim(raw_url);
    if (url.empty()) {
        M3uPlaylistIndex idx;
        idx.error_message = "M3U URL is empty.";
        cb(std::move(idx));
        return;
    }

    if (url.starts_with("~/")) {
        const char* home = g_get_home_dir();
        if (home) {
            url = std::string(home) + url.substr(1);
        }
    }

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
        } else if (!g_path_is_absolute(path.c_str())) {
            char* cwd = g_get_current_dir();
            char* abs_p = g_build_filename(cwd ? cwd : ".", path.c_str(), nullptr);
            if (abs_p) {
                path = abs_p;
                g_free(abs_p);
            }
            g_free(cwd);
        }

        // Index the local file on a background thread without loading all channels into RAM
        std::thread([path = std::move(path), cb_fn = std::move(cb)]() mutable {
            M3uPlaylistIndex idx = M3uParser::index_file(path, path);
            g_idle_add(
                [](gpointer data) -> gboolean {
                    auto* pack = static_cast<IndexDispatch*>(data);
                    if (pack->callback) {
                        pack->callback(std::move(pack->index));
                    }
                    delete pack;
                    return G_SOURCE_REMOVE;
                },
                new IndexDispatch{std::move(cb_fn), std::move(idx)});
        }).detach();
        return;
    }

    SoupMessage* msg = soup_message_new("GET", url.c_str());
    if (!msg) {
        M3uPlaylistIndex idx;
        idx.error_message = "Invalid M3U URL: " + url;
        cb(std::move(idx));
        return;
    }

    auto* ctx = new IndexFetchContext{url, cache_file_path, std::move(cb)};
    soup_session_send_and_read_async(
        impl_->session,
        msg,
        G_PRIORITY_DEFAULT,
        nullptr,
        [](GObject* source, GAsyncResult* async_res, gpointer user_data) {
            auto* c = static_cast<IndexFetchContext*>(user_data);
            GError* err = nullptr;
            GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), async_res, &err);
            if (err || !bytes) {
                M3uPlaylistIndex idx;
                idx.error_message = err ? err->message : "Network request failed.";
                if (err) g_error_free(err);
                c->callback(std::move(idx));
                delete c;
                return;
            }

            std::string req_url = std::move(c->url);
            std::string cache_path = std::move(c->cache_file_path);
            M3uParser::IndexCallback cb_fn = std::move(c->callback);
            delete c;

            // Write network bytes to disk cache and build lightweight file-offset index in background thread
            std::thread([bytes, req_url = std::move(req_url), cache_path = std::move(cache_path), cb_fn = std::move(cb_fn)]() mutable {
                gsize size = 0;
                const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
                GError* write_err = nullptr;
                bool saved = g_file_set_contents(cache_path.c_str(), data ? data : "", static_cast<gssize>(size), &write_err);
                g_bytes_unref(bytes);

                M3uPlaylistIndex idx;
                if (!saved) {
                    idx.error_message = write_err ? write_err->message : "Failed to write playlist cache file.";
                    if (write_err) g_error_free(write_err);
                } else {
                    idx = M3uParser::index_file(cache_path, req_url);
                }

                g_idle_add(
                    [](gpointer data) -> gboolean {
                        auto* pack = static_cast<IndexDispatch*>(data);
                        if (pack->callback) {
                            pack->callback(std::move(pack->index));
                        }
                        delete pack;
                        return G_SOURCE_REMOVE;
                    },
                    new IndexDispatch{std::move(cb_fn), std::move(idx)});
            }).detach();
        },
        ctx);
    g_object_unref(msg);
}

std::vector<uint32_t> M3uParser::filter_slots(
    const M3uPlaylistIndex& index,
    uint16_t group_id_filter,
    const std::string& search_query) {
    std::vector<uint32_t> matches;
    if (index.slots.empty()) return matches;

    std::string_view q_trimmed = trim_sv(search_query);
    if (q_trimmed.empty()) {
        if (group_id_filter == 0) {
            matches.resize(index.slots.size());
            for (uint32_t i = 0; i < static_cast<uint32_t>(index.slots.size()); ++i) {
                matches[i] = i;
            }
        } else {
            for (uint32_t i = 0; i < static_cast<uint32_t>(index.slots.size()); ++i) {
                if (index.slots[i].group_id == group_id_filter) {
                    matches.push_back(i);
                }
            }
        }
        return matches;
    }

    FILE* fp = std::fopen(index.backing_file_path.c_str(), "rb");
    if (!fp) return matches;

    char io_buf[65536];
    std::setvbuf(fp, io_buf, _IOFBF, sizeof(io_buf));
    std::string buf;

    for (uint32_t i = 0; i < static_cast<uint32_t>(index.slots.size()); ++i) {
        const auto& slot = index.slots[i];
        if (group_id_filter != 0 && slot.group_id != group_id_filter) {
            continue;
        }
        if (slot.byte_length == 0 || slot.byte_length > 16384) {
            continue;
        }
        buf.resize(slot.byte_length);
        if (fseeko(fp, static_cast<off_t>(slot.file_offset), SEEK_SET) != 0) {
            continue;
        }
        size_t n = std::fread(buf.data(), 1, slot.byte_length, fp);
        if (n == 0) continue;

        // Only search the #EXTINF metadata line (before the stream URL line)
        std::string_view block(buf.data(), n);
        size_t first_nl = block.find('\n');
        std::string_view extinf_part = (first_nl != std::string_view::npos) ? block.substr(0, first_nl) : block;
        if (find_ci(extinf_part, q_trimmed) != std::string_view::npos) {
            matches.push_back(i);
        }
    }

    std::fclose(fp);
    return matches;
}

Channel M3uParser::load_channel_at_slot(const M3uPlaylistIndex& index, size_t slot_idx) {
    if (slot_idx >= index.slots.size()) return Channel{};
    FILE* fp = std::fopen(index.backing_file_path.c_str(), "rb");
    if (!fp) return Channel{};

    const auto& slot = index.slots[slot_idx];
    std::string buf(slot.byte_length, '\0');
    Channel ch;
    if (slot.byte_length > 0 &&
        fseeko(fp, static_cast<off_t>(slot.file_offset), SEEK_SET) == 0) {
        size_t n = std::fread(buf.data(), 1, slot.byte_length, fp);
        if (n > 0) {
            ch = parse_slot_block_bytes(std::string_view(buf.data(), n), slot, index);
        }
    }
    std::fclose(fp);
    return ch;
}

std::vector<Channel> M3uParser::load_channels_range(
    const M3uPlaylistIndex& index,
    const std::vector<uint32_t>& matching_slots,
    size_t start_pos,
    size_t count) {
    std::vector<Channel> result;
    if (start_pos >= matching_slots.size() || count == 0) return result;

    FILE* fp = std::fopen(index.backing_file_path.c_str(), "rb");
    if (!fp) return result;

    size_t end_pos = std::min(matching_slots.size(), start_pos + count);
    result.reserve(end_pos - start_pos);

    std::string buf;
    for (size_t i = start_pos; i < end_pos; ++i) {
        uint32_t slot_idx = matching_slots[i];
        if (slot_idx >= index.slots.size()) continue;
        const auto& slot = index.slots[slot_idx];
        if (slot.byte_length == 0 || slot.byte_length > 32768) continue;

        buf.resize(slot.byte_length);
        if (fseeko(fp, static_cast<off_t>(slot.file_offset), SEEK_SET) == 0) {
            size_t n = std::fread(buf.data(), 1, slot.byte_length, fp);
            if (n > 0) {
                result.push_back(parse_slot_block_bytes(std::string_view(buf.data(), n), slot, index));
            }
        }
    }

    std::fclose(fp);
    return result;
}

bool M3uParser::find_channel_by_id_or_name(
    const M3uPlaylistIndex& index,
    const std::string& channel_id,
    const std::string& channel_name,
    Channel& out_channel) {
    if (!channel_name.empty()) {
        auto it = index.slot_by_name.find(channel_name);
        if (it != index.slot_by_name.end()) {
            out_channel = load_channel_at_slot(index, it->second);
            return !out_channel.stream_url.empty();
        }
    }
    if (!channel_id.empty()) {
        auto it = index.slot_by_tvg_id.find(to_lower_ascii(channel_id));
        if (it != index.slot_by_tvg_id.end()) {
            out_channel = load_channel_at_slot(index, it->second);
            return !out_channel.stream_url.empty();
        }
    }
    return false;
}

} // namespace iptv

