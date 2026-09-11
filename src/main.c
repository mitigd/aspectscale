#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "x11_scale.h"
#include "gl_scaler.h"
#include "config.h"
#include "notify.h"

#define APP_NAME "AspectScale"
#define APP_VERSION "2.2.0"

typedef struct {
    Display *main_dpy;
    Display *hotkey_dpy;
    AppIndicator *indicator;
    GtkWidget *menu;
    GtkWidget *remembered_submenu;
    GtkWidget *remember_item;
    bool running;
    pthread_t hotkey_thread;
    KeyCode keycode_s;
    KeyCode keycode_r;
    Window last_active_win;
    Window dismissed_win;
} AppContext;

static AppContext g_app;

static int x11_error_handler(Display *dpy, XErrorEvent *ev) {
    (void)dpy;
    (void)ev;
    return 0;
}

static void rebuild_remembered_submenu(void);

static Window resolve_target_window(Display *dpy, Window target) {
    if (!dpy || target == None) return target;

    Window root = DefaultRootWindow(dpy);
    Window parent = None, *children = NULL;
    unsigned int nchildren = 0;

    XWindowAttributes wa;
    if (!XGetWindowAttributes(dpy, target, &wa)) return target;

    int scr_w = DisplayWidth(dpy, DefaultScreen(dpy));
    int scr_h = DisplayHeight(dpy, DefaultScreen(dpy));

    /* If target is already full screen size (e.g. Wine virtual desktop wrapper), look for transient game window */
    if (wa.width >= scr_w && wa.height >= scr_h) {
        if (XQueryTree(dpy, root, &root, &parent, &children, &nchildren) && children) {
            for (int i = (int)nchildren - 1; i >= 0; i--) {
                Window trans = None;
                if (XGetTransientForHint(dpy, children[i], &trans) && trans == target) {
                    XWindowAttributes cwa;
                    if (XGetWindowAttributes(dpy, children[i], &cwa) && cwa.map_state == IsViewable) {
                        Window actual = children[i];
                        fprintf(stderr, "AspectScale Debug: Resolved transient game window 0x%lx for wrapper 0x%lx\n",
                                (unsigned long)actual, (unsigned long)target);
                        XFree(children);
                        return actual;
                    }
                }
            }
            XFree(children);
        }
    }
    return target;
}

static void trigger_scale_window(Window target) {
    target = resolve_target_window(g_app.main_dpy, target);
    fprintf(stderr, "AspectScale Debug: trigger_scale_window called with target=0x%lx, gl_scaler_running=%d\n",
            (unsigned long)target, gl_scaler_is_running());
    if (gl_scaler_is_running()) {
        g_app.dismissed_win = gl_scaler_get_target();
        gl_scaler_stop();
        if (config_get_notifications()) {
            notify_message("AspectScale", "Exited Fullscreen Scaler.");
        }
        return;
    }

    if (target == None) {
        if (config_get_notifications()) {
            notify_message("AspectScale", "No active window detected to scale.");
        }
        return;
    }

    char title[256];
    x11_get_window_title(g_app.main_dpy, target, title, sizeof(title));

    gl_scaler_set_hide_cursor(config_get_hide_cursor());

    if (gl_scaler_start(g_app.main_dpy, target)) {
        g_app.dismissed_win = None;
        if (config_get_notifications()) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Scaled \"%s\" to Fullscreen.\nPress Ctrl+Alt+S or Esc to exit.", title);
            notify_message("AspectScale", msg);
        }
    } else {
        WindowScaleInfo info;
        if (x11_toggle_scale_window(g_app.main_dpy, target, &info)) {
            notify_window_action(&info, config_get_notifications());
        }
    }
}

static void trigger_scale_active(void) {
    Window active = x11_get_active_window(g_app.main_dpy);
    fprintf(stderr, "AspectScale Debug: trigger_scale_active() called from somewhere! active=0x%lx\n", (unsigned long)active);
    trigger_scale_window(active);
}

