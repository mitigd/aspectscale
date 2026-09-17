#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "x11_scale.h"
#include "gl_scaler.h"
#include "config.h"
#include "notify.h"

#define APP_NAME "AspectScale"
#define APP_VERSION "2.3.0"

typedef struct {
    Display *main_dpy;
    Display *hotkey_dpy;
    AppIndicator *indicator;
    GtkWidget *menu;
    GtkWidget *remembered_submenu;
    GtkWidget *remember_item;
    GtkWidget *scale_mode_parent;
    bool running;
    pthread_t hotkey_thread;
    KeyCode keycode_f;
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
static void trigger_fullscreen_target(Window target);
static void trigger_fullscreen_active(void);
static void trigger_window_scale_target(Window target, int factor);
static void trigger_window_scale_cycle(void);
static void trigger_restore_active(void);

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

static void trigger_fullscreen_target(Window target) {
    target = resolve_target_window(g_app.main_dpy, target);
    fprintf(stderr, "AspectScale Debug: trigger_fullscreen_target called with target=0x%lx, gl_scaler_running=%d\n",
            (unsigned long)target, gl_scaler_is_running());

    if (gl_scaler_is_running()) {
        if (gl_scaler_get_mode() == SCALER_MODE_FULLSCREEN) {
            g_app.dismissed_win = gl_scaler_get_target();
            gl_scaler_stop();
            if (config_get_notifications()) {
                notify_message("AspectScale", "Exited Fullscreen Scaler.");
            }
            return;
        } else {
            /* Switch from windowed mode to fullscreen on current target */
            Window current_target = gl_scaler_get_target();
            gl_scaler_stop();
            target = current_target;
        }
    }

    if (target == None) {
        if (config_get_notifications()) {
            notify_message("AspectScale", "No active window detected to scale.");
        }
        return;
    }

    char title[256] = {0};
    x11_get_window_title(g_app.main_dpy, target, title, sizeof(title));

    gl_scaler_set_hide_cursor(config_get_hide_cursor());
    gl_scaler_set_scale_mode(config_get_scale_mode());

    if (gl_scaler_start_fullscreen(g_app.main_dpy, target)) {
        g_app.dismissed_win = None;
        if (config_get_notifications()) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Scaled \"%s\" to Fullscreen.\nPress Ctrl+Alt+F or Esc to exit.",
                     title[0] ? title : "Window");
            notify_message("AspectScale", msg);
        }
    } else {
        WindowScaleInfo info;
        if (x11_toggle_scale_window(g_app.main_dpy, target, &info)) {
            notify_window_action(&info, config_get_notifications());
        }
    }
}

static void trigger_fullscreen_active(void) {
    Window target = gl_scaler_is_running() ? gl_scaler_get_target() : x11_get_active_window(g_app.main_dpy);
    fprintf(stderr, "AspectScale Debug: trigger_fullscreen_active() called, target=0x%lx\n", (unsigned long)target);
    trigger_fullscreen_target(target);
}

