#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

typedef enum {
    SCALE_FILTER_BILINEAR = 0,   /* Filtering (Bilinear) */
    SCALE_FILTER_NEAREST = 1,    /* No Filtering (Nearest) */
    SCALE_FILTER_INTEGER = 2     /* Integer Scaling */
} ScaleFilterMode;

#define MAX_AUTOSCALE_RULES 64

typedef struct {
    char identifier[128]; /* class name or window identifier */
    char label[128];      /* human readable title / label */
} AutoScaleRule;

typedef struct {
    bool hide_cursor;
    bool show_notifications;
    ScaleFilterMode scale_mode;
    AutoScaleRule rules[MAX_AUTOSCALE_RULES];
    int rule_count;
} AppConfig;

void config_init(void);
void config_save(void);

bool config_get_hide_cursor(void);
void config_set_hide_cursor(bool hide);

bool config_get_notifications(void);
void config_set_notifications(bool enabled);

ScaleFilterMode config_get_scale_mode(void);
void config_set_scale_mode(ScaleFilterMode mode);

bool is_generic_wine_id(const char *str);
bool config_is_autoscale(const char *res_class, const char *res_name, const char *title, const char *exe_name, bool is_wine);
bool config_add_autoscale(const char *res_class, const char *res_name, const char *title, const char *exe_name, bool is_wine);
bool config_remove_autoscale(const char *identifier);
void config_clear_autoscale(void);

const AppConfig* config_get(void);

#endif /* CONFIG_H */