static void trigger_restore_active(void) {
    fprintf(stderr, "AspectScale Debug: trigger_restore_active() called\n");
    if (gl_scaler_is_running()) {
        g_app.dismissed_win = gl_scaler_get_target();
        gl_scaler_stop();
        if (config_get_notifications()) {
            notify_message("AspectScale", "Exited Fullscreen Scaler.");
        }
        return;
    }

    Window active = x11_get_active_window(g_app.main_dpy);
    if (active == None) return;

    WindowScaleInfo info;
    if (x11_restore_window(g_app.main_dpy, active, &info)) {
        notify_window_action(&info, config_get_notifications());
    }
}

static gboolean autoscale_poll_cb(gpointer user_data) {
    (void)user_data;
    if (gl_scaler_is_running()) return G_SOURCE_CONTINUE;

    Window active = x11_get_active_window(g_app.main_dpy);
    if (active == None) {
        g_app.last_active_win = None;
        return G_SOURCE_CONTINUE;
    }

    if (active != g_app.last_active_win) {
        g_app.last_active_win = active;

        // Reset dismissed window if switched away
        if (active != g_app.dismissed_win) {
            g_app.dismissed_win = None;
        }

        if (active != g_app.dismissed_win) {
            XClassHint ch = {NULL, NULL};
            char title[256] = {0};
            x11_get_window_title(g_app.main_dpy, active, title, sizeof(title));

            const char *cls = NULL;
            const char *nam = NULL;
            if (XGetClassHint(g_app.main_dpy, active, &ch)) {
                cls = ch.res_class;
                nam = ch.res_name;
            }

            if (config_is_autoscale(cls, nam, title)) {
                fprintf(stderr, "AspectScale Debug: autoscale triggered for win=0x%lx title='%s' class='%s'\n",
                        (unsigned long)active, title, cls ? cls : "");
                trigger_scale_window(active);
            }

            if (ch.res_name) {
                XFree(ch.res_name);
                ch.res_name = NULL;
            }
            if (ch.res_class) {
                XFree(ch.res_class);
                ch.res_class = NULL;
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_hotkey_scale_idle(gpointer user_data) {
    (void)user_data;
    fprintf(stderr, "AspectScale Debug: on_hotkey_scale_idle triggered by hotkey thread!\n");
    trigger_scale_active();
    return G_SOURCE_REMOVE;
}

static gboolean on_hotkey_restore_idle(gpointer user_data) {
    (void)user_data;
    trigger_restore_active();
    return G_SOURCE_REMOVE;
}

static void* hotkey_listener_thread(void *arg) {
    (void)arg;
    Display *dpy = g_app.hotkey_dpy;
    if (!dpy) return NULL;

    XEvent ev;
    while (g_app.running) {
        XNextEvent(dpy, &ev);

        if (ev.type == KeyPress) {
            XKeyEvent *kev = &ev.xkey;
            if (kev->keycode == g_app.keycode_s) {
                g_idle_add(on_hotkey_scale_idle, NULL);
            } else if (kev->keycode == g_app.keycode_r) {
                g_idle_add(on_hotkey_restore_idle, NULL);
            }
        }
    }
    return NULL;
}

static void on_scale_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    fprintf(stderr, "AspectScale Debug: on_scale_clicked triggered from tray menu!\n");
    trigger_scale_active();
}

static void on_restore_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    trigger_restore_active();
}

static void on_remember_active_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;

    Window active = gl_scaler_is_running() ? gl_scaler_get_target() : x11_get_active_window(g_app.main_dpy);
    if (active == None) {
        notify_message("AspectScale", "No active window to remember.");
        return;
    }

    XClassHint ch = {NULL, NULL};
    char title[256] = {0};
    x11_get_window_title(g_app.main_dpy, active, title, sizeof(title));

    const char *cls = NULL;
    const char *nam = NULL;
    if (XGetClassHint(g_app.main_dpy, active, &ch)) {
        cls = ch.res_class;
        nam = ch.res_name;
    }

    if (config_add_autoscale(cls, nam, title)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Remembered \"%s\"!\nWill automatically scale to fullscreen when launched.",
                 title[0] ? title : (cls ? cls : "Window"));
        notify_message("AspectScale", msg);
        rebuild_remembered_submenu();
    } else {
        notify_message("AspectScale", "This window is already in the auto-scale list.");
    }

    if (ch.res_name) {
        XFree(ch.res_name);
        ch.res_name = NULL;
    }
    if (ch.res_class) {
        XFree(ch.res_class);
        ch.res_class = NULL;
    }
}

