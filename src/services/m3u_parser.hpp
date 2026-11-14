#pragma once

#include "models/types.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iptv {

struct M3uParseResult {
    bool success = false;
    std::string error_message;
    std::string discovered_epg_url;
    std::vector<Channel> channels;
};

// Compact 16-byte file-offset descriptor for lazy on-demand channel loading.
// Allows indexing 230,000+ channels in ~3.5 MB without loading all Channel strings into RAM.
struct M3uChannelSlot {
    uint64_t file_offset = 0; // Byte offset of this channel's block in backing_file_path
    uint32_t byte_length = 0; // Byte length of this channel's #EXTINF + URL lines
    int32_t number = 0;       // 1-based channel number
    uint16_t group_id = 0;    // Index into M3uPlaylistIndex::groups
};

struct M3uPlaylistIndex {
    bool success = false;
    std::string error_message;
    std::string backing_file_path; // Local .m3u path or cached download path on disk
    std::string source_url;
    std::string discovered_epg_url;
    std::vector<std::string> groups; // Index 0 = "All Groups", 1..N = unique group-title values
    std::vector<M3uChannelSlot> slots;
    std::vector<Channel> epg_channels; // Lightweight EPG-capable channel descriptors (no logo/stream URLs)
    std::unordered_map<std::string, uint32_t> slot_by_tvg_id;
    std::unordered_map<std::string, uint32_t> slot_by_name;
};

class M3uParser {
public:
    using Callback = std::function<void(M3uParseResult)>;
    using IndexCallback = std::function<void(M3uPlaylistIndex)>;

    M3uParser();
    ~M3uParser();

    // Fetch an M3U playlist from an HTTPS/HTTP/file URL asynchronously and parse it
    void fetch_and_parse_async(const std::string& url, Callback cb);

    // Stream and index a large M3U playlist on disk without loading all channels into memory at once
    void fetch_and_index_async(
        const std::string& url,
        const std::string& cache_file_path,
        IndexCallback cb);

    // Synchronous parser for raw M3U text content
    static M3uParseResult parse_content(const std::string& content, const std::string& source_url = "");

    // Build a compact file-offset index by streaming through a local .m3u file line-by-line
    static M3uPlaylistIndex index_file(const std::string& file_path, const std::string& source_url = "");

    // Filter slot indices by group_id (0 = All Groups) and optional case-insensitive search query
    static std::vector<uint32_t> filter_slots(
        const M3uPlaylistIndex& index,
        uint16_t group_id_filter,
        const std::string& search_query);

    // Load a single channel into memory on demand from its file byte offset
    static Channel load_channel_at_slot(const M3uPlaylistIndex& index, size_t slot_idx);

    // Load a batch/window of channels into memory when they approach the scroller
    static std::vector<Channel> load_channels_range(
        const M3uPlaylistIndex& index,
        const std::vector<uint32_t>& matching_slots,
        size_t start_pos,
        size_t count);

    // Resolve a channel on demand by its XMLTV id or display name
    static bool find_channel_by_id_or_name(
        const M3uPlaylistIndex& index,
        const std::string& channel_id,
        const std::string& channel_name,
        Channel& out_channel);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv

