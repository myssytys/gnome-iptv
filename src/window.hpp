#pragma once

#include "models/types.hpp"
#include "services/config_store.hpp"
#include "services/epg_manager.hpp"
#include "services/m3u_parser.hpp"
#include "services/stream_player.hpp"
#include "services/stream_recorder.hpp"
#include "services/tray_icon.hpp"

#include <adwaita.h>
#include <string>

namespace iptv {

class MainWindow {
public:
    explicit MainWindow(AdwApplication* app,
                        const std::string& cli_m3u_url = "",
                        const std::string& cli_epg_url = "");
    ~MainWindow();

    GtkWindow* gtk_window() const;
    void present();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace iptv

