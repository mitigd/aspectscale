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
                snprintf(r->identifier, sizeof(r->identifier), "%.127s", key);
                snprintf(r->label, sizeof(r->label), "%.127s", val[0] ? val : key);
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

bool config_is_autoscale(const char *res_class, const char *res_name, const char *title) {
    for (int i = 0; i < s_config.rule_count; i++) {
        const char *id = s_config.rules[i].identifier;
        const char *lbl = s_config.rules[i].label;

        // Match identifier against class, name, title
        if (res_class && (strcasecmp(id, res_class) == 0 || strcasestr(res_class, id) != NULL)) return true;
        if (res_name && (strcasecmp(id, res_name) == 0 || strcasestr(res_name, id) != NULL)) return true;
        if (title && strcasestr(title, id) != NULL) return true;

        // Also match label against title, class, name
        if (lbl && lbl[0]) {
            if (title && strcasestr(title, lbl) != NULL) return true;
            if (res_class && strcasestr(res_class, lbl) != NULL) return true;
            if (res_name && strcasestr(res_name, lbl) != NULL) return true;
        }
    }
    return false;
}

bool config_add_autoscale(const char *res_class, const char *res_name, const char *title) {
    const char *id = (res_class && res_class[0]) ? res_class :
                     ((res_name && res_name[0]) ? res_name : title);
    if (!id || !id[0]) return false;

    for (int i = 0; i < s_config.rule_count; i++) {
        if (strcasecmp(s_config.rules[i].identifier, id) == 0) {
            return false;
        }
    }

    if (s_config.rule_count >= MAX_AUTOSCALE_RULES) {
        return false;
    }

    AutoScaleRule *r = &s_config.rules[s_config.rule_count++];
    snprintf(r->identifier, sizeof(r->identifier), "%.127s", id);
    const char *lbl = (title && title[0]) ? title : id;
    snprintf(r->label, sizeof(r->label), "%.127s", lbl);

    config_save();
    return true;
}

bool config_remove_autoscale(const char *identifier) {
    if (!identifier) return false;
    int idx = -1;
    for (int i = 0; i < s_config.rule_count; i++) {
        if (strcasecmp(s_config.rules[i].identifier, identifier) == 0) {
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
