#include <gio/gio.h>
#include <glib.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "proto/input-method-unstable-v2-client-protocol.h"
#include "proto/phoc-device-state-unstable-v1-client-protocol.h"

static struct zwp_input_method_v2 *input_method = NULL;
static struct wl_seat *seat = NULL;
static struct zwp_input_method_manager_v2 *input_method_manager = NULL;
static struct zwp_input_method_v2_listener input_method_listener;
static gboolean input_method_active = TRUE;

static struct zphoc_device_state_v1 *device_state = NULL;
static gboolean hwkbd = FALSE;

#define OSK_INTERFACE "sm.puri.OSK0"
#define OSK_OBJECT_PATH "/sm/puri/OSK0"

static guint visibility_timeout_id = 0;
static gboolean pending_visibility = FALSE;

static GSettings *a11y = NULL;
static gboolean screen_keyboard_enabled = FALSE;

typedef struct {
    GDBusConnection *connection;
    gboolean visible;
} OSKData;

static void set_visible(OSKData *data, gboolean visible);
static GVariant* get_property(GDBusConnection *connection,
                              const gchar *sender,
                              const gchar *object_path,
                              const gchar *interface_name,
                              const gchar *property_name,
                              GError **error,
                              gpointer user_data);
static gboolean set_property(GDBusConnection *connection,
                             const gchar *sender,
                             const gchar *object_path,
                             const gchar *interface_name,
                             const gchar *property_name,
                             GVariant *value,
                             GError **error,
                             gpointer user_data);

static gboolean enable_logging = FALSE;

#define log(fmt, ...) \
    do { if (enable_logging) g_print(fmt, ##__VA_ARGS__); } while (0)

static pid_t find_wvkbd_pid() {
    DIR *dir;
    struct dirent *ent;
    char path[PATH_MAX];
    char line[256];
    pid_t pid = -1;

    if ((dir = opendir("/proc")) != NULL) {
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_type == DT_DIR) {
                char *endptr;
                long lpid = strtol(ent->d_name, &endptr, 10);
                if (*endptr == '\0') {
                    snprintf(path, sizeof(path), "/proc/%ld/comm", lpid);
                    FILE *fp = fopen(path, "r");
                    if (fp) {
                        if (fgets(line, sizeof(line), fp) != NULL) {
                            if (strncmp(line, "wvkbd-mobintl", 13) == 0) {
                                pid = (pid_t)lpid;
                                fclose(fp);
                                break;
                            }
                        }
                        fclose(fp);
                    }
                }
            }
        }
        closedir(dir);
    }
    return pid;
}

static void send_signal_to_wvkbd(gboolean visible) {
    pid_t pid = find_wvkbd_pid();
    if (pid > 0) {
        usleep(100000); //  let me breathe
        int sig = visible ? SIGUSR2 : SIGUSR1;
        kill(pid, sig);
        log("Sent %s to wvkbd-mobintl (PID: %d)\n", visible ? "SIGUSR2" : "SIGUSR1", pid);
    } else {
        log("wvkbd-mobintl process not found\n");
    }
}

static void handle_method_call(GDBusConnection       *connection,
                               const gchar           *sender,
                               const gchar           *object_path,
                               const gchar           *interface_name,
                               const gchar           *method_name,
                               GVariant              *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer               user_data) {
    log("Received method call: %s\n", method_name);
    OSKData *data = (OSKData *)user_data;

    if (g_strcmp0(method_name, "SetVisible") == 0) {
        gboolean visible;
        g_variant_get(parameters, "(b)", &visible);
        set_visible(data, visible);
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method.");
    }
}

static GVariant* get_property(GDBusConnection  *connection,
                              const gchar      *sender,
                              const gchar      *object_path,
                              const gchar      *interface_name,
                              const gchar      *property_name,
                              GError          **error,
                              gpointer          user_data) {
    OSKData *data = (OSKData *)user_data;

    if (g_strcmp0(property_name, "Visible") == 0) {
        return g_variant_new_boolean(data->visible);
    }

    return NULL;
}

