#include "notify.h"
#include <glib.h>
#include <stdio.h>

void notify_init(void) {
    /* Ready */
}

void notify_message(const char *summary, const char *body) {
    if (!summary || !body) return;

    gchar *argv[] = {
        "notify-send",
        "-a", "AspectScale",
        "-i", "view-fullscreen",
        "-t", "3000",
        (gchar *)summary,
        (gchar *)body,
        NULL
    };

    GError *error = NULL;
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
    if (error) {
        g_error_free(error);
    }
}

void notify_window_action(const WindowScaleInfo *info, bool show_notifications) {
    if (!show_notifications || !info) return;

    char summary[128];
    char body[512];

    if (info->is_restored) {
        snprintf(summary, sizeof(summary), "Window Restored");
        snprintf(body, sizeof(body), "\"%s\"\nRestored to %d × %d",
                 info->title[0] ? info->title : "Active Window",
                 info->new_geom.width, info->new_geom.height);
    } else if (info->was_scaled) {
        snprintf(summary, sizeof(summary), "Window Scaled (Aspect Corrected)");
        snprintf(body, sizeof(body), "\"%s\"\n%d × %d ➔ %d × %d (AR %.2f:1)",
                 info->title[0] ? info->title : "Active Window",
                 info->old_geom.width, info->old_geom.height,
                 info->new_geom.width, info->new_geom.height,
                 info->aspect_ratio);
    } else {
        return;
    }

    notify_message(summary, body);
}
