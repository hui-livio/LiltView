#include "global_menu.hpp"

#include "appmenu-client-protocol.h"

#include <SDL3/SDL.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gio/gio.h>
#include <spdlog/spdlog.h>
#include <wayland-client.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace liltview
{
namespace
{
constexpr const char *object_path    = "/org/liltview/MenuBar";
constexpr const char *interface_name = "com.canonical.dbusmenu";
constexpr const char *registrar_name = "com.canonical.AppMenu.Registrar";
constexpr const char *registrar_path = "/com/canonical/AppMenu/Registrar";
constexpr const char *interface_xml  = R"xml(
<node><interface name="com.canonical.dbusmenu">
 <method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
  <arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
 <method name="GetGroupProperties"><arg type="ai" direction="in"/><arg type="as" direction="in"/><arg type="a(ia{sv})" direction="out"/></method>
 <method name="GetProperty"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
 <method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
 <method name="EventGroup"><arg type="a(isvu)" direction="in"/><arg type="ai" direction="out"/></method>
 <method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
 <method name="AboutToShowGroup"><arg type="ai" direction="in"/><arg type="ai" direction="out"/><arg type="ai" direction="out"/></method>
 <property name="Version" type="u" access="read"/>
 <property name="TextDirection" type="s" access="read"/>
 <property name="Status" type="s" access="read"/>
 <property name="IconThemePath" type="as" access="read"/>
 <signal name="LayoutUpdated"><arg type="u"/><arg type="i"/></signal>
 <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})"/><arg type="a(ias)"/></signal>
 <signal name="ItemActivationRequested"><arg type="i"/><arg type="u"/></signal>
</interface></node>)xml";

std::vector<std::string> property_names(GVariant *array)
{
    std::vector<std::string> names;
    GVariantIter iterator;
    const char *name = nullptr;
    g_variant_iter_init(&iterator, array);
    while (g_variant_iter_next(&iterator, "&s", &name))
        names.emplace_back(name);
    return names;
}

GVariant *properties(const MenuItem &item, const std::vector<std::string> &filter)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
    const auto add = [&builder, &filter](const char *name, GVariant *value)
    {
        bool include = filter.empty();
        for (const auto &requested : filter)
            include = include || requested == name;
        if (include)
            g_variant_builder_add(&builder, "{sv}", name, value);
        else
            g_variant_unref(g_variant_ref_sink(value));
    };
    add("label", g_variant_new_string(item.label.c_str()));
    add("enabled", g_variant_new_boolean(item.enabled));
    add("visible", g_variant_new_boolean(TRUE));
    if (item.separator)
        add("type", g_variant_new_string("separator"));
    if (!item.children.empty())
        add("children-display", g_variant_new_string("submenu"));
    if (item.checkable)
    {
        add("toggle-type",
            g_variant_new_string((item.action == MenuAction::fullscreen || item.action == MenuAction::system_titlebar ||
                                  item.action == MenuAction::disable_rounding) ?
                                     "checkmark" :
                                     "radio"));
        add("toggle-state", g_variant_new_int32(item.checked ? 1 : 0));
    }
    if (!item.shortcut.empty())
    {
        GVariantBuilder outer;
        GVariantBuilder keys;
        g_variant_builder_init(&outer, G_VARIANT_TYPE("aas"));
        g_variant_builder_init(&keys, G_VARIANT_TYPE("as"));
        for (const auto &key : item.shortcut)
            g_variant_builder_add(&keys, "s", key.c_str());
        g_variant_builder_add_value(&outer, g_variant_builder_end(&keys));
        add("shortcut", g_variant_builder_end(&outer));
    }
    return g_variant_builder_end(&builder);
}

GVariant *layout(const MenuItem &item, int depth, const std::vector<std::string> &filter)
{
    GVariantBuilder children;
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    if (depth != 0)
    {
        for (const auto &child : item.children)
            g_variant_builder_add_value(&children, g_variant_new_variant(layout(child, depth < 0 ? -1 : depth - 1, filter)));
    }
    return g_variant_new("(i@a{sv}@av)", item.id, properties(item, filter), g_variant_builder_end(&children));
}

