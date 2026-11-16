#include "application.hpp"
#include "window.hpp"

namespace iptv {

Application::Application(const std::string& cli_m3u, const std::string& cli_epg)
    : cli_m3u_(cli_m3u), cli_epg_(cli_epg) {
    app_ = adw_application_new("org.gnome.IptvPlayer", G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(
        app_, "activate",
        G_CALLBACK(+[](GApplication*, gpointer user_data) {
            auto* self = static_cast<Application*>(user_data);
            if (!self->main_window_) {
                self->main_window_ = new MainWindow(self->app_, self->cli_m3u_, self->cli_epg_);
            }
            self->main_window_->present();
        }),
        this);
}

Application::~Application() {
    delete main_window_;
    main_window_ = nullptr;
    if (app_) {
        g_object_unref(app_);
        app_ = nullptr;
    }
}

int Application::run(int argc, char** argv) {
    return g_application_run(G_APPLICATION(app_), argc, argv);
}

} // namespace iptv