static void on_remove_rule_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    char *id = (char *)user_data;
    if (id) {
        config_remove_autoscale(id);
        char msg[256];
        snprintf(msg, sizeof(msg), "Removed auto-scale rule for: %s", id);
        notify_message("AspectScale", msg);
        free(id);
        rebuild_remembered_submenu();
    }
}

static void on_clear_rules_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    config_clear_autoscale();
    notify_message("AspectScale", "Cleared all auto-scale rules.");
    rebuild_remembered_submenu();
}

static void on_hide_cursor_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    (void)user_data;
    bool hide = gtk_check_menu_item_get_active(item);
    config_set_hide_cursor(hide);
    gl_scaler_set_hide_cursor(hide);
}

static void on_notify_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    (void)user_data;
    bool enabled = gtk_check_menu_item_get_active(item);
    config_set_notifications(enabled);
}

static void rebuild_remembered_submenu(void) {
    if (!g_app.remembered_submenu) return;

    GList *children = gtk_container_get_children(GTK_CONTAINER(g_app.remembered_submenu));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    const AppConfig *cfg = config_get();
    if (cfg->rule_count == 0) {
        GtkWidget *empty_item = gtk_menu_item_new_with_label("(No apps remembered yet)");
        gtk_widget_set_sensitive(empty_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_app.remembered_submenu), empty_item);
    } else {
        for (int i = 0; i < cfg->rule_count; i++) {
            char label[256];
            snprintf(label, sizeof(label), "Remove: %s", cfg->rules[i].label);
            GtkWidget *rule_item = gtk_menu_item_new_with_label(label);
            g_signal_connect(rule_item, "activate", G_CALLBACK(on_remove_rule_clicked), strdup(cfg->rules[i].identifier));
            gtk_menu_shell_append(GTK_MENU_SHELL(g_app.remembered_submenu), rule_item);
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(g_app.remembered_submenu), gtk_separator_menu_item_new());

        GtkWidget *clear_item = gtk_menu_item_new_with_label("Clear All Remembered Apps");
        g_signal_connect(clear_item, "activate", G_CALLBACK(on_clear_rules_clicked), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(g_app.remembered_submenu), clear_item);
    }
    gtk_widget_show_all(g_app.remembered_submenu);
}

static void on_about_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;

    GtkWidget *dialog = gtk_message_dialog_new(
        NULL,
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "%s v%s\n\n"
        "Lossless Scaling / Aspect-Corrected Fullscreen for Linux\n\n"
        "• Global Shortcut: Ctrl + Alt + S\n"
        "  Scales active window to fullscreen with hardware OpenGL,\n"
        "  preserving aspect ratio with pure black borders and covering panels.\n"
        "  Pressing again or Esc exits fullscreen.\n\n"
        "• Auto-Scale on Launch:\n"
        "  Click 'Remember Active Window for Auto-Scale' in the tray.\n"
        "  When that app is opened, it automatically snaps to fullscreen!\n\n"
        "• Cursor Hiding:\n"
        "  Cursor is hidden automatically in fullscreen (toggleable in tray).\n\n"
        "Zero-copy GPU texturing via GLX_EXT_texture_from_pixmap.",
        APP_NAME, APP_VERSION
    );
    gtk_window_set_title(GTK_WINDOW(dialog), "About AspectScale");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_quit_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    g_app.running = false;
    gtk_main_quit();
}