static gboolean set_property(GDBusConnection  *connection,
                             const gchar      *sender,
                             const gchar      *object_path,
                             const gchar      *interface_name,
                             const gchar      *property_name,
                             GVariant         *value,
                             GError          **error,
                             gpointer          user_data) {
    OSKData *data = (OSKData *)user_data;

    if (g_strcmp0(property_name, "Visible") == 0) {
        gboolean visible;
        g_variant_get(value, "b", &visible);
        set_visible(data, visible);
        return TRUE;
    }

    return FALSE;
}

static gboolean apply_visibility_change(gpointer user_data) {
    OSKData *data = (OSKData *)user_data;
    gboolean visible = pending_visibility;
    
    visibility_timeout_id = 0;
    
    if (data->visible != visible) {
        data->visible = visible;
        send_signal_to_wvkbd(visible);

        GVariantBuilder *builder;
        GError *error = NULL;

        builder = g_variant_builder_new(G_VARIANT_TYPE_ARRAY);
        g_variant_builder_add(builder, "{sv}", "Visible",
                              g_variant_new_boolean(visible));

        g_dbus_connection_emit_signal(data->connection,
                                      NULL,
                                      OSK_OBJECT_PATH,
                                      "org.freedesktop.DBus.Properties",
                                      "PropertiesChanged",
                                      g_variant_new("(sa{sv}as)",
                                                    OSK_INTERFACE,
                                                    builder,
                                                    NULL),
                                      &error);

        if (error != NULL) {
            g_warning("Failed to emit PropertiesChanged: %s", error->message);
            g_error_free(error);
        }

        g_variant_builder_unref(builder);
        log("Emitted PropertiesChanged signal for Visible property\n");
    } else {
        log("Visibility unchanged, not emitting signal\n");
    }
    
    return G_SOURCE_REMOVE;
}

static void set_visible(OSKData *data, gboolean visible) {
    log("set_visible called with value: %d\n", visible);
    
    pending_visibility = visible;
    
    if (visibility_timeout_id != 0) {
        g_source_remove(visibility_timeout_id);
    }
    
    visibility_timeout_id = g_timeout_add(110, apply_visibility_change, data);
}

static GDBusMethodInfo set_visible_method = {
    -1, "SetVisible",
    (GDBusArgInfo *[]) {
        &(GDBusArgInfo) { -1, "visible", "b", NULL },
        NULL
    },
    NULL,
    NULL
};

static GDBusMethodInfo *methods[] = {&set_visible_method, NULL};
static GDBusSignalInfo *signals[] = {NULL};

static GDBusPropertyInfo visible_property = {
    -1, "Visible", "b", G_DBUS_PROPERTY_INFO_FLAGS_READABLE, NULL
};

static GDBusPropertyInfo *properties[] = {&visible_property, NULL};

static GDBusInterfaceInfo interface_info = {
    -1, OSK_INTERFACE,
    methods,
    signals,
    properties,
    NULL
};

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    get_property,
    set_property
};

static void initialize_osk_state(OSKData *data) {
    data ->visible = TRUE;
    set_visible(data, FALSE);
}

static void on_bus_acquired(GDBusConnection *connection,
                            const gchar     *name,
                            gpointer         user_data) {
    log("Acquired the name %s on the session bus\n", name);
    OSKData *data = g_new0(OSKData, 1);
    data->connection = connection;
    data->visible = FALSE;  // Initially not visible

    GError *error = NULL;
    guint registration_id = g_dbus_connection_register_object(connection,
                                                             OSK_OBJECT_PATH,
                                                             &interface_info,
                                                             &interface_vtable,
                                                             data,
                                                             g_free,
                                                             &error);

    if (registration_id == 0) {
        log("Error registering object: %s\n", error->message);
        g_error_free(error);
    } else {
        initialize_osk_state(data);
    }
}

static void toggle_input_method_listener();

static void handle_capabilities(void *data, struct zphoc_device_state_v1 *zphoc_device_state_v1, uint32_t capabilities) {
    gboolean new_keyboard_state = (capabilities & ZPHOC_DEVICE_STATE_V1_CAPABILITY_KEYBOARD) != 0;

    if (new_keyboard_state != hwkbd) {
        hwkbd = new_keyboard_state;
        log("Hardware kbd %s\n", hwkbd ? "connected" : "disconnected");
        toggle_input_method_listener();
    }
}