static void trigger_window_scale_target(Window target, int factor) {
    target = resolve_target_window(g_app.main_dpy, target);
    if (target == None) {
        if (config_get_notifications()) {
            notify_message("AspectScale", "No active window detected to scale.");
        }
        return;
    }

    int max_factor = gl_scaler_get_max_factor(g_app.main_dpy, target);
    if (factor == 999) {
        factor = max_factor;
    }

    if (max_factor < 2) {
        if (config_get_notifications()) {
            Window r_ret; int tx, ty; unsigned int tw = 0, th = 0, tb, td;
            XGetGeometry(g_app.main_dpy, target, &r_ret, &tx, &ty, &tw, &th, &tb, &td);
            char msg[256];
            snprintf(msg, sizeof(msg), "Window (%ux%u) is too large to scale 2x without exceeding screen resolution.", tw, th);
            notify_message("AspectScale", msg);
        }
        return;
    }

    if (factor > max_factor) {
        if (config_get_notifications()) {
            char msg[256];
            snprintf(msg, sizeof(msg), "Scale %dx would exceed screen resolution. Max fit is %dx.", factor, max_factor);
            notify_message("AspectScale", msg);
        }
        return;
    }

    if (factor < 2) {
        /* Factor 1: restore / exit windowed scaler */
        if (gl_scaler_is_running()) {
            g_app.dismissed_win = gl_scaler_get_target();
            gl_scaler_stop();
            if (config_get_notifications()) {
                notify_message("AspectScale", "Restored window to original size (1x).");
            }
        }
        return;
    }

    char title[256] = {0};
    x11_get_window_title(g_app.main_dpy, target, title, sizeof(title));

    gl_scaler_set_hide_cursor(config_get_hide_cursor());
    gl_scaler_set_scale_mode(config_get_scale_mode());

    if (gl_scaler_is_running() && gl_scaler_get_mode() == SCALER_MODE_WINDOWED) {
        gl_scaler_set_factor(factor);
        if (config_get_notifications()) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Window scaled to %dx.\nPress Ctrl+Alt+S to cycle, or Esc to restore.", factor);
            notify_message("AspectScale", msg);
        }
        return;
    }

    if (gl_scaler_is_running()) {
        gl_scaler_stop();
    }

    if (gl_scaler_start_windowed(g_app.main_dpy, target, factor)) {
        g_app.dismissed_win = None;
        if (config_get_notifications()) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Scaled \"%s\" to %dx windowed.\nPress Ctrl+Alt+S to cycle, Ctrl+Alt+F for fullscreen, or Esc to restore.",
                     title[0] ? title : "Window", factor);
            notify_message("AspectScale", msg);
        }
    }
}

static void trigger_window_scale_cycle(void) {
    if (gl_scaler_is_running()) {
        if (gl_scaler_get_mode() == SCALER_MODE_FULLSCREEN) {
            /* Switch from fullscreen to windowed mode at 2x */
            Window target = gl_scaler_get_target();
            gl_scaler_stop();
            trigger_window_scale_target(target, 2);
            return;
        } else {
            /* Already running in windowed mode: cycle to next integer factor */
            Window target = gl_scaler_get_target();
            int cur_f = gl_scaler_get_factor();
            int max_f = gl_scaler_get_max_factor(g_app.main_dpy, target);
            int next_f = cur_f + 1;

            if (next_f <= max_f) {
                gl_scaler_set_factor(next_f);
                if (config_get_notifications()) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Window scaled to %dx.\nPress Ctrl+Alt+S to cycle, or Esc to restore.", next_f);
                    notify_message("AspectScale", msg);
                }
            } else {
                /* Exceeded screen resolution: cycle back to 1x (restore original window) */
                g_app.dismissed_win = target;
                gl_scaler_stop();
                if (config_get_notifications()) {
                    notify_message("AspectScale", "Restored window to original size (1x).");
                }
            }
            return;
        }
    }

    Window active = x11_get_active_window(g_app.main_dpy);
    trigger_window_scale_target(active, 2);
}

static void trigger_restore_active(void) {
    fprintf(stderr, "AspectScale Debug: trigger_restore_active() called\n");
    if (gl_scaler_is_running()) {
        g_app.dismissed_win = gl_scaler_get_target();
        gl_scaler_stop();
        if (config_get_notifications()) {
            notify_message("AspectScale", "Restored original window size (1x).");
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

            char exe_name[128] = {0};
            bool is_wine = false;
            x11_get_window_exe(g_app.main_dpy, active, exe_name, sizeof(exe_name), &is_wine);

            if (config_is_autoscale(cls, nam, title, exe_name, is_wine)) {
                fprintf(stderr, "AspectScale Debug: autoscale triggered for win=0x%lx title='%s' class='%s' exe='%s' wine=%d\n",
                        (unsigned long)active, title, cls ? cls : "", exe_name, is_wine);
                trigger_fullscreen_target(active);
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

static gboolean on_hotkey_fullscreen_idle(gpointer user_data) {
    (void)user_data;
    fprintf(stderr, "AspectScale Debug: on_hotkey_fullscreen_idle triggered by Ctrl+Alt+F!\n");
    trigger_fullscreen_active();
    return G_SOURCE_REMOVE;
}

static gboolean on_hotkey_window_scale_idle(gpointer user_data) {
    (void)user_data;
    fprintf(stderr, "AspectScale Debug: on_hotkey_window_scale_idle triggered by Ctrl+Alt+S!\n");
    trigger_window_scale_cycle();
    return G_SOURCE_REMOVE;
}

static gboolean on_hotkey_restore_idle(gpointer user_data) {
    (void)user_data;
    fprintf(stderr, "AspectScale Debug: on_hotkey_restore_idle triggered by Ctrl+Alt+R!\n");
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
            if (kev->keycode == g_app.keycode_f) {
                g_idle_add(on_hotkey_fullscreen_idle, NULL);
            } else if (kev->keycode == g_app.keycode_s) {
                g_idle_add(on_hotkey_window_scale_idle, NULL);
            } else if (kev->keycode == g_app.keycode_r) {
                g_idle_add(on_hotkey_restore_idle, NULL);
            }
        }
    }
    return NULL;
}

static void on_fullscreen_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    trigger_fullscreen_active();
}

static void on_window_scale_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    (void)user_data;
    trigger_window_scale_cycle();
}