static void build_tray_menu(void) {
    GtkWidget *menu = gtk_menu_new();

    /* Header */
    GtkWidget *header_item = gtk_menu_item_new_with_label("AspectScale (Lossless Fullscreen)");
    gtk_widget_set_sensitive(header_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), header_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* Actions */
    GtkWidget *scale_item = gtk_menu_item_new_with_label("Scale to Fullscreen          [Ctrl+Alt+S]");
    g_signal_connect(scale_item, "activate", G_CALLBACK(on_scale_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), scale_item);

    GtkWidget *restore_item = gtk_menu_item_new_with_label("Restore Windowed             [Ctrl+Alt+R]");
    g_signal_connect(restore_item, "activate", G_CALLBACK(on_restore_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), restore_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* Auto-scale remember */
    GtkWidget *remember_item = gtk_menu_item_new_with_label("Remember Active Window for Auto-Scale");
    g_signal_connect(remember_item, "activate", G_CALLBACK(on_remember_active_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), remember_item);
    g_app.remember_item = remember_item;

    GtkWidget *remembered_parent = gtk_menu_item_new_with_label("Remembered Apps (Auto-Scale)");
    GtkWidget *remembered_submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(remembered_parent), remembered_submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), remembered_parent);
    g_app.remembered_submenu = remembered_submenu;
    rebuild_remembered_submenu();

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* Hide Cursor Checkbox */
    GtkWidget *cursor_item = gtk_check_menu_item_new_with_label("Hide Cursor in Fullscreen");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(cursor_item), config_get_hide_cursor());
    g_signal_connect(cursor_item, "toggled", G_CALLBACK(on_hide_cursor_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), cursor_item);

    /* Notification checkbox */
    GtkWidget *notify_item = gtk_check_menu_item_new_with_label("Show Desktop Notifications");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(notify_item), config_get_notifications());
    g_signal_connect(notify_item, "toggled", G_CALLBACK(on_notify_toggled), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), notify_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* About & Quit */
    GtkWidget *about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(about_item, "activate", G_CALLBACK(on_about_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), about_item);

    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_quit_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    g_app.menu = menu;
}

static void print_usage(const char *prog) {
    printf("AspectScale v%s - Lossless Aspect-Corrected Scaler for Linux\n\n"
           "Usage: %s [OPTIONS]\n\n"
           "Options:\n"
           "  -h, --help            Show this help message\n"
           "  -v, --version         Show version\n"
           "  --scale-active        Scale currently active window immediately to fullscreen\n"
           "  --restore-active      Restore currently active window immediately\n"
           "  --remember-active     Add active window to auto-scale list\n"
           "  --no-hide-cursor      Do not hide cursor in fullscreen\n"
           "  --no-notify           Disable desktop notifications\n"
           "  --no-tray             Run as daemon without system tray icon\n\n"
           "Hotkeys:\n"
           "  Ctrl + Alt + S        Scale active window to fullscreen (toggle restores)\n"
           "  Ctrl + Alt + R        Restore active window\n"
           "  Escape                Exit fullscreen scaling\n",
           APP_VERSION, prog);
}