void append_group_properties(GVariantBuilder &builder, const MenuItem &item, const std::vector<std::string> &names)
{
    g_variant_builder_add(&builder, "(i@a{sv})", item.id, properties(item, names));
    for (const auto &child : item.children)
        append_group_properties(builder, child, names);
}

// Layout replies may omit default-valued properties, but GetProperty must still
// return their protocol defaults rather than pretending they do not exist.
GVariant *default_property(const char *name)
{
    if (std::strcmp(name, "type") == 0)
        return g_variant_new_string("standard");
    if (std::strcmp(name, "disposition") == 0)
        return g_variant_new_string("normal");
    if (std::strcmp(name, "toggle-state") == 0)
        return g_variant_new_int32(-1);
    if (std::strcmp(name, "shortcut") == 0)
        return g_variant_new_array(G_VARIANT_TYPE("as"), nullptr, 0);
    if (std::strcmp(name, "icon-data") == 0)
        return g_variant_new_array(G_VARIANT_TYPE_BYTE, nullptr, 0);
    for (const auto *property : {"icon-name", "toggle-type", "children-display", "accessible-desc"})
        if (std::strcmp(name, property) == 0)
            return g_variant_new_string("");
    return nullptr;
}
} // namespace

struct GlobalMenu::Impl
{
    explicit Impl(SDL_Window *window, const MenuItem &menu) :
        window(window),
        menu(menu)
    {
        context           = g_main_context_new();
        loop              = g_main_loop_new(context, FALSE);
        cancellable       = g_cancellable_new();
        wakeup_event      = SDL_RegisterEvents(1);
        auto ready_future = ready.get_future();
        worker            = std::thread([this] { serve(); });
        if (ready_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready || !ready_future.get())
            return;
        available = true;
        associate_window();
    }

    ~Impl()
    {
        disassociate_window();
        stopping = true;
        g_cancellable_cancel(cancellable);
        auto *source = g_idle_source_new();
        g_source_set_callback(
            source,
            [](gpointer data) -> gboolean
            {
                g_main_loop_quit(static_cast<Impl *>(data)->loop);
                return G_SOURCE_REMOVE;
            },
            this, nullptr);
        g_source_attach(source, context);
        g_source_unref(source);
        if (worker.joinable())
            worker.join();
        g_object_unref(cancellable);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
    }

    void wake() const
    {
        if (wakeup_event != static_cast<Uint32>(-1))
        {
            SDL_Event event{};
            event.type = wakeup_event;
            SDL_PushEvent(&event);
        }
    }

    bool activate(std::int32_t id, const char *event)
    {
        const auto received_ns = SDL_GetTicksNS();
        std::lock_guard lock(mutex);
        const auto *item = find_menu_item(menu, id);
        if (item == nullptr)
            return false;
        if (std::strcmp(event, "clicked") == 0 && item->enabled && item->action != MenuAction::none)
        {
            if (pending.size() < 64)
                pending.push_back({item->action, received_ns});
            wake();
        }
        return true;
    }

    // D-Bus methods share one narrow dispatcher; each method owns its parsing, validation and reply.
    void handle(const char *method, GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        if (std::strcmp(method, "Event") == 0)
            return handle_event(parameters, invocation);
        if (std::strcmp(method, "EventGroup") == 0)
            return handle_event_group(parameters, invocation);
        if (std::strcmp(method, "GetLayout") == 0)
            return handle_get_layout(parameters, invocation);
        if (std::strcmp(method, "GetGroupProperties") == 0)
            return handle_get_group_properties(parameters, invocation);
        if (std::strcmp(method, "GetProperty") == 0)
            return handle_get_property(parameters, invocation);
        if (std::strcmp(method, "AboutToShow") == 0)
            return handle_about_to_show(parameters, invocation);
        if (std::strcmp(method, "AboutToShowGroup") == 0)
            return handle_about_to_show_group(parameters, invocation);
        g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unsupported method");
    }

    void handle_event(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        gint32 id;
        const char *event;
        GVariant *data;
        guint32 timestamp;
        g_variant_get(parameters, "(i&s@vu)", &id, &event, &data, &timestamp);
        activate(id, event);
        g_variant_unref(data);
        g_dbus_method_invocation_return_value(invocation, nullptr);
    }