static void on_scale_preset_clicked(GtkMenuItem *item, gpointer user_data) {
    (void)item;
    int factor = GPOINTER_TO_INT(user_data);
    Window target = gl_scaler_is_running() ? gl_scaler_get_target() : x11_get_active_window(g_app.main_dpy);
    trigger_window_scale_target(target, factor);
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

    char exe_name[128] = {0};
    bool is_wine = false;
    x11_get_window_exe(g_app.main_dpy, active, exe_name, sizeof(exe_name), &is_wine);

    if (config_add_autoscale(cls, nam, title, exe_name, is_wine)) {
        char msg[512];
        const char *display_name = title[0] ? title : (exe_name[0] ? exe_name : (cls ? cls : "Window"));
        snprintf(msg, sizeof(msg), "Remembered \"%s\"!\nWill automatically scale to fullscreen when launched.",
                 display_name);
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

static void update_scale_mode_parent_label(ScaleFilterMode mode) {
    if (!g_app.scale_mode_parent) return;
    const char *label = "Scaling: Filtering (Bilinear)";
    if (mode == SCALE_FILTER_INTEGER) label = "Scaling: Integer (Pixel-Perfect)";
    else if (mode == SCALE_FILTER_NEAREST) label = "Scaling: No Filtering (Sharp)";
    gtk_menu_item_set_label(GTK_MENU_ITEM(g_app.scale_mode_parent), label);
}

static void on_scale_mode_toggled(GtkCheckMenuItem *item, gpointer user_data) {
    if (!gtk_check_menu_item_get_active(item)) return;
    ScaleFilterMode mode = (ScaleFilterMode)GPOINTER_TO_INT(user_data);
    config_set_scale_mode(mode);
    gl_scaler_set_scale_mode(mode);
    update_scale_mode_parent_label(mode);

    if (config_get_notifications()) {
        const char *name = (mode == SCALE_FILTER_INTEGER) ? "Integer Scaling (Pixel-Perfect)" :
                           ((mode == SCALE_FILTER_NEAREST) ? "No Filtering (Nearest Neighbor)" :
                            "Filtering (Bilinear)");
        char msg[256];
        snprintf(msg, sizeof(msg), "Scaling mode set to: %s", name);
        notify_message("AspectScale", msg);
    }
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
        "Hardware-Accelerated Aspect-Corrected Scaler for Linux (X11)\n\n"
        "• Fullscreen Scaler:\n"
        "  - Global Hotkey: Ctrl + Alt + F\n"
        "  - Scales active window to full monitor resolution with exact aspect ratio,\n"
        "    pure black borders, covering desktop panels.\n\n"
        "• Windowed Scaler (2x, 3x, 4x...):\n"
        "  - Global Hotkey: Ctrl + Alt + S\n"
        "  - Scales active window to 2x, 3x, 4x in a GPU-accelerated window.\n"
        "  - Stops before exceeding screen resolution, then cycles back to 1x.\n"
        "  - Direct presets (1x, 2x, 3x, 4x, 5x, Max Fit) in the tray menu.\n\n"
        "• Restore:\n"
        "  - Ctrl + Alt + R or Escape: Restores original window size (1x).\n\n"
        "• Zero-copy GPU texturing via GLX_EXT_texture_from_pixmap.",
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
    if (gl_scaler_is_running()) {
        gl_scaler_stop();
    }
    gl_scaler_cleanup();
    if (g_app.hotkey_dpy) {
        XClientMessageEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.window = DefaultRootWindow(g_app.hotkey_dpy);
        ev.format = 32;
        XSendEvent(g_app.hotkey_dpy, DefaultRootWindow(g_app.hotkey_dpy), False, 0, (XEvent *)&ev);
        XFlush(g_app.hotkey_dpy);
    }
    gtk_main_quit();
    exit(0);
}

static void handle_sigterm(int sig) {
    (void)sig;
    g_app.running = false;
    if (gl_scaler_is_running()) {
        gl_scaler_stop();
    }
    gl_scaler_cleanup();
    exit(0);
}

static void build_tray_menu(void) {
    GtkWidget *menu = gtk_menu_new();

    /* Header */
    GtkWidget *header_item = gtk_menu_item_new_with_label("AspectScale (Fullscreen & Window Scaler)");
    gtk_widget_set_sensitive(header_item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), header_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    /* Fullscreen Scaling Action */
    GtkWidget *fullscreen_item = gtk_menu_item_new_with_label("Scale to Fullscreen          [Ctrl+Alt+F]");
    g_signal_connect(fullscreen_item, "activate", G_CALLBACK(on_fullscreen_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), fullscreen_item);

    /* Window Scaling Action */
    GtkWidget *scale_item = gtk_menu_item_new_with_label("Scale Window (Next)         [Ctrl+Alt+S]");
    g_signal_connect(scale_item, "activate", G_CALLBACK(on_window_scale_clicked), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), scale_item);

    /* Window Scale Preset Submenu */
    GtkWidget *preset_parent = gtk_menu_item_new_with_label("Window Scale Preset");
    GtkWidget *preset_submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(preset_parent), preset_submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), preset_parent);

    GtkWidget *p1_item = gtk_menu_item_new_with_label("1x (Restore Original)");
    g_signal_connect(p1_item, "activate", G_CALLBACK(on_scale_preset_clicked), GINT_TO_POINTER(1));
    gtk_menu_shell_append(GTK_MENU_SHELL(preset_submenu), p1_item);

    GtkWidget *p2_item = gtk_menu_item_new_with_label("2x");
    g_signal_connect(p2_item, "activate", G_CALLBACK(on_scale_preset_clicked), GINT_TO_POINTER(2));
    gtk_menu_shell_append(GTK_MENU_SHELL(preset_submenu), p2_item);

    GtkWidget *p3_item = gtk_menu_item_new_with_label("3x");
    g_signal_connect(p3_item, "activate", G_CALLBACK(on_scale_preset_clicked), GINT_TO_POINTER(3));
    gtk_menu_shell_append(GTK_MENU_SHELL(preset_submenu), p3_item);

    GtkWidget *p4_item = gtk_menu_item_new_with_label("4x");
    g_signal_connect(p4_item, "activate", G_CALLBACK(on_scale_preset_clicked), GINT_TO_POINTER(4));
    gtk_menu_shell_append(GTK_MENU_SHELL(preset_submenu), p4_item);

    GtkWidget *p5_item = gtk_menu_item_new_with_label("5x");
    g_signal_connect(p5_item, "activate", G_CALLBACK(on_scale_preset_clicked), GINT_TO_POINTER(5));
    gtk_menu_shell_append(GTK_MENU_SHELL(preset_submenu), p5_item);

    GtkWidget *pmax_item = gtk_menu_item_new_with_label("Max Fit (Fill Screen Integer)");
    g_signal_connect(pmax_item, "activate", G_CALLBACK(on_scale_preset_clicked), GINT_TO_POINTER(999));
    gtk_menu_shell_append(GTK_MENU_SHELL(preset_submenu), pmax_item);

    /* Restore Windowed Action */
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

    /* Scaling / Filter mode options */
    GtkWidget *scale_mode_parent = gtk_menu_item_new_with_label("Scaling Mode");
    GtkWidget *scale_mode_submenu = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(scale_mode_parent), scale_mode_submenu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), scale_mode_parent);
    g_app.scale_mode_parent = scale_mode_parent;

    GSList *sm_group = NULL;
    GtkWidget *filter_item = gtk_radio_menu_item_new_with_label(sm_group, "Filtering (Bilinear / Smooth)");
    sm_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(filter_item));

    GtkWidget *nofilter_item = gtk_radio_menu_item_new_with_label(sm_group, "No Filtering (Nearest Neighbor / Sharp)");
    sm_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(nofilter_item));

    GtkWidget *integer_item = gtk_radio_menu_item_new_with_label(sm_group, "Integer Scaling (Pixel-Perfect)");

    ScaleFilterMode cur_sm = config_get_scale_mode();
    if (cur_sm == SCALE_FILTER_INTEGER) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(integer_item), TRUE);
    } else if (cur_sm == SCALE_FILTER_NEAREST) {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(nofilter_item), TRUE);
    } else {
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(filter_item), TRUE);
    }
    update_scale_mode_parent_label(cur_sm);

    g_signal_connect(filter_item, "toggled", G_CALLBACK(on_scale_mode_toggled), GINT_TO_POINTER(SCALE_FILTER_BILINEAR));
    g_signal_connect(nofilter_item, "toggled", G_CALLBACK(on_scale_mode_toggled), GINT_TO_POINTER(SCALE_FILTER_NEAREST));
    g_signal_connect(integer_item, "toggled", G_CALLBACK(on_scale_mode_toggled), GINT_TO_POINTER(SCALE_FILTER_INTEGER));

    gtk_menu_shell_append(GTK_MENU_SHELL(scale_mode_submenu), filter_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(scale_mode_submenu), nofilter_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(scale_mode_submenu), integer_item);

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
    printf("AspectScale v%s - Aspect-Corrected Scaler for Linux (X11)\n\n"
           "Usage: %s [OPTIONS]\n\n"
           "Options:\n"
           "  -h, --help            Show this help message\n"
           "  -v, --version         Show version\n"
           "  --scale-fullscreen    Scale currently active window to fullscreen\n"
           "  --scale-window        Scale active window in windowed mode (2x, 3x...)\n"
           "  --factor <N>          Specify integer scale factor for windowed scale (e.g. 2, 3, 4)\n"
           "  --restore-active      Restore currently active window to original (1x)\n"
           "  --remember-active     Add active window to auto-scale list\n"
           "  --filtering           Set scaling mode to Bilinear Filtering (smooth)\n"
           "  --no-filtering        Set scaling mode to No Filtering (nearest neighbor / sharp)\n"
           "  --integer             Set scaling mode to Integer Scaling (pixel-perfect)\n"
           "  --no-hide-cursor      Do not hide cursor in fullscreen\n"
           "  --no-notify           Disable desktop notifications\n"
           "  --no-tray             Run as daemon without system tray icon\n\n"
           "Hotkeys:\n"
           "  Ctrl + Alt + F        Scale active window to fullscreen (toggle restores)\n"
           "  Ctrl + Alt + S        Cycle windowed scaling (2x, 3x... stops before screen res, cycles to 1x)\n"
           "  Ctrl + Alt + R        Restore active window (1x)\n"
           "  Escape                Exit fullscreen or windowed scaling\n",
           APP_VERSION, prog);
}

