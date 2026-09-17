#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static AppConfig s_config;
static char s_config_path[512] = {0};

static void get_config_path(char *out, size_t max_len) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(out, max_len, "%s/.config/aspectscale", home);
    mkdir(out, 0755);
    snprintf(out, max_len, "%s/.config/aspectscale/config.ini", home);
}

void config_init(void) {
    memset(&s_config, 0, sizeof(AppConfig));
    s_config.hide_cursor = true;
    s_config.show_notifications = true;
    s_config.scale_mode = SCALE_FILTER_BILINEAR;
    s_config.rule_count = 0;

    get_config_path(s_config_path, sizeof(s_config_path));

    FILE *f = fopen(s_config_path, "r");
    if (!f) return;

    char line[256];
    char section[64] = {0};

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\0') continue;

        if (*p == '[' && strchr(p, ']')) {
            char *end = strchr(p, ']');
            *end = '\0';
            snprintf(section, sizeof(section), "%s", p + 1);
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        if (strcmp(section, "settings") == 0) {
            if (strcmp(key, "hide_cursor") == 0) {
                s_config.hide_cursor = (atoi(val) != 0);
            } else if (strcmp(key, "show_notifications") == 0) {
                s_config.show_notifications = (atoi(val) != 0);
            } else if (strcmp(key, "scale_mode") == 0 || strcmp(key, "scaling") == 0 || strcmp(key, "filter") == 0) {
                if (strcasecmp(val, "integer") == 0 || strcmp(val, "2") == 0) {
                    s_config.scale_mode = SCALE_FILTER_INTEGER;
                } else if (strcasecmp(val, "none") == 0 || strcasecmp(val, "nearest") == 0 ||
                           strcasecmp(val, "no_filtering") == 0 || strcasecmp(val, "no-filtering") == 0 ||
                           strcmp(val, "1") == 0) {
                    s_config.scale_mode = SCALE_FILTER_NEAREST;
                } else {
                    s_config.scale_mode = SCALE_FILTER_BILINEAR;
                }
            }
        } else if (strcmp(section, "autoscale") == 0) {
            if (s_config.rule_count < MAX_AUTOSCALE_RULES && strlen(key) > 0) {
                AutoScaleRule *r = &s_config.rules[s_config.rule_count++];
                if (is_generic_wine_id(key) && val[0]) {
                    // Migrate legacy generic wine runner entry (e.g. steam_app_default=RhemIISE)
                    snprintf(r->identifier, sizeof(r->identifier), "%.127s", val);
                    snprintf(r->label, sizeof(r->label), "%.127s", val);
                } else {
                    snprintf(r->identifier, sizeof(r->identifier), "%.127s", key);
                    snprintf(r->label, sizeof(r->label), "%.127s", val[0] ? val : key);
                }
            }
        }
    }
    fclose(f);
}

void config_save(void) {
    if (s_config_path[0] == '\0') {
        get_config_path(s_config_path, sizeof(s_config_path));
    }

    FILE *f = fopen(s_config_path, "w");
    if (!f) return;

    fprintf(f, "[settings]\n");
    fprintf(f, "hide_cursor=%d\n", s_config.hide_cursor ? 1 : 0);
    fprintf(f, "show_notifications=%d\n", s_config.show_notifications ? 1 : 0);
    const char *sm = "filtering";
    if (s_config.scale_mode == SCALE_FILTER_INTEGER) sm = "integer";
    else if (s_config.scale_mode == SCALE_FILTER_NEAREST) sm = "no_filtering";
    fprintf(f, "scale_mode=%s\n\n", sm);

    fprintf(f, "[autoscale]\n");
    for (int i = 0; i < s_config.rule_count; i++) {
        fprintf(f, "%s=%s\n", s_config.rules[i].identifier, s_config.rules[i].label);
    }
    fclose(f);
}

bool config_get_hide_cursor(void) {
    return s_config.hide_cursor;
}

void config_set_hide_cursor(bool hide) {
    s_config.hide_cursor = hide;
    config_save();
}

bool config_get_notifications(void) {
    return s_config.show_notifications;
}

void config_set_notifications(bool enabled) {
    s_config.show_notifications = enabled;
    config_save();
}

ScaleFilterMode config_get_scale_mode(void) {
    return s_config.scale_mode;
}

void config_set_scale_mode(ScaleFilterMode mode) {
    s_config.scale_mode = mode;
    config_save();
}

bool is_generic_wine_id(const char *str) {
    if (!str || !str[0]) return false;
    if (strcasecmp(str, "wine") == 0) return true;
    if (strcasecmp(str, "wine64") == 0) return true;
    if (strcasecmp(str, "wine-preloader") == 0) return true;
    if (strcasecmp(str, "wine64-preloader") == 0) return true;
    if (strcasecmp(str, "steam_app_default") == 0) return true;
    if (strcasecmp(str, "steam_app_0") == 0) return true;
    if (strncasecmp(str, "steam_app_", 10) == 0) return true;
    if (strcasecmp(str, "explorer.exe") == 0) return true;
    if (strcasecmp(str, "winevdm.exe") == 0) return true;
    if (strcasecmp(str, "start.exe") == 0) return true;
    if (strcasecmp(str, "winedevice.exe") == 0) return true;
    return false;
}

static bool name_matches(const char *target, const char *pattern) {
    if (!target || !pattern || !target[0] || !pattern[0]) return false;
    // Exact case-insensitive match
    if (strcasecmp(target, pattern) == 0) return true;

    // Substring match
    if (strcasestr(target, pattern) != NULL || strcasestr(pattern, target) != NULL) return true;

    // Match stem (strip .exe if present)
    char t_stem[128], p_stem[128];
    snprintf(t_stem, sizeof(t_stem), "%s", target);
    snprintf(p_stem, sizeof(p_stem), "%s", pattern);
    char *dot = strrchr(t_stem, '.');
    if (dot && strcasecmp(dot, ".exe") == 0) *dot = '\0';
    dot = strrchr(p_stem, '.');
    if (dot && strcasecmp(dot, ".exe") == 0) *dot = '\0';

    if (t_stem[0] && p_stem[0]) {
        if (strcasecmp(t_stem, p_stem) == 0) return true;
        if (strcasestr(t_stem, p_stem) != NULL || strcasestr(p_stem, t_stem) != NULL) return true;
    }

    return false;
}

bool config_is_autoscale(const char *res_class, const char *res_name, const char *title, const char *exe_name, bool is_wine) {
    for (int i = 0; i < s_config.rule_count; i++) {
        const char *id = s_config.rules[i].identifier;
        const char *lbl = s_config.rules[i].label;

        // If rule identifier is a generic wine runner (e.g. unmigrated legacy rule),
        // NEVER match generic class/name directly! Only match against label!
        if (is_generic_wine_id(id)) {
            if (lbl && lbl[0]) {
                if (exe_name && exe_name[0] && name_matches(exe_name, lbl)) return true;
                if (title && title[0] && name_matches(title, lbl)) return true;
            }
            continue;
        }

        // 1. Match against actual EXE name
        if (exe_name && exe_name[0]) {
            if (name_matches(exe_name, id)) return true;
            if (lbl && lbl[0] && name_matches(exe_name, lbl)) return true;
        }

        // 2. Match against window title
        if (title && title[0]) {
            if (name_matches(title, id)) return true;
            if (lbl && lbl[0] && name_matches(title, lbl)) return true;
        }

        // 3. Match against class & name
        if (!is_wine) {
            if (res_class && !is_generic_wine_id(res_class)) {
                if (name_matches(res_class, id)) return true;
                if (lbl && lbl[0] && name_matches(res_class, lbl)) return true;
            }
            if (res_name && !is_generic_wine_id(res_name)) {
                if (name_matches(res_name, id)) return true;
                if (lbl && lbl[0] && name_matches(res_name, lbl)) return true;
            }
        } else {
            // For wine windows, only match class/name if they are non-generic
            if (res_class && !is_generic_wine_id(res_class) && name_matches(res_class, id)) return true;
            if (res_name && !is_generic_wine_id(res_name) && name_matches(res_name, id)) return true;
        }
    }
    return false;
}

bool config_add_autoscale(const char *res_class, const char *res_name, const char *title, const char *exe_name, bool is_wine) {
    char id[128] = {0};
    char lbl[128] = {0};

    if (is_wine) {
        if (exe_name && exe_name[0] && !is_generic_wine_id(exe_name)) {
            snprintf(id, sizeof(id), "%s", exe_name);
        } else if (title && title[0]) {
            snprintf(id, sizeof(id), "%s", title);
        } else if (res_name && res_name[0] && !is_generic_wine_id(res_name)) {
            snprintf(id, sizeof(id), "%s", res_name);
        } else {
            snprintf(id, sizeof(id), "WineApp");
        }
    } else {
        if (res_class && res_class[0]) {
            snprintf(id, sizeof(id), "%s", res_class);
        } else if (res_name && res_name[0]) {
            snprintf(id, sizeof(id), "%s", res_name);
        } else if (exe_name && exe_name[0]) {
            snprintf(id, sizeof(id), "%s", exe_name);
        } else if (title && title[0]) {
            snprintf(id, sizeof(id), "%s", title);
        } else {
            return false;
        }
    }

    if (title && title[0]) {
        snprintf(lbl, sizeof(lbl), "%s", title);
    } else if (exe_name && exe_name[0]) {
        snprintf(lbl, sizeof(lbl), "%s", exe_name);
    } else {
        snprintf(lbl, sizeof(lbl), "%s", id);
    }

    // Check if already in list
    for (int i = 0; i < s_config.rule_count; i++) {
        if (strcasecmp(s_config.rules[i].identifier, id) == 0 ||
            (lbl[0] && strcasecmp(s_config.rules[i].label, lbl) == 0 &&
             name_matches(s_config.rules[i].identifier, id))) {
            return false;
        }
    }

    if (s_config.rule_count >= MAX_AUTOSCALE_RULES) {
        return false;
    }

    AutoScaleRule *r = &s_config.rules[s_config.rule_count++];
    snprintf(r->identifier, sizeof(r->identifier), "%.127s", id);
    snprintf(r->label, sizeof(r->label), "%.127s", lbl);

    config_save();
    return true;
}

bool config_remove_autoscale(const char *identifier) {
    if (!identifier) return false;
    int idx = -1;
    for (int i = 0; i < s_config.rule_count; i++) {
        if (strcasecmp(s_config.rules[i].identifier, identifier) == 0 ||
            strcasecmp(s_config.rules[i].label, identifier) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return false;

    for (int i = idx; i < s_config.rule_count - 1; i++) {
        s_config.rules[i] = s_config.rules[i + 1];
    }
    s_config.rule_count--;
    config_save();
    return true;
}

void config_clear_autoscale(void) {
    s_config.rule_count = 0;
    config_save();
}

const AppConfig* config_get(void) {
    return &s_config;
}