static const struct zphoc_device_state_v1_listener device_state_listener = {
    .capabilities = handle_capabilities,
};

static void global_registry_handler(void *data, struct wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
    log("Interface added: %s (id: %d, version: %d)\n", interface, id, version);
    if (strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0) {
        input_method_manager = wl_registry_bind(registry, id, &zwp_input_method_manager_v2_interface, version);
        log("Bound input method manager interface.\n");
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        seat = wl_registry_bind(registry, id, &wl_seat_interface, version);
        log("Bound wl_seat interface.\n");
    } else if (strcmp(interface, zphoc_device_state_v1_interface.name) == 0) {
        device_state = wl_registry_bind(registry, id, &zphoc_device_state_v1_interface, version);
        zphoc_device_state_v1_add_listener(device_state, &device_state_listener, NULL);
       log("Bound phoc_device_state interface\n"); 
    }
}

static void global_registry_remove_handler(void *data, struct wl_registry *registry, uint32_t id) {
    log("Global removed: id: %d\n", id);
}

static void set_osk_visibility(gboolean visible) {
    GDBusConnection *connection;
    GError *error = NULL;

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (error != NULL) {
        log("Failed to get session bus: %s\n", error->message);
        g_error_free(error);
        return;
    }

    GVariant *result = g_dbus_connection_call_sync(
        connection,
        OSK_INTERFACE,  // destination bus name
        OSK_OBJECT_PATH, // object path
        OSK_INTERFACE,  // interface name
        "SetVisible",   // method name
        g_variant_new("(b)", visible), // parameters
        NULL,           // expected reply type
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &error
    );

    if (error != NULL) {
        log("Failed to call SetVisible method: %s\n", error->message);
        g_error_free(error);
    } else {
        g_variant_unref(result);
    }

    g_object_unref(connection);
}

static void handle_activate(void *data, struct zwp_input_method_v2 *input_method) {
    log("User clicked in text input area.\n");
    set_osk_visibility(TRUE);
}

static void handle_deactivate(void *data, struct zwp_input_method_v2 *input_method) {
    log("User left text input area.\n");
    set_osk_visibility(FALSE);
}

static void handle_unavailable(void *data, struct zwp_input_method_v2 *input_method) {
    log("Input method unavailable.\n");
}

static void handle_surrounding_text(void *data, struct zwp_input_method_v2 *input_method, const char *text, uint32_t cursor, uint32_t anchor) {
    log("Surrounding text: %s, cursor: %u, anchor: %u\n", text, cursor, anchor);
}

static void handle_text_change_cause(void *data, struct zwp_input_method_v2 *input_method, uint32_t cause) {
    log("Text change cause: %u\n", cause);
}

static void handle_content_type(void *data, struct zwp_input_method_v2 *input_method, uint32_t hint, uint32_t purpose) {
    log("Content type: hint: %u, purpose: %u\n", hint, purpose);
}

static void handle_done(void *data, struct zwp_input_method_v2 *input_method) {
    log("Input method done event.\n");
}

static void init_input_method_listener() {
    input_method_listener = (struct zwp_input_method_v2_listener) {
      .activate = handle_activate,
      .deactivate = handle_deactivate,
      .unavailable = handle_unavailable,
      .surrounding_text = handle_surrounding_text,
      .text_change_cause = handle_text_change_cause,
      .content_type = handle_content_type,
      .done = handle_done,
    };
}

static void toggle_input_method_listener() {
    gboolean should_be_active = screen_keyboard_enabled && !hwkbd;
    
    if (should_be_active != input_method_active) {
        if (should_be_active) {
            if (!input_method && input_method_manager && seat) {
                input_method = zwp_input_method_manager_v2_get_input_method(input_method_manager, seat);
                if (input_method) {
                    zwp_input_method_v2_add_listener(input_method, &input_method_listener, NULL);
                    input_method_active = TRUE;
                    log("Input method listener started\n");
                } else {
                    log("Failed to create Input method object\n");
                }
            } else {
                log("manager or seat unavailable\n");
            }
        } else {
            if (input_method) {
                zwp_input_method_v2_destroy(input_method);
                input_method = NULL;
            }
            input_method_active = FALSE;
            set_osk_visibility(FALSE);
            log("Input method listener paused\n");
        }
    }
}

