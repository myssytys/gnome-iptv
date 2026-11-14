#include "models/types.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace iptv {

bool EpgProgramme::is_currently_airing(std::time_t now) const {
    if (now == 0) {
        now = std::time(nullptr);
    }
    return start_time <= now && now < stop_time;
}

double EpgProgramme::progress_fraction(std::time_t now) const {
    if (now == 0) {
        now = std::time(nullptr);
    }
    if (stop_time <= start_time || now <= start_time) {
        return 0.0;
    }
    if (now >= stop_time) {
        return 1.0;
    }
    return static_cast<double>(now - start_time) / static_cast<double>(stop_time - start_time);
}

std::string EpgProgramme::formatted_time_range() const {
    return format_timestamp_hhmm(start_time) + " – " + format_timestamp_hhmm(stop_time);
}

std::string format_timestamp_hhmm(std::time_t t) {
    if (t <= 0) return "--:--";
    std::tm local_tm{};
    localtime_r(&t, &local_tm);
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%H:%M");
    return oss.str();
}

std::string format_timestamp_full(std::time_t t) {
    if (t <= 0) return "Never";
    std::tm local_tm{};
    localtime_r(&t, &local_tm);
    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string format_duration_hms(std::time_t seconds) {
    if (seconds < 0) seconds = 0;
    long hrs = seconds / 3600;
    long mins = (seconds % 3600) / 60;
    long secs = seconds % 60;
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hrs << ":"
        << std::setfill('0') << std::setw(2) << mins << ":"
        << std::setfill('0') << std::setw(2) << secs;
    return oss.str();
}

std::string sanitize_filename(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            out.push_back(static_cast<char>(c));
        } else if (std::isspace(c) || c == '.' || c == '/' || c == ':') {
            if (!out.empty() && out.back() != '_') {
                out.push_back('_');
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        return "stream";
    }
    return out;
}

} // namespace iptv

