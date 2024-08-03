#include <gio/gio.h>
#include <glib.h>
#include <signal.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>

#define OSK_INTERFACE "sm.puri.OSK0"
#define OSK_OBJECT_PATH "/sm/puri/OSK0"

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

// Function to find PID of wvkbd
static pid_t find_wvkbd_pid() {
    DIR *dir;
    struct dirent *ent;
    char path[PATH_MAX];
    char line[256];
    pid_t pids[10]; // Array to store PIDs (adjust size as needed)
    int count = 0;
    pid_t min_pid = -1;
    
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
                                if (count < sizeof(pids)/sizeof(pids[0])) {
                                    pids[count++] = (pid_t)lpid;
                                }
                            }
                        }
                        fclose(fp);
                    }
                }
            }
        }
        closedir(dir);
    }

    if (count > 1) {
        // Find the smallest PID
        min_pid = pids[0];
        for (int i = 1; i < count; i++) {
            if (pids[i] < min_pid) {
                min_pid = pids[i];
            }
        }

        // Kill the smallest PID
        kill(min_pid, SIGKILL);

        // Find the remaining PID to send the signal
        for (int i = 0; i < count; i++) {
            if (pids[i] != min_pid) {
                return pids[i]; // Return the remaining PID
            }
        }
    } else if (count == 1) {
        return pids[0]; // Return the single PID found
    }

    return -1; // No suitable PID found
}

// Function to send SIGRTMIN to wvkbd
static void send_signal_to_wvkbd(gboolean visible) {
    pid_t pid = find_wvkbd_pid();
    if (pid > 0) {
        int sig = visible ? SIGUSR2 : SIGUSR1;
        kill(pid, sig);
        g_print("Sent %s to wvkbd-mobintl (PID: %d)\n", visible ? "SIGUSR2" : "SIGUSR1", pid);
    } else {
        g_print("wvkbd-mobintl process not found\n");
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
    g_print("Received method call: %s\n", method_name);
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

static void set_visible(OSKData *data, gboolean visible) {
    g_print("set_visible called with value: %d\n", visible);
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
        g_print("Emitted PropertiesChanged signal for Visible property\n");
    } else {
        g_print("Visibility unchanged, not emitting signal\n");
    }
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

static void on_bus_acquired(GDBusConnection *connection,
                            const gchar     *name,
                            gpointer         user_data) {
    g_print("Acquired the name %s on the session bus\n", name);
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
        g_printerr("Error registering object: %s\n", error->message);
        g_error_free(error);
    }
}

int main(void) {
    GMainLoop *loop;
    guint owner_id;

    loop = g_main_loop_new(NULL, FALSE);

    owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                              OSK_INTERFACE,
                              G_BUS_NAME_OWNER_FLAGS_NONE,
                              on_bus_acquired,
                              NULL,
                              NULL,
                              NULL,
                              NULL);

    g_print("Entering main loop...\n");
    g_main_loop_run(loop);

    g_print("Exiting...\n");

    g_bus_unown_name(owner_id);
    g_main_loop_unref(loop);

    return 0;
}
