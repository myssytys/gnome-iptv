#pragma once

#include "models/types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace iptv {

struct M3uParseResult {
    bool success = false;
    std::string error_message;
    std::string discovered_epg_url;
    std::vector<Channel> channels;
};

class M3uParser {
public:
    using Callback = std::function<void(M3uParseResult)>;

    M3uParser();
    ~M3uParser();

    // Fetch an M3U playlist from an HTTPS/HTTP/file URL asynchronously and parse it
    void fetch_and_parse_async(const std::string& url, Callback cb);

    // Synchronous parser for raw M3U text content
    static M3uParseResult parse_content(const std::string& content, const std::string& source_url = "");

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv

