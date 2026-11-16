#include "services/tray_icon.hpp"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <glib.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

namespace iptv {

namespace {

const char* kSniIntrospectionXml = R"XML(
<node>
  <interface name="org.kde.StatusNotifierItem">
    <property name="Category" type="s" access="read"/>
    <property name="Id" type="s" access="read"/>
    <property name="Title" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="WindowId" type="i" access="read"/>
    <property name="IconThemePath" type="s" access="read"/>
    <property name="Menu" type="o" access="read"/>
    <property name="ItemIsMenu" type="b" access="read"/>
    <property name="IconName" type="s" access="read"/>
    <property name="IconPixmap" type="a(iiay)" access="read"/>
    <property name="OverlayIconName" type="s" access="read"/>
    <property name="OverlayIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionIconName" type="s" access="read"/>
    <property name="AttentionIconPixmap" type="a(iiay)" access="read"/>
    <property name="AttentionMovieName" type="s" access="read"/>
    <method name="ContextMenu">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="Activate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="ProvideXdgActivationToken">
      <arg name="token" type="s" direction="in"/>
    </method>
    <method name="SecondaryActivate">
      <arg name="x" type="i" direction="in"/>
      <arg name="y" type="i" direction="in"/>
    </method>
    <method name="XAyatanaSecondaryActivate">
      <arg name="timestamp" type="u" direction="in"/>
    </method>
    <method name="Scroll">
      <arg name="delta" type="i" direction="in"/>
      <arg name="orientation" type="s" direction="in"/>
    </method>
    <signal name="NewTitle"/>
    <signal name="NewIcon"/>
    <signal name="NewStatus">
      <arg name="status" type="s"/>
    </signal>
  </interface>
</node>
)XML";

const char* kDbusMenuIntrospectionXml = R"XML(
<node>
  <interface name="com.canonical.dbusmenu">
    <property name="Version" type="u" access="read"/>
    <property name="TextDirection" type="s" access="read"/>
    <property name="Status" type="s" access="read"/>
    <property name="IconThemePath" type="as" access="read"/>
    <method name="GetLayout">
      <arg type="i" name="parentId" direction="in"/>
      <arg type="i" name="recursionDepth" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="u" name="revision" direction="out"/>
      <arg type="(ia{sv}av)" name="layout" direction="out"/>
    </method>
    <method name="GetGroupProperties">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="as" name="propertyNames" direction="in"/>
      <arg type="a(ia{sv})" name="properties" direction="out"/>
    </method>
    <method name="GetProperty">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="name" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <method name="Event">
      <arg type="i" name="id" direction="in"/>
      <arg type="s" name="eventId" direction="in"/>
      <arg type="v" name="data" direction="in"/>
      <arg type="u" name="timestamp" direction="in"/>
    </method>
    <method name="EventGroup">
      <arg type="a(isvu)" name="events" direction="in"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <method name="AboutToShow">
      <arg type="i" name="id" direction="in"/>
      <arg type="b" name="needUpdate" direction="out"/>
    </method>
    <method name="AboutToShowGroup">
      <arg type="ai" name="ids" direction="in"/>
      <arg type="ai" name="updatesNeeded" direction="out"/>
      <arg type="ai" name="idErrors" direction="out"/>
    </method>
    <signal name="ItemsPropertiesUpdated">
      <arg type="a(ia{sv})" name="updatedProps" direction="out"/>
      <arg type="a(ias)" name="removedProps" direction="out"/>
    </signal>
    <signal name="LayoutUpdated">
      <arg type="u" name="revision" direction="out"/>
      <arg type="i" name="parent" direction="out"/>
    </signal>
  </interface>
</node>
)XML";

} // namespace

struct TrayIcon::Impl {
    Callbacks cbs;
    GDBusConnection* conn = nullptr;
    GDBusNodeInfo* sni_node_info = nullptr;
    GDBusNodeInfo* menu_node_info = nullptr;
    guint sni_reg_id = 0;
    guint menu_reg_id = 0;
    guint own_name_id = 0;
    guint watcher_watch_id = 0;

    std::string bus_name;
    bool window_visible = true;
    bool muted = false;
    std::string now_playing;
    guint32 menu_revision = 1;
    GVariant* icon_pixmap_variant = nullptr;

