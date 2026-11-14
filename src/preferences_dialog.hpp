#pragma once

#include "models/types.hpp"

#include <adwaita.h>
#include <functional>

namespace iptv {

class PreferencesDialog {
public:
    using SaveCallback = std::function<void(const AppConfig&)>;

    static void present(GtkWindow* parent, const AppConfig& current_cfg, SaveCallback on_save);
};

} // namespace iptv