static void on_screen_keyboard_enabled_changed(GSettings *settings, gchar *key, gpointer user_data) {
  screen_keyboard_enabled = g_settings_get_boolean(settings, "screen-keyboard-enabled");
  log("Screen kbd enabled: %s\n", screen_keyboard_enabled ? "true" : "false");
  toggle_input_method_listener();
}

void *run_main_loop(void *arg) {
    GMainLoop *loop = (GMainLoop *)arg;
    g_main_loop_run(loop);
    return NULL;
}

void *dispatch_wayland(void *arg) {
    struct wl_display *display = (struct wl_display *)arg;
    while (wl_display_dispatch(display) != -1) {
    }
    return NULL;
}

int main(void) {
    GMainLoop *loop;
    guint owner_id;
    pthread_t loop_thread, wayland_thread;

    loop = g_main_loop_new(NULL, FALSE);

    GError *error = NULL;
    GDBusNodeInfo *introspection_data = g_dbus_node_info_new_for_xml(
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
        "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
        "<node>\n"
        "  <interface name=\"sm.puri.OSK0\">\n"
        "    <method name=\"SetVisible\">\n"
        "      <arg type=\"b\" direction=\"in\" name=\"visible\"/>\n"
        "    </method>\n"
        "    <property name=\"Visible\" type=\"b\" access=\"read\">\n"
        "    </property>\n"
        "  </interface>\n"
        "</node>\n",
        &error);

    if (introspection_data == NULL) {
        log("Error parsing introspection XML: %s\n", error->message);
        g_error_free(error);
        return 1;
    }

    owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                              OSK_INTERFACE,
                              G_BUS_NAME_OWNER_FLAGS_NONE,
                              on_bus_acquired,
                              NULL,
                              NULL,
                              introspection_data->interfaces[0],
                              NULL);

    struct wl_display *display;
    struct wl_registry *registry;

    display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "Failed to connect to Wayland display\n");
        return EXIT_FAILURE;
    }
    log("Connected to Wayland display.\n");

    registry = wl_display_get_registry(display);
    if (!registry) {
        fprintf(stderr, "Failed to get Wayland registry\n");
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }
    log("Got Wayland registry.\n");

    wl_registry_add_listener(registry, &(struct wl_registry_listener){
        .global = global_registry_handler,
        .global_remove = global_registry_remove_handler
    }, NULL);

    wl_display_roundtrip(display);

    if (!input_method_manager || !seat || !device_state) {
        fprintf(stderr, "Input method manager or seat or phosh_device_state not available\n");
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }

    input_method = zwp_input_method_manager_v2_get_input_method(input_method_manager, seat);
    if (!input_method) {
        fprintf(stderr, "Failed to create input method object\n");
        wl_display_disconnect(display);
        return EXIT_FAILURE;
    }

    a11y = g_settings_new("org.gnome.desktop.a11y.applications");
    screen_keyboard_enabled = g_settings_get_boolean(a11y, "screen-keyboard-enabled");
    log("INitial : %s\n", screen_keyboard_enabled ? "true" : "false");
    g_signal_connect(a11y, "changed::screen-keyboard-enabled", G_CALLBACK(on_screen_keyboard_enabled_changed), NULL);

    init_input_method_listener();
    zwp_input_method_v2_add_listener(input_method, &input_method_listener, NULL);
    toggle_input_method_listener();

    pthread_create(&loop_thread, NULL, run_main_loop, loop);

    pthread_create(&wayland_thread, NULL, dispatch_wayland, display);

    pthread_join(wayland_thread, NULL);

    g_main_loop_quit(loop);

    pthread_join(loop_thread, NULL);

    g_bus_unown_name(owner_id);
    g_dbus_node_info_unref(introspection_data);
    g_main_loop_unref(loop);
    g_object_unref(a11y);

    wl_display_disconnect(display);
    log("Disconnected from Wayland display.\n");

    return EXIT_SUCCESS;
}
