#ifndef C1_UI_PREFERENCES_H
#define C1_UI_PREFERENCES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define C1_DESKTOP_CONFIG "/usr/data/c1/desktop.conf"
#define C1_LOCK_TEXT_BYTES 193U

typedef enum { C1_LANGUAGE_ZH = 0, C1_LANGUAGE_EN = 1 } c1_language;
typedef enum {
    C1_LOCK_WALLPAPER = 0, C1_LOCK_FREEZE, C1_LOCK_TEXT, C1_LOCK_CALENDAR
} c1_lock_style;
typedef enum {
    C1_POWER_MODE_SAVING = 0, C1_POWER_MODE_STANDARD, C1_POWER_MODE_PERFORMANCE
} c1_power_mode;
typedef struct {
    uint32_t lock_minutes;
    uint32_t shutdown_minutes; /* Separate countdown starting at manual/automatic lock. */
    bool suspend_on_lock;
} c1_power_settings;
typedef enum {
    C1_SETTING_LANGUAGE = 0,
    C1_SETTING_POWER_MODE,
    C1_SETTING_LOCK_STYLE,
    C1_SETTING_TIME_ZONE,
    C1_SETTING_NETWORK_TIME,
    C1_SETTING_LOCK_TEXT,
    C1_SETTING_UPDATE,
    C1_SETTING_ABOUT,
    C1_SETTING_COUNT
} c1_setting;
typedef struct {
    c1_language language;
    c1_lock_style lock_style;
    c1_power_mode power_mode;
    int utc_offset_minutes;
    bool network_time;
    char lock_text[C1_LOCK_TEXT_BYTES];
    bool terminal_enabled;
    bool background_checks;
} c1_preferences;

c1_preferences c1_preferences_default(void);
/* Unknown/absent modes default to Standard; malformed explicit modes fail
 * validation. Legacy timeout fields are checked but never create a fourth mode. */
c1_power_settings c1_preferences_power_settings(c1_power_mode mode);
bool c1_preferences_load(c1_preferences *prefs, const char *path);
bool c1_preferences_save(const c1_preferences *prefs, const char *path);
/* Nonempty, printable UTF-8, NUL-terminated within C1_LOCK_TEXT_BYTES. */
bool c1_preferences_valid_text(const char *text);
const char *c1_ui_tr(c1_language language, const char *zh, const char *en);
/* index is c1_setting; editing/actions are handled by the UI model. */
void c1_preferences_cycle(c1_preferences *prefs, unsigned int index, int direction);

#endif