    void handle_event_group(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        auto *events = g_variant_get_child_value(parameters, 0);
        GVariantIter iterator;
        g_variant_iter_init(&iterator, events);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        gint32 id;
        const char *event;
        GVariant *data;
        guint32 timestamp;
        while (g_variant_iter_next(&iterator, "(i&s@vu)", &id, &event, &data, &timestamp))
        {
            if (!activate(id, event))
                g_variant_builder_add(&errors, "i", id);
            g_variant_unref(data);
        }
        g_variant_unref(events);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors)));
    }

    void handle_get_layout(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        std::lock_guard lock(mutex);
        gint32 id, depth;
        GVariant *filter;
        g_variant_get(parameters, "(ii@as)", &id, &depth, &filter);
        const auto names = property_names(filter);
        g_variant_unref(filter);
        const auto *item = find_menu_item(menu, id);
        if (item == nullptr)
            return invalid_item(invocation);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", revision, layout(*item, depth, names)));
    }

    void handle_get_group_properties(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        std::lock_guard lock(mutex);
        GVariant *ids, *filter;
        g_variant_get(parameters, "(@ai@as)", &ids, &filter);
        const auto names = property_names(filter);
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        GVariantIter iterator;
        g_variant_iter_init(&iterator, ids);
        gint32 id;
        if (g_variant_n_children(ids) == 0)
            append_group_properties(result, menu, names);
        else
            while (g_variant_iter_next(&iterator, "i", &id))
                if (const auto *item = find_menu_item(menu, id))
                    g_variant_builder_add(&result, "(i@a{sv})", id, properties(*item, names));
        g_variant_unref(ids);
        g_variant_unref(filter);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result)));
    }

    void handle_get_property(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        std::lock_guard lock(mutex);
        gint32 id;
        const char *name;
        g_variant_get(parameters, "(i&s)", &id, &name);
        const auto *item = find_menu_item(menu, id);
        if (item == nullptr)
            return invalid_item(invocation);
        auto *all   = g_variant_ref_sink(properties(*item, {}));
        auto *value = g_variant_lookup_value(all, name, nullptr);
        if (value == nullptr)
            if (auto *fallback = default_property(name))
                value = g_variant_ref_sink(fallback);
        if (value != nullptr)
        {
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
            g_variant_unref(value);
        }
        else
            g_dbus_method_invocation_return_dbus_error(invocation, "com.canonical.dbusmenu.Error.PropertyNotFound",
                                                       "Unknown property");
        g_variant_unref(all);
    }

    void handle_about_to_show(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        std::lock_guard lock(mutex);
        gint32 id;
        g_variant_get(parameters, "(i)", &id);
        if (find_menu_item(menu, id) == nullptr)
            return invalid_item(invocation);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    }

    void handle_about_to_show_group(GVariant *parameters, GDBusMethodInvocation *invocation)
    {
        std::lock_guard lock(mutex);
        GVariantBuilder updates, errors;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        auto *ids = g_variant_get_child_value(parameters, 0);
        GVariantIter iterator;
        g_variant_iter_init(&iterator, ids);
        gint32 id;
        while (g_variant_iter_next(&iterator, "i", &id))
            if (find_menu_item(menu, id) == nullptr)
                g_variant_builder_add(&errors, "i", id);
        g_variant_unref(ids);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai@ai)", g_variant_builder_end(&updates), g_variant_builder_end(&errors)));
    }

    static void invalid_item(GDBusMethodInvocation *invocation)
    {
        g_dbus_method_invocation_return_dbus_error(invocation, "com.canonical.dbusmenu.Error.InvalidMenuItem",
                                                   "Unknown menu item");
    }

    // 工作线程只管理总线生命周期；接口注册、名字监听和每种请求各有独立入口。
    void serve()
    {
        g_main_context_push_thread_default(context);
        if (!connect_session())
        {
            g_main_context_pop_thread_default(context);
            return;
        }

        GError *error           = nullptr;
        auto *node              = g_dbus_node_info_new_for_xml(interface_xml, &error);
        const auto registration = node != nullptr ? register_interface(node, &error) : 0;
        if (registration == 0)
        {
            spdlog::warn("Cannot export global menu: {}", error != nullptr ? error->message : "invalid interface");
            g_clear_error(&error);
            ready.set_value(false);
        }
        else
        {
            service = g_dbus_connection_get_unique_name(connection);
            ready.set_value(true);
            run_service_loop(registration);
        }
        if (node != nullptr)
            g_dbus_node_info_unref(node);
        g_object_unref(connection);
        connection = nullptr;
        g_main_context_pop_thread_default(context);
    }

    bool connect_session()
    {
        GError *error = nullptr;
        connection    = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, &error);
        if (connection != nullptr)
            return true;
        spdlog::warn("Global menu unavailable: {}", error != nullptr ? error->message : "no session bus");
        g_clear_error(&error);
        ready.set_value(false);
        return false;
    }

    guint register_interface(GDBusNodeInfo *node, GError **error)
    {
        // 静态回调表不依赖注册函数栈帧，明确覆盖整个 D-Bus 对象生命周期。
        static const GDBusInterfaceVTable vtable{dispatch_method, read_property, nullptr, {nullptr}};
        return g_dbus_connection_register_object(connection, object_path, node->interfaces[0], &vtable, this, nullptr, error);
    }

    static void dispatch_method(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *method,
                                GVariant *parameters, GDBusMethodInvocation *invocation, gpointer userdata)
    {
        // 不让 C++ 异常越过 GLib 的 C 回调边界。
        try
        {
            static_cast<Impl *>(userdata)->handle(method, parameters, invocation);
        } catch (const std::exception &exception)
        {
            g_dbus_method_invocation_return_dbus_error(invocation, "org.liltview.Error", exception.what());
        }
    }

    static GVariant *read_property(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *name, GError **,
                                   gpointer)
    {
        if (std::strcmp(name, "Version") == 0)
            return g_variant_new_uint32(4);
        if (std::strcmp(name, "TextDirection") == 0)
            return g_variant_new_string("ltr");
        if (std::strcmp(name, "Status") == 0)
            return g_variant_new_string("normal");
        if (std::strcmp(name, "IconThemePath") == 0)
            return g_variant_new_strv(nullptr, 0);
        return nullptr;
    }

    guint watch_registrar()
    {
        return g_bus_watch_name_on_connection(
            connection, registrar_name, G_BUS_NAME_WATCHER_FLAGS_NONE,
            [](GDBusConnection *, const gchar *, const gchar *, gpointer data) { static_cast<Impl *>(data)->register_x11(); },
            [](GDBusConnection *, const gchar *, gpointer data)
            {
                auto *self         = static_cast<Impl *>(data);
                self->x_registered = false;
                self->wake();
            },
            this, nullptr);
    }

    guint watch_panel()
    {
        return g_bus_watch_name_on_connection(
            connection, "org.kde.kappmenuview", G_BUS_NAME_WATCHER_FLAGS_NONE,
            [](GDBusConnection *, const gchar *, const gchar *, gpointer data)
            {
                auto *self  = static_cast<Impl *>(data);
                self->panel = true;
                self->wake();
            },
            [](GDBusConnection *, const gchar *, gpointer data)
            {
                auto *self  = static_cast<Impl *>(data);
                self->panel = false;
                self->wake();
            },
            this, nullptr);
    }

    void run_service_loop(guint registration)
    {
        const auto registrar_watch = watch_registrar();
        const auto panel_watch     = watch_panel();
        if (!stopping)
            g_main_loop_run(loop);
        g_bus_unwatch_name(registrar_watch);
        g_bus_unwatch_name(panel_watch);
        // 退出前收回在途异步回调，防止它们继续引用已销毁的 Impl。
        while (pending_registrations > 0)
            g_main_context_iteration(context, TRUE);
        unregister_x11();
        g_dbus_connection_unregister_object(connection, registration);
    }

    void unregister_x11()
    {
        if (!x_registered)
            return;
        g_dbus_connection_call(connection, registrar_name, registrar_path, registrar_name, "UnregisterWindow",
                               g_variant_new("(u)", x_window.load()), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr,
                               nullptr);
    }

    void register_x11()
    {
        if (x_window == 0)
            return;
        const auto id = x_window.load();
        struct Registration
        {
            Impl *self;
            uint32_t id;
        };
        auto *registration = new Registration{this, id};
        ++pending_registrations;
        g_dbus_connection_call(
            connection, registrar_name, registrar_path, registrar_name, "RegisterWindow", g_variant_new("(uo)", id, object_path),
            nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, cancellable,
            [](GObject *object, GAsyncResult *result, gpointer data)
            {
                std::unique_ptr<Registration> registration(static_cast<Registration *>(data));
                auto *self     = registration->self;
                GError *error  = nullptr;
                auto *response = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
                if (self->x_window == registration->id)
                    self->x_registered = response != nullptr;
                --self->pending_registrations;
                if (response != nullptr)
                    g_variant_unref(response);
                g_clear_error(&error);
                self->wake();
            },
            registration);
    }

    // 平台探测和各平台协议流程分开，保持 Wayland / X11 的资源边界清楚。
    void associate_window()
    {
        const auto props = SDL_GetWindowProperties(window);
        auto *display =
            static_cast<wl_display *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr));
        auto *surface =
            static_cast<wl_surface *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
        if (display != nullptr && surface != nullptr)
            associate_wayland(display, surface);
        else
            associate_x11(props);
    }

    void associate_wayland(wl_display *display, wl_surface *surface)
    {
        wl_display_ptr = display;
        queue          = wl_display_create_queue(display);
        registry       = wl_display_get_registry(display);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(registry), queue);
        static const wl_registry_listener listener{wayland_global, wayland_global_removed};
        wl_registry_add_listener(registry, &listener, this);
        if (wl_display_roundtrip_queue(display, queue) < 0 || manager == nullptr)
            return;
        appmenu = org_kde_kwin_appmenu_manager_create(manager, surface);
        org_kde_kwin_appmenu_set_address(appmenu, service.c_str(), object_path);
        wl_display_flush(display);
        wayland_associated = true;
        spdlog::info("Global menu exported to KDE Wayland: {} {}", service, object_path);
    }

    static void wayland_global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t)
    {
        auto *self = static_cast<Impl *>(data);
        if (std::strcmp(interface, "org_kde_kwin_appmenu_manager") != 0)
            return;
        self->manager = static_cast<org_kde_kwin_appmenu_manager *>(
            wl_registry_bind(registry, name, &org_kde_kwin_appmenu_manager_interface, 1));
        self->manager_name = name;
    }

    static void wayland_global_removed(void *data, wl_registry *, uint32_t name)
    {
        auto *self = static_cast<Impl *>(data);
        if (name == self->manager_name)
            self->wayland_associated = false;
    }

    void associate_x11(SDL_PropertiesID props)
    {
        x_display = static_cast<Display *>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
        x_window  = static_cast<uint32_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        if (x_display == nullptr || x_window == 0)
            return;
        set_x11_property("_KDE_NET_WM_APPMENU_SERVICE_NAME", service.c_str());
        set_x11_property("_KDE_NET_WM_APPMENU_OBJECT_PATH", object_path);
        XFlush(x_display);
        invoke([](Impl *self) { self->register_x11(); });
        spdlog::info("Global menu exported to X11: {} {}", service, object_path);
    }

    void set_x11_property(const char *name, const char *value)
    {
        XChangeProperty(x_display, x_window, XInternAtom(x_display, name, False), XInternAtom(x_display, "UTF8_STRING", False), 8,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(value), static_cast<int>(std::strlen(value)));
    }

    void disassociate_window()
    {
        if (appmenu != nullptr)
            org_kde_kwin_appmenu_release(appmenu);
        if (manager != nullptr)
            wl_proxy_destroy(reinterpret_cast<wl_proxy *>(manager));
        if (registry != nullptr)
            wl_registry_destroy(registry);
        if (wl_display_ptr != nullptr)
            wl_display_flush(wl_display_ptr);
        if (queue != nullptr)
            wl_event_queue_destroy(queue);
        if (x_display != nullptr && x_window != 0)
        {
            XDeleteProperty(x_display, x_window, XInternAtom(x_display, "_KDE_NET_WM_APPMENU_SERVICE_NAME", False));
            XDeleteProperty(x_display, x_window, XInternAtom(x_display, "_KDE_NET_WM_APPMENU_OBJECT_PATH", False));
            XFlush(x_display);
        }
        appmenu            = nullptr;
        manager            = nullptr;
        registry           = nullptr;
        queue              = nullptr;
        wl_display_ptr     = nullptr;
        wayland_associated = false;
        manager_name       = 0;
        x_display          = nullptr;
    }

    void invoke(std::function<void(Impl *)> callback)
    {
        struct Invocation
        {
            Impl *self;
            std::function<void(Impl *)> callback;
        };
        auto *source = g_idle_source_new();
        g_source_set_callback(
            source,
            [](gpointer data) -> gboolean
            {
                const auto *invocation = static_cast<Invocation *>(data);
                invocation->callback(invocation->self);
                return G_SOURCE_REMOVE;
            },
            new Invocation{this, std::move(callback)}, [](gpointer data) { delete static_cast<Invocation *>(data); });
        g_source_attach(source, context);
        g_source_unref(source);
    }

    SDL_Window *window;
    MenuItem menu;
    std::mutex mutex;
    std::vector<GlobalMenuAction> pending;
    std::string service;
    guint32 revision              = 1;
    Uint32 wakeup_event           = 0;
    std::atomic_bool stopping     = false;
    std::atomic_bool panel        = false;
    std::atomic_bool x_registered = false;
    std::atomic_uint32_t x_window = 0;
    std::promise<bool> ready;
    std::thread worker;
    GMainContext *context                 = nullptr;
    GMainLoop *loop                       = nullptr;
    GCancellable *cancellable             = nullptr;
    GDBusConnection *connection           = nullptr; // D-Bus worker only.
    wl_display *wl_display_ptr            = nullptr;
    wl_event_queue *queue                 = nullptr;
    wl_registry *registry                 = nullptr;
    org_kde_kwin_appmenu_manager *manager = nullptr;
    org_kde_kwin_appmenu *appmenu         = nullptr;
    uint32_t manager_name                 = 0;
    bool wayland_associated               = false;
    bool available                        = false;
    unsigned pending_registrations        = 0; // Worker-only async callbacks drained before teardown.
    Display *x_display                    = nullptr;
};