    ~Impl() {
        if (icon_pixmap_variant) {
            g_variant_unref(icon_pixmap_variant);
            icon_pixmap_variant = nullptr;
        }
        if (watcher_watch_id > 0) {
            g_bus_unwatch_name(watcher_watch_id);
            watcher_watch_id = 0;
        }
        if (own_name_id > 0) {
            g_bus_unown_name(own_name_id);
            own_name_id = 0;
        }
        if (conn) {
            if (sni_reg_id > 0) {
                g_dbus_connection_unregister_object(conn, sni_reg_id);
                sni_reg_id = 0;
            }
            if (menu_reg_id > 0) {
                g_dbus_connection_unregister_object(conn, menu_reg_id);
                menu_reg_id = 0;
            }
            g_object_unref(conn);
            conn = nullptr;
        }
        if (sni_node_info) {
            g_dbus_node_info_unref(sni_node_info);
            sni_node_info = nullptr;
        }
        if (menu_node_info) {
            g_dbus_node_info_unref(menu_node_info);
            menu_node_info = nullptr;
        }
    }

    GVariant* load_icon_pixmap() const {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));

        for (int size : {24, 32, 48}) {
            std::string res_path = "/org/gnome/IptvPlayer/icons/" + std::to_string(size) + "x" +
                                   std::to_string(size) + "/apps/gnome-iptv.png";
            GError* err = nullptr;
            GdkPixbuf* pb = gdk_pixbuf_new_from_resource(res_path.c_str(), &err);
            if (!pb) {
                if (err) g_error_free(err);
                continue;
            }

            int w = gdk_pixbuf_get_width(pb);
            int h = gdk_pixbuf_get_height(pb);
            int n_channels = gdk_pixbuf_get_n_channels(pb);
            int rowstride = gdk_pixbuf_get_rowstride(pb);
            const guint8* pixels = gdk_pixbuf_get_pixels(pb);

            std::vector<guint8> argb(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                const guint8* row = pixels + y * rowstride;
                for (int x = 0; x < w; ++x) {
                    size_t out_idx = (static_cast<size_t>(y) * w + x) * 4;
                    const guint8* px = row + x * n_channels;
                    guint8 r = px[0];
                    guint8 g = px[1];
                    guint8 b = px[2];
                    guint8 a = (n_channels >= 4) ? px[3] : 255;
                    argb[out_idx + 0] = a;
                    argb[out_idx + 1] = r;
                    argb[out_idx + 2] = g;
                    argb[out_idx + 3] = b;
                }
            }
            g_object_unref(pb);

            GVariant* byte_data = g_variant_new_fixed_array(
                G_VARIANT_TYPE_BYTE,
                argb.data(),
                argb.size(),
                sizeof(guint8));
            g_variant_builder_add(&b, "(ii@ay)", w, h, byte_data);
        }

        return g_variant_builder_end(&b);
    }

    void fill_item_properties(int id, GVariantBuilder* builder) const {
        if (id == 0) {
            g_variant_builder_add(builder, "{sv}", "children-display", g_variant_new_string("submenu"));
            return;
        }
        if (id == 1) {
            const char* lbl = window_visible ? "Minimize to System Tray" : "Restore GNOME IPTV Window";
            g_variant_builder_add(builder, "{sv}", "label", g_variant_new_string(lbl));
            g_variant_builder_add(builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "icon-name", g_variant_new_string("gnome-iptv"));
            return;
        }
        if (id == 2) {
            const char* lbl = muted ? "Unfade Audio Stream" : "Fade Audio Stream Out";
            const char* icn = muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic";
            g_variant_builder_add(builder, "{sv}", "label", g_variant_new_string(lbl));
            g_variant_builder_add(builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "icon-name", g_variant_new_string(icn));
            return;
        }
        if (id == 3) {
            g_variant_builder_add(builder, "{sv}", "label", g_variant_new_string("Stop Playback"));
            g_variant_builder_add(builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "icon-name", g_variant_new_string("media-playback-stop-symbolic"));
            return;
        }
        if (id == 4) {
            g_variant_builder_add(builder, "{sv}", "type", g_variant_new_string("separator"));
            g_variant_builder_add(builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
            return;
        }
        if (id == 5) {
            g_variant_builder_add(builder, "{sv}", "label", g_variant_new_string("Quit GNOME IPTV"));
            g_variant_builder_add(builder, "{sv}", "enabled", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "visible", g_variant_new_boolean(TRUE));
            g_variant_builder_add(builder, "{sv}", "icon-name", g_variant_new_string("application-exit-symbolic"));
            return;
        }
    }

    GVariant* build_menu_item_node(int id) const {
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        fill_item_properties(id, &props);

        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (id == 0) {
            for (int child_id = 1; child_id <= 5; ++child_id) {
                g_variant_builder_add(&children, "v", build_menu_item_node(child_id));
            }
        }
        return g_variant_new("(ia{sv}av)", id, &props, &children);
    }

    void handle_menu_click(int id) {
        if (id == 1 && cbs.on_toggle_window) {
            cbs.on_toggle_window();
        } else if (id == 2 && cbs.on_toggle_mute) {
            cbs.on_toggle_mute();
        } else if (id == 3 && cbs.on_stop_playback) {
            cbs.on_stop_playback();
        } else if (id == 5 && cbs.on_quit) {
            cbs.on_quit();
        }
    }

    void register_with_watcher() {
        if (!conn || bus_name.empty()) return;
        g_dbus_connection_call(
            conn,
            "org.kde.StatusNotifierWatcher",
            "/StatusNotifierWatcher",
            "org.kde.StatusNotifierWatcher",
            "RegisterStatusNotifierItem",
            g_variant_new("(s)", bus_name.c_str()),
            nullptr,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            nullptr,
            nullptr,
            nullptr);
    }

    void emit_layout_updated() {
        if (!conn) return;
        ++menu_revision;
        g_dbus_connection_emit_signal(
            conn,
            nullptr,
            "/MenuBar",
            "com.canonical.dbusmenu",
            "LayoutUpdated",
            g_variant_new("(ui)", menu_revision, 0),
            nullptr);
        g_dbus_connection_emit_signal(
            conn,
            nullptr,
            "/StatusNotifierItem",
            "org.kde.StatusNotifierItem",
            "NewTitle",
            nullptr,
            nullptr);
    }

    static void on_sni_method_call(
        GDBusConnection*,
        const gchar*,
        const gchar*,
        const gchar*,
        const gchar* method_name,
        GVariant* parameters,
        GDBusMethodInvocation* invocation,
        gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);

        if (std::strcmp(method_name, "Activate") == 0) {
            if (self->cbs.on_toggle_window) {
                self->cbs.on_toggle_window();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "SecondaryActivate") == 0 ||
            std::strcmp(method_name, "XAyatanaSecondaryActivate") == 0) {
            if (self->cbs.on_toggle_mute) {
                self->cbs.on_toggle_mute();
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "Scroll") == 0) {
            gint32 delta = 0;
            const char* orientation = nullptr;
            g_variant_get(parameters, "(is)", &delta, &orientation);
            if (self->cbs.on_scroll_volume && delta != 0) {
                self->cbs.on_scroll_volume(delta < 0 ? 1 : -1);
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "ContextMenu") == 0 ||
            std::strcmp(method_name, "ProvideXdgActivationToken") == 0) {
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    static GVariant* on_sni_get_property(
        GDBusConnection*,
        const gchar*,
        const gchar*,
        const gchar*,
        const gchar* property_name,
        GError**,
        gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);

        if (std::strcmp(property_name, "Category") == 0) {
            return g_variant_new_string("ApplicationStatus");
        }
        if (std::strcmp(property_name, "Id") == 0) {
            return g_variant_new_string("org.gnome.IptvPlayer");
        }
        if (std::strcmp(property_name, "Title") == 0) {
            std::string title = self->now_playing.empty()
                                    ? "GNOME IPTV Player & Recorder"
                                    : ("GNOME IPTV: " + self->now_playing);
            return g_variant_new_string(title.c_str());
        }
        if (std::strcmp(property_name, "Status") == 0) {
            return g_variant_new_string("Active");
        }
        if (std::strcmp(property_name, "WindowId") == 0) {
            return g_variant_new_int32(0);
        }
        if (std::strcmp(property_name, "IconThemePath") == 0) {
            return g_variant_new_string("");
        }
        if (std::strcmp(property_name, "Menu") == 0) {
            return g_variant_new_object_path("/MenuBar");
        }
        if (std::strcmp(property_name, "ItemIsMenu") == 0) {
            return g_variant_new_boolean(FALSE);
        }
        if (std::strcmp(property_name, "IconName") == 0) {
            return g_variant_new_string("gnome-iptv");
        }
        if (std::strcmp(property_name, "OverlayIconName") == 0 ||
            std::strcmp(property_name, "AttentionMovieName") == 0) {
            return g_variant_new_string("");
        }
        if (std::strcmp(property_name, "AttentionIconName") == 0) {
            return g_variant_new_string("media-record-symbolic");
        }
        if (std::strcmp(property_name, "IconPixmap") == 0) {
            if (!self->icon_pixmap_variant) {
                self->icon_pixmap_variant = self->load_icon_pixmap();
            }
            return g_variant_ref(self->icon_pixmap_variant);
        }
        if (std::strcmp(property_name, "OverlayIconPixmap") == 0 ||
            std::strcmp(property_name, "AttentionIconPixmap") == 0) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a(iiay)"));
            return g_variant_builder_end(&b);
        }
        return nullptr;
    }

    static void on_menu_method_call(
        GDBusConnection*,
        const gchar*,
        const gchar*,
        const gchar*,
        const gchar* method_name,
        GVariant* parameters,
        GDBusMethodInvocation* invocation,
        gpointer user_data) {
        auto* self = static_cast<Impl*>(user_data);

        if (std::strcmp(method_name, "GetLayout") == 0) {
            gint32 parent_id = 0;
            gint32 recursion_depth = -1;
            GVariantIter* prop_iter = nullptr;
            g_variant_get(parameters, "(iias)", &parent_id, &recursion_depth, &prop_iter);
            if (prop_iter) g_variant_iter_free(prop_iter);

            GVariant* layout = self->build_menu_item_node(parent_id);
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(u@(ia{sv}av))", self->menu_revision, layout));
            return;
        }
        if (std::strcmp(method_name, "GetGroupProperties") == 0) {
            GVariantIter* id_iter = nullptr;
            GVariantIter* prop_iter = nullptr;
            g_variant_get(parameters, "(aias)", &id_iter, &prop_iter);
            if (prop_iter) g_variant_iter_free(prop_iter);

            GVariantBuilder res;
            g_variant_builder_init(&res, G_VARIANT_TYPE("a(ia{sv})"));
            if (id_iter) {
                gint32 id = 0;
                while (g_variant_iter_next(id_iter, "i", &id)) {
                    GVariantBuilder props;
                    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
                    self->fill_item_properties(id, &props);
                    g_variant_builder_add(&res, "(ia{sv})", id, &props);
                }
                g_variant_iter_free(id_iter);
            }
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &res));
            return;
        }
        if (std::strcmp(method_name, "GetProperty") == 0) {
            gint32 id = 0;
            const char* name = nullptr;
            g_variant_get(parameters, "(is)", &id, &name);
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
            self->fill_item_properties(id, &props);
            GVariant* dict = g_variant_builder_end(&props);
            GVariant* val = name ? g_variant_lookup_value(dict, name, nullptr) : nullptr;
            g_variant_unref(dict);
            if (!val) val = g_variant_new_string("");
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", val));
            return;
        }
        if (std::strcmp(method_name, "Event") == 0) {
            gint32 id = 0;
            const char* event_id = nullptr;
            GVariant* data = nullptr;
            guint32 timestamp = 0;
            g_variant_get(parameters, "(isvu)", &id, &event_id, &data, &timestamp);
            if (data) g_variant_unref(data);

            if (event_id && std::strcmp(event_id, "clicked") == 0) {
                self->handle_menu_click(id);
            }
            g_dbus_method_invocation_return_value(invocation, nullptr);
            return;
        }
        if (std::strcmp(method_name, "EventGroup") == 0) {
            GVariantIter* events = nullptr;
            g_variant_get(parameters, "(a(isvu))", &events);
            if (events) {
                gint32 id = 0;
                const char* event_id = nullptr;
                GVariant* data = nullptr;
                guint32 timestamp = 0;
                while (g_variant_iter_next(events, "(i&svu)", &id, &event_id, &data, &timestamp)) {
                    if (data) g_variant_unref(data);
                    if (event_id && std::strcmp(event_id, "clicked") == 0) {
                        self->handle_menu_click(id);
                    }
                }
                g_variant_iter_free(events);
            }
            GVariantBuilder errs;
            g_variant_builder_init(&errs, G_VARIANT_TYPE("ai"));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ai)", &errs));
            return;
        }
        if (std::strcmp(method_name, "AboutToShow") == 0) {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
            return;
        }
        if (std::strcmp(method_name, "AboutToShowGroup") == 0) {
            GVariantBuilder updates;
            GVariantBuilder errs;
            g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
            g_variant_builder_init(&errs, G_VARIANT_TYPE("ai"));
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(aiai)", &updates, &errs));
            return;
        }
    }

    static GVariant* on_menu_get_property(
        GDBusConnection*,
        const gchar*,
        const gchar*,
        const gchar*,
        const gchar* property_name,
        GError**,
        gpointer) {
        if (std::strcmp(property_name, "Version") == 0) {
            return g_variant_new_uint32(3);
        }
        if (std::strcmp(property_name, "TextDirection") == 0) {
            return g_variant_new_string("ltr");
        }
        if (std::strcmp(property_name, "Status") == 0) {
            return g_variant_new_string("normal");
        }
        if (std::strcmp(property_name, "IconThemePath") == 0) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
            return g_variant_builder_end(&b);
        }
        return nullptr;
    }
};

