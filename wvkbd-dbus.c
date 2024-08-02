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

// Function to find PID of wvkbd
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
                            if (strncmp(line, "wvkbd", 5) == 0) {
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

// Function to send SIGRTMIN to wvkbd
static void send_signal_to_wvkbd() {
    pid_t pid = find_wvkbd_pid();
    if (pid > 0) {
        kill(pid, SIGRTMIN);
        g_print("Sent SIGRTMIN to wvkbd (PID: %d)\n", pid);
    } else {
        g_print("wvkbd process not found\n");
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
    OSKData *data = (OSKData *)user_data;

    if (g_strcmp0(method_name, "SetVisible") == 0) {
        gboolean visible;
        g_variant_get(parameters, "(b)", &visible);
        
        if (data->visible != visible) {
            data->visible = visible;

            // Here you would implement the actual logic to show/hide the OSK
            g_print("Setting OSK visibility to: %s\n", visible ? "true" : "false");

            // Send signal to wvkbd
            send_signal_to_wvkbd();

            // Emit the PropertiesChanged signal
            GVariantBuilder *builder = g_variant_builder_new(G_VARIANT_TYPE_ARRAY);
            g_variant_builder_add(builder, "{sv}", "Visible", g_variant_new_boolean(visible));
            g_dbus_connection_emit_signal(connection,
                                          NULL,
                                          object_path,
                                          "org.freedesktop.DBus.Properties",
                                          "PropertiesChanged",
                                          g_variant_new("(sa{sv}as)",
                                                        interface_name,
                                                        builder,
                                                        NULL),
                                          NULL);
            g_variant_builder_unref(builder);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);
    } else {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method.");
    }
}

static GVariant* handle_get_property(GDBusConnection  *connection,
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

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    handle_get_property,
    NULL  // We don't need a setter as our property is read-only
};

static void on_bus_acquired(GDBusConnection *connection,
                            const gchar     *name,
                            gpointer         user_data) {
    OSKData *data = g_new0(OSKData, 1);
    data->connection = connection;
    data->visible = FALSE;  // Initially not visible

    GError *error = NULL;
    guint registration_id = g_dbus_connection_register_object(connection,
                                                             OSK_OBJECT_PATH,
                                                             (GDBusInterfaceInfo *)user_data,
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
        g_printerr("Error parsing introspection XML: %s\n", error->message);
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

    g_main_loop_run(loop);

    g_bus_unown_name(owner_id);
    g_dbus_node_info_unref(introspection_data);
    g_main_loop_unref(loop);

    return 0;
}
