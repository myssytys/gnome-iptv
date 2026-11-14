#pragma once

#include <adwaita.h>
#include <string>

namespace iptv {

class MainWindow;

class Application {
public:
    Application(const std::string& cli_m3u = "", const std::string& cli_epg = "");
    ~Application();

    int run(int argc, char** argv);

private:
    AdwApplication* app_ = nullptr;
    MainWindow* main_window_ = nullptr;
    std::string cli_m3u_;
    std::string cli_epg_;
};

} // namespace iptv