TrayIcon::TrayIcon() : impl_(new Impl()) {}

TrayIcon::~TrayIcon() {
    delete impl_;
}

void TrayIcon::start(Callbacks cbs) {
    impl_->cbs = std::move(cbs);

    GError* err = nullptr;
    impl_->conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!impl_->conn) {
        if (err) g_error_free(err);
        return;
    }

    impl_->sni_node_info = g_dbus_node_info_new_for_xml(kSniIntrospectionXml, nullptr);
    impl_->menu_node_info = g_dbus_node_info_new_for_xml(kDbusMenuIntrospectionXml, nullptr);
    if (!impl_->sni_node_info || !impl_->menu_node_info) {
        return;
    }

    static const GDBusInterfaceVTable sni_vtable = {
        Impl::on_sni_method_call,
        Impl::on_sni_get_property,
        nullptr,
        {nullptr}
    };
    static const GDBusInterfaceVTable menu_vtable = {
        Impl::on_menu_method_call,
        Impl::on_menu_get_property,
        nullptr,
        {nullptr}
    };

    impl_->sni_reg_id = g_dbus_connection_register_object(
        impl_->conn,
        "/StatusNotifierItem",
        impl_->sni_node_info->interfaces[0],
        &sni_vtable,
        impl_,
        nullptr,
        nullptr);

    impl_->menu_reg_id = g_dbus_connection_register_object(
        impl_->conn,
        "/MenuBar",
        impl_->menu_node_info->interfaces[0],
        &menu_vtable,
        impl_,
        nullptr,
        nullptr);

    impl_->bus_name = "org.freedesktop.StatusNotifierItem-" + std::to_string(::getpid()) + "-1";
    impl_->own_name_id = g_bus_own_name_on_connection(
        impl_->conn,
        impl_->bus_name.c_str(),
        G_BUS_NAME_OWNER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, gpointer data) {
            static_cast<Impl*>(data)->register_with_watcher();
        },
        nullptr,
        impl_,
        nullptr);

    impl_->watcher_watch_id = g_bus_watch_name_on_connection(
        impl_->conn,
        "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_WATCHER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, gpointer data) {
            static_cast<Impl*>(data)->register_with_watcher();
        },
        nullptr,
        impl_,
        nullptr);
}

void TrayIcon::update_state(bool window_visible, bool muted, const std::string& now_playing) {
    impl_->window_visible = window_visible;
    impl_->muted = muted;
    impl_->now_playing = now_playing;
    impl_->emit_layout_updated();
}

} // namespace iptv
