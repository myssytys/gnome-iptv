#pragma once

#include "models/types.hpp"
#include <string>

namespace iptv {

class ConfigStore {
public:
    ConfigStore();
    ~ConfigStore() = default;

    AppConfig load() const;
    bool save(const AppConfig& cfg) const;

    std::string config_dir() const { return config_dir_; }
    std::string data_dir() const { return data_dir_; }
    std::string db_path() const { return data_dir_ + "/epg.db"; }

private:
    std::string config_dir_;
    std::string data_dir_;
    std::string config_file_;
};

} // namespace iptv