GlobalMenu::GlobalMenu(SDL_Window *window, const MenuItem &menu) :
    impl_(std::make_unique<Impl>(window, menu))
{
}
GlobalMenu::~GlobalMenu() = default;

bool GlobalMenu::active()
{
    if (impl_->wl_display_ptr != nullptr)
        wl_display_dispatch_queue_pending(impl_->wl_display_ptr, impl_->queue);
    return impl_->panel && (impl_->wayland_associated || impl_->x_registered);
}

void GlobalMenu::update(const MenuItem &menu)
{
    if (!impl_->available)
        return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->menu = menu;
        ++impl_->revision;
    }
    impl_->invoke(
        [](Impl *self)
        {
            if (self->connection == nullptr)
                return;
            std::lock_guard lock(self->mutex);
            g_dbus_connection_emit_signal(self->connection, nullptr, object_path, interface_name, "LayoutUpdated",
                                          g_variant_new("(ui)", self->revision, 0), nullptr);
        });
}

void GlobalMenu::rebind_window(SDL_Window *window)
{
    const auto old_xid = impl_->x_window.load();
    impl_->disassociate_window();
    impl_->x_window     = 0;
    impl_->x_registered = false;
    impl_->window       = window;
    if (!impl_->available)
        return;
    if (old_xid != 0)
        impl_->invoke(
            [old_xid](Impl *self)
            {
                if (self->connection)
                    g_dbus_connection_call(self->connection, registrar_name, registrar_path, registrar_name, "UnregisterWindow",
                                           g_variant_new("(u)", old_xid), nullptr, G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr,
                                           nullptr);
            });
    impl_->associate_window();
}

std::vector<GlobalMenuAction> GlobalMenu::take_actions()
{
    std::lock_guard lock(impl_->mutex);
    std::vector<GlobalMenuAction> result;
    result.swap(impl_->pending);
    return result;
}
} // namespace liltview