int main(int argc, char *argv[]) {
    XInitThreads();
    XSetErrorHandler(x11_error_handler);
    config_init();

    g_app.running = true;
    g_app.last_active_win = None;
    g_app.dismissed_win = None;

    bool one_shot_fullscreen = false;
    bool one_shot_window = false;
    int one_shot_factor = 2;
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
        } else if (strcmp(argv[i], "--scale-fullscreen") == 0 || strcmp(argv[i], "--scale-active") == 0) {
            one_shot_fullscreen = true;
        } else if (strcmp(argv[i], "--scale-window") == 0) {
            one_shot_window = true;
        } else if (strcmp(argv[i], "--factor") == 0 && i + 1 < argc) {
            one_shot_factor = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--restore-active") == 0) {
            one_shot_restore = true;
        } else if (strcmp(argv[i], "--remember-active") == 0) {
            one_shot_remember = true;
        } else if (strcmp(argv[i], "--filtering") == 0) {
            config_set_scale_mode(SCALE_FILTER_BILINEAR);
        } else if (strcmp(argv[i], "--no-filtering") == 0 || strcmp(argv[i], "--nearest") == 0) {
            config_set_scale_mode(SCALE_FILTER_NEAREST);
        } else if (strcmp(argv[i], "--integer") == 0) {
            config_set_scale_mode(SCALE_FILTER_INTEGER);
        } else if (strcmp(argv[i], "--no-hide-cursor") == 0) {
            config_set_hide_cursor(false);
        } else if (strcmp(argv[i], "--no-notify") == 0) {
            config_set_notifications(false);
        } else if (strcmp(argv[i], "--no-tray") == 0) {
            enable_tray = false;
        }
    }

    signal(SIGINT, handle_sigterm);
    signal(SIGTERM, handle_sigterm);

    g_app.main_dpy = XOpenDisplay(NULL);
    if (!g_app.main_dpy) {
        fprintf(stderr, "Error: Could not open X11 display. Are you running an X11 session?\n");
        return 1;
    }

    x11_scale_init(g_app.main_dpy);
    gl_scaler_init(g_app.main_dpy);
    gl_scaler_set_hide_cursor(config_get_hide_cursor());
    gl_scaler_set_scale_mode(config_get_scale_mode());

    /* Handle one-shot actions */
    if (one_shot_fullscreen) {
        trigger_fullscreen_active();
        while (gl_scaler_is_running()) usleep(50000);
        XCloseDisplay(g_app.main_dpy);
        return 0;
    }
    if (one_shot_window) {
        Window active = x11_get_active_window(g_app.main_dpy);
        trigger_window_scale_target(active, one_shot_factor);
        while (gl_scaler_is_running()) usleep(50000);
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

    KeySym sym_f = XK_f;
    KeySym sym_s = XK_s;
    KeySym sym_r = XK_r;
    g_app.keycode_f = XKeysymToKeycode(g_app.hotkey_dpy, sym_f);
    g_app.keycode_s = XKeysymToKeycode(g_app.hotkey_dpy, sym_s);
    g_app.keycode_r = XKeysymToKeycode(g_app.hotkey_dpy, sym_r);

    if (g_app.keycode_f != 0) {
        x11_grab_key(g_app.hotkey_dpy, g_app.keycode_f, ControlMask | Mod1Mask);
    }

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
        notify_message("AspectScale", "Ready. Press Ctrl+Alt+F for fullscreen, Ctrl+Alt+S for 2x/3x windowed scale.");
    }

    /* Run GTK Main Loop */
    gtk_main();

    /* Cleanup */
    g_app.running = false;
    if (gl_scaler_is_running()) {
        gl_scaler_stop();
    }
    gl_scaler_cleanup();
    x11_scale_cleanup(g_app.main_dpy);
    exit(0);
}