int main(int argc, char *argv[]) {
    XInitThreads();
    XSetErrorHandler(x11_error_handler);
    config_init();

    g_app.running = true;
    g_app.last_active_win = None;
    g_app.dismissed_win = None;

    bool one_shot_scale = false;
    bool one_shot_restore = false;
    bool one_shot_remember = false;
    bool enable_tray = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", APP_NAME, APP_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--scale-active") == 0) {
            one_shot_scale = true;
        } else if (strcmp(argv[i], "--restore-active") == 0) {
            one_shot_restore = true;
        } else if (strcmp(argv[i], "--remember-active") == 0) {
            one_shot_remember = true;
        } else if (strcmp(argv[i], "--no-hide-cursor") == 0) {
            config_set_hide_cursor(false);
        } else if (strcmp(argv[i], "--no-notify") == 0) {
            config_set_notifications(false);
        } else if (strcmp(argv[i], "--no-tray") == 0) {
            enable_tray = false;
        }
    }

    g_app.main_dpy = XOpenDisplay(NULL);
    if (!g_app.main_dpy) {
        fprintf(stderr, "Error: Could not open X11 display. Are you running an X11 session?\n");
        return 1;
    }

    x11_scale_init(g_app.main_dpy);
    gl_scaler_init(g_app.main_dpy);
    gl_scaler_set_hide_cursor(config_get_hide_cursor());

    /* Handle one-shot actions */
    if (one_shot_scale) {
        trigger_scale_active();
        XCloseDisplay(g_app.main_dpy);
        return 0;
    }
    if (one_shot_restore) {
        trigger_restore_active();
        XCloseDisplay(g_app.main_dpy);
        return 0;
    }
    if (one_shot_remember) {
        on_remember_active_clicked(NULL, NULL);
        XCloseDisplay(g_app.main_dpy);
        return 0;
    }

    /* Initialize GTK */
    gtk_init(&argc, &argv);

    /* Setup autoscale periodic poll timer (every 200ms) */
    g_timeout_add(200, autoscale_poll_cb, NULL);

    /* Open dedicated display connection for global hotkey grabbing and thread */
    g_app.hotkey_dpy = XOpenDisplay(NULL);
    if (!g_app.hotkey_dpy) {
        fprintf(stderr, "Error: Could not open X11 hotkey display connection.\n");
        XCloseDisplay(g_app.main_dpy);
        return 1;
    }

    KeySym sym_s = XK_s;
    KeySym sym_r = XK_r;
    g_app.keycode_s = XKeysymToKeycode(g_app.hotkey_dpy, sym_s);
    g_app.keycode_r = XKeysymToKeycode(g_app.hotkey_dpy, sym_r);

    if (g_app.keycode_s != 0) {
        x11_grab_key(g_app.hotkey_dpy, g_app.keycode_s, ControlMask | Mod1Mask);
    }

    if (g_app.keycode_r != 0) {
        x11_grab_key(g_app.hotkey_dpy, g_app.keycode_r, ControlMask | Mod1Mask);
    }

    /* Start hotkey listener thread */
    if (pthread_create(&g_app.hotkey_thread, NULL, hotkey_listener_thread, NULL) != 0) {
        fprintf(stderr, "Error: Failed to create hotkey listener thread.\n");
        XCloseDisplay(g_app.hotkey_dpy);
        XCloseDisplay(g_app.main_dpy);
        return 1;
    }

    /* Setup System Tray Indicator */
    if (enable_tray) {
        build_tray_menu();
        g_app.indicator = app_indicator_new(
            "aspectscale",
            "view-fullscreen",
            APP_INDICATOR_CATEGORY_APPLICATION_STATUS
        );
        app_indicator_set_status(g_app.indicator, APP_INDICATOR_STATUS_ACTIVE);
        app_indicator_set_title(g_app.indicator, "AspectScale");
        app_indicator_set_menu(g_app.indicator, GTK_MENU(g_app.menu));
    }

    if (config_get_notifications()) {
        notify_message("AspectScale", "Ready. Press Ctrl+Alt+S on any window to scale.");
    }

    /* Run GTK Main Loop */
    gtk_main();

    /* Cleanup */
    g_app.running = false;
    if (g_app.keycode_s != 0) {
        x11_ungrab_key(g_app.hotkey_dpy, g_app.keycode_s, ControlMask | Mod1Mask);
    }
    if (g_app.keycode_r != 0) {
        x11_ungrab_key(g_app.hotkey_dpy, g_app.keycode_r, ControlMask | Mod1Mask);
    }

    pthread_cancel(g_app.hotkey_thread);
    pthread_join(g_app.hotkey_thread, NULL);

    gl_scaler_cleanup();
    x11_scale_cleanup(g_app.main_dpy);
    XCloseDisplay(g_app.hotkey_dpy);
    XCloseDisplay(g_app.main_dpy);

    return 0;
}
