/*
 * TimeTo - continuous time tracker for Flipper Zero.
 *
 * Inspired by timeto.me: you are always tracking exactly one activity.
 * Switching to another activity closes the current interval and starts a
 * new one. Every closed interval is appended to a CSV file on the SD card,
 * and the running interval is saved too, so tracking continues even when
 * the app is closed or the Flipper is restarted.
 *
 * Files (on the SD card):
 *   /ext/apps_data/timeto/activities.txt  one activity name per line (editable)
 *   /ext/apps_data/timeto/current.txt     running interval: "<start_ts>,<name>"
 *   /ext/apps_data/timeto/log.csv         finished intervals
 *   /ext/apps_data/timeto/tags.txt        NFC tags: "<uid hex>,<activity>"
 *
 * The start time of the running interval can be changed (Edit > Change start
 * time). If the previous interval ended where the running one began, its end
 * moves too, so the timeline stays continuous.
 *
 * NFC: scanning a linked tag (any ISO14443-3A tag: NTAG, Ultralight, Mifare
 * Classic, DESFire...) switches to its activity. An unknown tag is linked to
 * an activity the first time it is scanned.
 */

#include <furi.h>
#include <furi_hal_rtc.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/elements.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>
#include <datetime/datetime.h>
#include <nfc/nfc.h>
#include <nfc/nfc_poller.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a.h>
#include <nfc/protocols/iso14443_3a/iso14443_3a_poller.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "TimeTo"

#define TIMETO_DIR       EXT_PATH("apps_data/timeto")
#define ACTIVITIES_PATH  TIMETO_DIR "/activities.txt"
#define CURRENT_PATH     TIMETO_DIR "/current.txt"
#define LOG_PATH         TIMETO_DIR "/log.csv"
#define LOG_HEADER       "start_ts,end_ts,duration_s,start,end,activity\n"
#define TAGS_PATH        TIMETO_DIR "/tags.txt"

#define MAX_ACTIVITIES   24
#define NAME_LEN         24
#define SECONDS_PER_DAY  86400UL
#define SUMMARY_ROWS     4
#define SUMMARY_MAX      (MAX_ACTIVITIES + 4)
#define MAX_TAGS         32
#define UID_STR_LEN      (ISO14443_3A_MAX_UID_SIZE * 2 + 1)
#define LOG_TAIL         256 /* bytes read from the end of log.csv to find the last row */
#define LOG_NAME_LEN     64

static const char* const default_activities[] = {
    "Work",
    "Study",
    "Exercise",
    "Eating",
    "Commute",
    "Chores",
    "Social",
    "Leisure",
    "Sleep",
    "Other",
};

typedef enum {
    ViewTracker,
    ViewSummary,
    ViewPicker,
    ViewManage,
    ViewDelete,
    ViewTextInput,
    ViewNfcScan,
    ViewNfcLink,
    ViewNfcUnlink,
    ViewStartEdit,
} TimeToView;

typedef enum {
    EventTick = 1,
    EventNfcTag,
} TimeToEvent;

typedef enum {
    ManageStartTime,
    ManageAdd,
    ManageDelete,
    ManageNfcLink,
    ManageNfcUnlink,
    ManageStop,
} ManageItem;

typedef struct {
    char uid[UID_STR_LEN];
    char name[NAME_LEN];
} NfcTag;

typedef struct {
    bool link_mode;
} NfcScanModel;

/* Last finished interval in log.csv. */
typedef struct {
    bool found;
    uint32_t start;
    uint32_t end;
    char name[LOG_NAME_LEN];
    uint64_t offset; /* byte offset of the row in log.csv */
} LastLog;

typedef struct {
    char name[NAME_LEN];
    uint32_t start; /* proposed start */
    uint32_t min; /* earliest allowed start */
    uint32_t now;
    bool has_prev; /* previous interval ends where this one starts and moves with it */
    char prev_name[NAME_LEN];
    uint32_t prev_start;
    uint8_t field; /* 0 = hours, 1 = minutes */
} StartEditModel;

typedef struct {
    bool tracking;
    char name[NAME_LEN];
    uint32_t start;
    uint32_t now;
    uint32_t today_prev; /* today's total for this activity, excluding running interval */
} TrackerModel;

typedef struct {
    char title[24];
    uint32_t total;
    size_t count;
    size_t scroll;
    char names[SUMMARY_MAX][NAME_LEN];
    uint32_t secs[SUMMARY_MAX];
} SummaryModel;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Storage* storage;
    NotificationApp* notifications;

    View* tracker_view;
    View* summary_view;
    Submenu* picker;
    Submenu* manage;
    Submenu* delete_menu;
    TextInput* text_input;
    View* nfc_scan_view;
    Submenu* nfc_link_menu;
    Submenu* nfc_unlink_menu;
    View* start_edit_view;
    FuriTimer* timer;

    /* NFC: allocated only while scanning, so the app doesn't hold the radio */
    Nfc* nfc;
    NfcPoller* nfc_poller;
    bool nfc_scanning;
    bool nfc_link_mode; /* always ask which activity, even for a known tag */
    uint8_t nfc_uid[ISO14443_3A_MAX_UID_SIZE];
    size_t nfc_uid_len;
    char scanned_uid[UID_STR_LEN];

    NfcTag tags[MAX_TAGS];
    size_t tag_count;

    char activities[MAX_ACTIVITIES][NAME_LEN];
    size_t activity_count;

    bool tracking;
    char current[NAME_LEN];
    uint32_t current_start;

    uint32_t today_prev;
    uint32_t today_day; /* day index (ts / 86400) that today_prev belongs to */

    uint32_t summary_day_offset; /* 0 = today, 1 = yesterday, ... */
    char input_buf[NAME_LEN];

    /* last log row, read when the start time editor is opened */
    LastLog edit_last;

    /* scratch buffers for totals */
    char tmp_names[SUMMARY_MAX][NAME_LEN];
    uint32_t tmp_secs[SUMMARY_MAX];
} TimeToApp;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

static uint32_t now_ts(void) {
    return furi_hal_rtc_get_timestamp();
}

/* Strip characters that would break the CSV / line based files. */
static void sanitize_name(char* s) {
    for(char* p = s; *p; p++) {
        if(*p == ',' || *p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
    }
    /* trim leading spaces */
    char* start = s;
    while(*start == ' ') start++;
    if(start != s) memmove(s, start, strlen(start) + 1);
    /* trim trailing spaces */
    size_t len = strlen(s);
    while(len > 0 && s[len - 1] == ' ') s[--len] = '\0';
}

static void format_datetime(uint32_t ts, char* out, size_t size) {
    DateTime dt;
    datetime_timestamp_to_datetime(ts, &dt);
    snprintf(
        out,
        size,
        "%04u-%02u-%02u %02u:%02u:%02u",
        dt.year,
        dt.month,
        dt.day,
        dt.hour,
        dt.minute,
        dt.second);
}

static void format_date(uint32_t ts, char* out, size_t size) {
    DateTime dt;
    datetime_timestamp_to_datetime(ts, &dt);
    snprintf(out, size, "%04u-%02u-%02u", dt.year, dt.month, dt.day);
}

static void format_duration(uint32_t secs, char* out, size_t size) {
    unsigned long h = secs / 3600;
    unsigned long m = (secs % 3600) / 60;
    if(h > 0) {
        snprintf(out, size, "%luh %02lum", h, m);
    } else if(m > 0) {
        snprintf(out, size, "%lum", m);
    } else {
        snprintf(out, size, "%lus", (unsigned long)secs);
    }
}

static uint32_t overlap(uint32_t a_start, uint32_t a_end, uint32_t b_start, uint32_t b_end) {
    uint32_t s = a_start > b_start ? a_start : b_start;
    uint32_t e = a_end < b_end ? a_end : b_end;
    return e > s ? e - s : 0;
}

/* ------------------------------------------------------------------------ */
/* Storage                                                                  */
/* ------------------------------------------------------------------------ */

static void storage_ensure_dir(TimeToApp* app) {
    storage_simply_mkdir(app->storage, EXT_PATH("apps_data"));
    storage_simply_mkdir(app->storage, TIMETO_DIR);
}

static bool write_text_file(TimeToApp* app, const char* path, const char* text) {
    File* file = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        size_t len = strlen(text);
        ok = storage_file_write(file, text, len) == len;
    }
    storage_file_close(file);
    storage_file_free(file);
    if(!ok) FURI_LOG_E(TAG, "Failed to write %s", path);
    return ok;
}

/* Calls on_line for every line in the file. Returns false if the file could not be opened. */
typedef void (*LineCallback)(const char* line, void* ctx);

static bool read_lines(TimeToApp* app, const char* path, LineCallback on_line, void* ctx) {
    File* file = storage_file_alloc(app->storage);
    bool opened = storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(opened) {
        FuriString* line = furi_string_alloc();
        char buf[128];
        size_t read;
        while((read = storage_file_read(file, buf, sizeof(buf))) > 0) {
            for(size_t i = 0; i < read; i++) {
                if(buf[i] == '\n') {
                    on_line(furi_string_get_cstr(line), ctx);
                    furi_string_reset(line);
                } else if(buf[i] != '\r') {
                    furi_string_push_back(line, buf[i]);
                }
            }
        }
        if(furi_string_size(line) > 0) on_line(furi_string_get_cstr(line), ctx);
        furi_string_free(line);
    }
    storage_file_close(file);
    storage_file_free(file);
    return opened;
}

static void save_activities(TimeToApp* app) {
    FuriString* text = furi_string_alloc();
    for(size_t i = 0; i < app->activity_count; i++) {
        furi_string_cat_printf(text, "%s\n", app->activities[i]);
    }
    write_text_file(app, ACTIVITIES_PATH, furi_string_get_cstr(text));
    furi_string_free(text);
}

static int find_activity(TimeToApp* app, const char* name) {
    for(size_t i = 0; i < app->activity_count; i++) {
        if(strcmp(app->activities[i], name) == 0) return (int)i;
    }
    return -1;
}

static bool add_activity(TimeToApp* app, const char* raw) {
    if(app->activity_count >= MAX_ACTIVITIES) return false;
    char name[NAME_LEN];
    strlcpy(name, raw, sizeof(name));
    sanitize_name(name);
    if(name[0] == '\0' || find_activity(app, name) >= 0) return false;
    strlcpy(app->activities[app->activity_count++], name, NAME_LEN);
    return true;
}

static void activity_line_cb(const char* line, void* ctx) {
    add_activity(ctx, line);
}

static void load_activities(TimeToApp* app) {
    app->activity_count = 0;
    read_lines(app, ACTIVITIES_PATH, activity_line_cb, app);
    if(app->activity_count == 0) {
        for(size_t i = 0; i < COUNT_OF(default_activities); i++) {
            add_activity(app, default_activities[i]);
        }
        save_activities(app);
    }
}

static void save_current(TimeToApp* app) {
    if(app->tracking) {
        char text[NAME_LEN + 16];
        snprintf(text, sizeof(text), "%lu,%s\n", (unsigned long)app->current_start, app->current);
        write_text_file(app, CURRENT_PATH, text);
    } else {
        storage_simply_remove(app->storage, CURRENT_PATH);
    }
}

static void current_line_cb(const char* line, void* ctx) {
    TimeToApp* app = ctx;
    if(app->tracking) return; /* only first line matters */
    const char* comma = strchr(line, ',');
    if(!comma) return;
    uint32_t start = strtoul(line, NULL, 10);
    char name[NAME_LEN];
    strlcpy(name, comma + 1, sizeof(name));
    sanitize_name(name);
    if(start == 0 || name[0] == '\0') return;
    app->current_start = start;
    strlcpy(app->current, name, NAME_LEN);
    app->tracking = true;
}

static void load_current(TimeToApp* app) {
    app->tracking = false;
    read_lines(app, CURRENT_PATH, current_line_cb, app);
}

static FuriString* format_log_line(uint32_t start, uint32_t end, const char* name) {
    char start_str[32];
    char end_str[32];
    format_datetime(start, start_str, sizeof(start_str));
    format_datetime(end, end_str, sizeof(end_str));
    return furi_string_alloc_printf(
        "%lu,%lu,%lu,%s,%s,%s\n",
        (unsigned long)start,
        (unsigned long)end,
        (unsigned long)(end - start),
        start_str,
        end_str,
        name);
}

/* log line: start_ts,end_ts,duration_s,start,end,activity */
static bool parse_log_line(const char* line, uint32_t* start, uint32_t* end, const char** name) {
    if(line[0] < '0' || line[0] > '9') return false; /* header or garbage */
    char* p;
    *start = strtoul(line, &p, 10);
    if(*p != ',') return false;
    *end = strtoul(p + 1, NULL, 10);

    /* activity is after the 5th comma */
    const char* n = line;
    for(int i = 0; i < 5; i++) {
        n = strchr(n, ',');
        if(!n) return false;
        n++;
    }
    *name = n;
    return true;
}

static void append_log(TimeToApp* app, uint32_t start, uint32_t end, const char* name) {
    if(end <= start) return;
    FuriString* line = format_log_line(start, end, name);

    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        if(storage_file_size(file) == 0) {
            storage_file_write(file, LOG_HEADER, strlen(LOG_HEADER));
        }
        size_t len = furi_string_size(line);
        if(storage_file_write(file, furi_string_get_cstr(line), len) != len) {
            FURI_LOG_E(TAG, "Failed to append log");
        }
    } else {
        FURI_LOG_E(TAG, "Failed to open log");
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(line);
}

/* Reads the last row of log.csv. Only the tail of the file is read. */
static void read_last_log(TimeToApp* app, LastLog* out) {
    memset(out, 0, sizeof(*out));
    File* file = storage_file_alloc(app->storage);
    if(storage_file_open(file, LOG_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(file);
        uint64_t from = size > LOG_TAIL ? size - LOG_TAIL : 0;
        char buf[LOG_TAIL + 1];
        size_t n = 0;
        if(storage_file_seek(file, (uint32_t)from, true)) {
            n = storage_file_read(file, buf, LOG_TAIL);
        }
        buf[n] = '\0';
        while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
        char* line = strrchr(buf, '\n');
        line = line ? line + 1 : (from == 0 ? buf : NULL); /* NULL: row longer than the tail */

        uint32_t start, end;
        const char* name;
        if(line && parse_log_line(line, &start, &end, &name)) {
            out->found = true;
            out->start = start;
            out->end = end;
            strlcpy(out->name, name, sizeof(out->name));
            out->offset = from + (uint64_t)(line - buf);
        }
    }
    storage_file_close(file);
    storage_file_free(file);
}

/* Replaces the last row of log.csv with the same interval ending at new_end. */
static bool rewrite_last_log_end(TimeToApp* app, const LastLog* last, uint32_t new_end) {
    if(!last->found || new_end <= last->start) return false;
    FuriString* line = format_log_line(last->start, new_end, last->name);
    File* file = storage_file_alloc(app->storage);
    bool ok = false;
    if(storage_file_open(file, LOG_PATH, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) {
        size_t len = furi_string_size(line);
        ok = storage_file_seek(file, (uint32_t)last->offset, true) && storage_file_truncate(file) &&
             storage_file_write(file, furi_string_get_cstr(line), len) == len;
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_string_free(line);
    if(!ok) FURI_LOG_E(TAG, "Failed to rewrite last log row");
    return ok;
}

static int find_tag(TimeToApp* app, const char* uid) {
    for(size_t i = 0; i < app->tag_count; i++) {
        if(strcmp(app->tags[i].uid, uid) == 0) return (int)i;
    }
    return -1;
}

static void save_tags(TimeToApp* app) {
    FuriString* text = furi_string_alloc();
    for(size_t i = 0; i < app->tag_count; i++) {
        furi_string_cat_printf(text, "%s,%s\n", app->tags[i].uid, app->tags[i].name);
    }
    write_text_file(app, TAGS_PATH, furi_string_get_cstr(text));
    furi_string_free(text);
}

/* Links uid to name, replacing an existing link for the same tag. */
static bool set_tag(TimeToApp* app, const char* uid, const char* raw_name) {
    char name[NAME_LEN];
    strlcpy(name, raw_name, sizeof(name));
    sanitize_name(name);
    if(uid[0] == '\0' || name[0] == '\0') return false;
    int idx = find_tag(app, uid);
    if(idx < 0) {
        if(app->tag_count >= MAX_TAGS) return false;
        idx = (int)app->tag_count++;
        strlcpy(app->tags[idx].uid, uid, UID_STR_LEN);
    }
    strlcpy(app->tags[idx].name, name, NAME_LEN);
    return true;
}

/* tags.txt line: <uid hex>,<activity> */
static void tag_line_cb(const char* line, void* ctx) {
    TimeToApp* app = ctx;
    const char* comma = strchr(line, ',');
    if(!comma) return;
    size_t uid_len = (size_t)(comma - line);
    if(uid_len == 0 || uid_len >= UID_STR_LEN) return;
    char uid[UID_STR_LEN];
    for(size_t i = 0; i < uid_len; i++) {
        char c = line[i];
        if(c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        if(!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return;
        uid[i] = c;
    }
    uid[uid_len] = '\0';
    set_tag(app, uid, comma + 1);
}

static void load_tags(TimeToApp* app) {
    app->tag_count = 0;
    read_lines(app, TAGS_PATH, tag_line_cb, app);
}

/* ------------------------------------------------------------------------ */
/* Totals                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    TimeToApp* app;
    uint32_t from;
    uint32_t to;
    size_t count;
} TotalsCtx;

static void totals_add(TotalsCtx* t, const char* name, uint32_t secs) {
    if(secs == 0) return;
    TimeToApp* app = t->app;
    for(size_t i = 0; i < t->count; i++) {
        if(strcmp(app->tmp_names[i], name) == 0) {
            app->tmp_secs[i] += secs;
            return;
        }
    }
    if(t->count >= SUMMARY_MAX) return;
    strlcpy(app->tmp_names[t->count], name, NAME_LEN);
    app->tmp_secs[t->count] = secs;
    t->count++;
}

static void totals_line_cb(const char* line, void* ctx) {
    TotalsCtx* t = ctx;
    uint32_t start, end;
    const char* name;
    if(!parse_log_line(line, &start, &end, &name)) return;
    totals_add(t, name, overlap(start, end, t->from, t->to));
}

/* Fills app->tmp_names / tmp_secs with per-activity totals for [from, to).
 * If include_running is set, the running interval is counted too. */
static size_t compute_totals(TimeToApp* app, uint32_t from, uint32_t to, bool include_running) {
    TotalsCtx t = {.app = app, .from = from, .to = to, .count = 0};
    read_lines(app, LOG_PATH, totals_line_cb, &t);
    if(include_running && app->tracking) {
        totals_add(&t, app->current, overlap(app->current_start, now_ts(), from, to));
    }
    return t.count;
}

static void refresh_today_prev(TimeToApp* app) {
    uint32_t now = now_ts();
    uint32_t day_start = now - (now % SECONDS_PER_DAY);
    app->today_day = now / SECONDS_PER_DAY;
    app->today_prev = 0;
    if(!app->tracking) return;
    size_t count = compute_totals(app, day_start, day_start + SECONDS_PER_DAY, false);
    for(size_t i = 0; i < count; i++) {
        if(strcmp(app->tmp_names[i], app->current) == 0) {
            app->today_prev = app->tmp_secs[i];
            break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Tracker view                                                             */
/* ------------------------------------------------------------------------ */

static void tracker_update_model(TimeToApp* app) {
    uint32_t now = now_ts();
    with_view_model(
        app->tracker_view,
        TrackerModel * model,
        {
            model->tracking = app->tracking;
            strlcpy(model->name, app->current, NAME_LEN);
            model->start = app->current_start;
            model->now = now;
            model->today_prev = app->today_prev;
        },
        true);
}

static void tracker_draw(Canvas* canvas, void* ctx) {
    TrackerModel* m = ctx;
    char buf[32];
    canvas_clear(canvas);

    /* Header: clock on the right */
    canvas_set_font(canvas, FontSecondary);
    uint32_t tod = m->now % SECONDS_PER_DAY;
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(tod / 3600), (unsigned long)((tod % 3600) / 60));
    canvas_draw_str_aligned(canvas, 127, 0, AlignRight, AlignTop, buf);

    if(!m->tracking) {
        canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "TimeTo");
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Not tracking");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignCenter, "Pick an activity to start");
        canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignCenter, "or scan an NFC tag (Down)");
        elements_button_center(canvas, "Start");
        elements_button_left(canvas, "Edit");
        elements_button_right(canvas, "Stats");
        return;
    }

    uint32_t start_tod = m->start % SECONDS_PER_DAY;
    snprintf(
        buf,
        sizeof(buf),
        "since %02lu:%02lu",
        (unsigned long)(start_tod / 3600),
        (unsigned long)((start_tod % 3600) / 60));
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, buf);

    /* Activity name */
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 11, AlignCenter, AlignTop, m->name);

    /* Elapsed */
    uint32_t elapsed = m->now > m->start ? m->now - m->start : 0;
    snprintf(
        buf,
        sizeof(buf),
        "%lu:%02lu:%02lu",
        (unsigned long)(elapsed / 3600),
        (unsigned long)((elapsed % 3600) / 60),
        (unsigned long)(elapsed % 60));
    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 23, AlignCenter, AlignTop, buf);

    /* Today total for this activity */
    uint32_t day_start = m->now - (m->now % SECONDS_PER_DAY);
    uint32_t today = m->today_prev + overlap(m->start, m->now, day_start, day_start + SECONDS_PER_DAY);
    char dur[16];
    format_duration(today, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "Today: %s", dur);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 41, AlignCenter, AlignTop, buf);

    elements_button_center(canvas, "Switch");
    elements_button_left(canvas, "Edit");
    elements_button_right(canvas, "Stats");
}

static void picker_rebuild(TimeToApp* app);
static void summary_refresh(TimeToApp* app);
static void nfc_scan_start(TimeToApp* app, bool link_mode);

static bool tracker_input(InputEvent* event, void* ctx) {
    TimeToApp* app = ctx;
    if(event->type != InputTypeShort) return false;
    switch(event->key) {
    case InputKeyDown:
        nfc_scan_start(app, false);
        return true;
    case InputKeyOk:
    case InputKeyUp:
        picker_rebuild(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewPicker);
        return true;
    case InputKeyRight:
        app->summary_day_offset = 0;
        summary_refresh(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSummary);
        return true;
    case InputKeyLeft:
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewManage);
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Switching                                                                */
/* ------------------------------------------------------------------------ */

static void stop_tracking(TimeToApp* app) {
    if(!app->tracking) return;
    append_log(app, app->current_start, now_ts(), app->current);
    app->tracking = false;
    app->current[0] = '\0';
    save_current(app);
    refresh_today_prev(app);
    tracker_update_model(app);
}

static void switch_activity(TimeToApp* app, const char* name) {
    if(app->tracking && strcmp(app->current, name) == 0) return;
    uint32_t now = now_ts();
    if(app->tracking) {
        append_log(app, app->current_start, now, app->current);
    }
    strlcpy(app->current, name, NAME_LEN);
    app->current_start = now;
    app->tracking = true;
    save_current(app);
    refresh_today_prev(app);
    tracker_update_model(app);
    notification_message(app->notifications, &sequence_single_vibro);
}

/* ------------------------------------------------------------------------ */
/* Change start time                                                        */
/* ------------------------------------------------------------------------ */

/* The previous interval moves with the start only if it ended exactly where the
 * running one began (i.e. it was ended by a switch, not by "Stop tracking"). */
static bool start_edit_prev_linked(TimeToApp* app) {
    return app->edit_last.found && app->edit_last.end == app->current_start &&
           app->edit_last.start < app->current_start;
}

static void start_edit_open(TimeToApp* app) {
    read_last_log(app, &app->edit_last);
    bool linked = start_edit_prev_linked(app);

    /* Earliest start: keep at least a minute of the linked previous interval,
     * otherwise don't overlap the last logged interval. */
    uint32_t min = 0;
    if(linked) {
        min = app->edit_last.start + 60;
    } else if(app->edit_last.found) {
        min = app->edit_last.end;
    }
    if(min > app->current_start) min = app->current_start;

    with_view_model(
        app->start_edit_view,
        StartEditModel * m,
        {
            strlcpy(m->name, app->current, NAME_LEN);
            m->start = app->current_start;
            m->min = min;
            m->now = now_ts();
            m->has_prev = linked;
            strlcpy(m->prev_name, linked ? app->edit_last.name : "", NAME_LEN);
            m->prev_start = linked ? app->edit_last.start : 0;
            m->field = 0;
        },
        true);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewStartEdit);
}

static void start_edit_apply(TimeToApp* app, uint32_t new_start) {
    if(!app->tracking || new_start == app->current_start) return;
    uint32_t now = now_ts();
    if(new_start > now) new_start = now;

    if(start_edit_prev_linked(app)) {
        if(new_start <= app->edit_last.start) return;
        if(!rewrite_last_log_end(app, &app->edit_last, new_start)) return;
    } else if(app->edit_last.found && new_start < app->edit_last.end) {
        new_start = app->edit_last.end;
    }

    app->current_start = new_start;
    save_current(app);
    refresh_today_prev(app);
    tracker_update_model(app);
    notification_message(app->notifications, &sequence_single_vibro);
}

static void start_edit_draw(Canvas* canvas, void* ctx) {
    StartEditModel* m = ctx;
    char buf[32];
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "Start time");

    canvas_set_font(canvas, FontSecondary);
    uint32_t start_day = m->start / SECONDS_PER_DAY;
    uint32_t today = m->now / SECONDS_PER_DAY;
    if(start_day == today) {
        strlcpy(buf, "Today", sizeof(buf));
    } else if(start_day + 1 == today) {
        strlcpy(buf, "Yesterday", sizeof(buf));
    } else {
        format_date(m->start, buf, sizeof(buf));
    }
    canvas_draw_str_aligned(canvas, 127, 1, AlignRight, AlignTop, buf);

    /* HH:MM with the selected field underlined */
    uint32_t tod = m->start % SECONDS_PER_DAY;
    char hh[4];
    char mm[4];
    snprintf(hh, sizeof(hh), "%02lu", (unsigned long)(tod / 3600));
    snprintf(mm, sizeof(mm), "%02lu", (unsigned long)((tod % 3600) / 60));
    snprintf(buf, sizeof(buf), "%s:%s", hh, mm);
    canvas_set_font(canvas, FontBigNumbers);
    int w = canvas_string_width(canvas, buf);
    int x = 64 - w / 2;
    canvas_draw_str(canvas, x, 27, buf);
    int hw = canvas_string_width(canvas, hh);
    if(m->field == 0) {
        canvas_draw_box(canvas, x, 29, hw, 2);
    } else {
        snprintf(buf, sizeof(buf), "%s:", hh);
        int mx = x + canvas_string_width(canvas, buf);
        canvas_draw_box(canvas, mx, 29, canvas_string_width(canvas, mm), 2);
    }

    canvas_set_font(canvas, FontSecondary);
    char dur[16];
    format_duration(m->now > m->start ? m->now - m->start : 0, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "%s: %s", m->name, dur);
    canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignTop, buf);
    if(m->has_prev) {
        format_duration(m->start > m->prev_start ? m->start - m->prev_start : 0, dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "before: %s %s", m->prev_name, dur);
        canvas_draw_str_aligned(canvas, 64, 43, AlignCenter, AlignTop, buf);
    }

    elements_button_center(canvas, "Save");
}

static bool start_edit_input(InputEvent* event, void* ctx) {
    TimeToApp* app = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyOk) {
        if(event->type != InputTypeShort) return true;
        uint32_t start = 0;
        with_view_model(app->start_edit_view, StartEditModel * m, { start = m->start; }, false);
        start_edit_apply(app, start);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
        return true;
    }

    bool handled = true;
    with_view_model(
        app->start_edit_view,
        StartEditModel * m,
        {
            m->now = now_ts();
            int64_t step = m->field == 0 ? 3600 : 60;
            switch(event->key) {
            case InputKeyLeft:
                m->field = 0;
                break;
            case InputKeyRight:
                m->field = 1;
                break;
            case InputKeyUp:
            case InputKeyDown: {
                /* move in whole minutes: drop the seconds before stepping */
                int64_t v = (int64_t)(m->start - m->start % 60);
                if(event->key == InputKeyUp) {
                    v += step;
                } else {
                    if(m->start % 60) v += 60;
                    v -= step;
                }
                if(v > (int64_t)m->now) v = m->now;
                if(v < (int64_t)m->min) v = m->min;
                m->start = (uint32_t)v;
                break;
            }
            default:
                handled = false;
                break;
            }
        },
        true);
    return handled;
}

/* ------------------------------------------------------------------------ */
/* Picker                                                                   */
/* ------------------------------------------------------------------------ */

static void picker_cb(void* ctx, uint32_t index) {
    TimeToApp* app = ctx;
    if(index < app->activity_count) {
        switch_activity(app, app->activities[index]);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
}

static void picker_rebuild(TimeToApp* app) {
    submenu_reset(app->picker);
    submenu_set_header(app->picker, "Switch to");
    uint32_t selected = 0;
    char label[NAME_LEN + 4];
    for(size_t i = 0; i < app->activity_count; i++) {
        bool is_current = app->tracking && strcmp(app->activities[i], app->current) == 0;
        snprintf(label, sizeof(label), "%s%s", is_current ? "> " : "", app->activities[i]);
        submenu_add_item(app->picker, label, i, picker_cb, app);
        if(is_current) selected = i;
    }
    submenu_set_selected_item(app->picker, selected);
}

/* ------------------------------------------------------------------------ */
/* NFC                                                                      */
/* ------------------------------------------------------------------------ */

/* Runs on the NFC worker thread. */
static NfcCommand nfc_poller_cb(NfcGenericEvent event, void* ctx) {
    TimeToApp* app = ctx;
    furi_assert(event.protocol == NfcProtocolIso14443_3a);
    const Iso14443_3aPollerEvent* ev = event.event_data;
    if(ev->type != Iso14443_3aPollerEventTypeReady) {
        return NfcCommandContinue; /* no tag yet, keep polling */
    }
    const Iso14443_3aData* data = (const Iso14443_3aData*)nfc_poller_get_data(app->nfc_poller);
    size_t len = 0;
    const uint8_t* uid = iso14443_3a_get_uid(data, &len);
    if(len > sizeof(app->nfc_uid)) len = sizeof(app->nfc_uid);
    memcpy(app->nfc_uid, uid, len);
    app->nfc_uid_len = len;
    view_dispatcher_send_custom_event(app->view_dispatcher, EventNfcTag);
    return NfcCommandStop;
}

/* Safe to call when not scanning. Must run on the app (GUI) thread. */
static void nfc_scan_stop(TimeToApp* app) {
    if(!app->nfc_scanning) return;
    app->nfc_scanning = false;
    nfc_poller_stop(app->nfc_poller);
    nfc_poller_free(app->nfc_poller);
    app->nfc_poller = NULL;
    nfc_free(app->nfc);
    app->nfc = NULL;
    notification_message(app->notifications, &sequence_blink_stop);
}

static void nfc_scan_start(TimeToApp* app, bool link_mode) {
    nfc_scan_stop(app);
    app->nfc_link_mode = link_mode;
    app->nfc_uid_len = 0;
    with_view_model(
        app->nfc_scan_view, NfcScanModel * model, { model->link_mode = link_mode; }, true);
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewNfcScan);

    app->nfc = nfc_alloc();
    app->nfc_poller = nfc_poller_alloc(app->nfc, NfcProtocolIso14443_3a);
    app->nfc_scanning = true;
    nfc_poller_start(app->nfc_poller, nfc_poller_cb, app);
    notification_message(app->notifications, &sequence_blink_start_cyan);
}

static void nfc_scan_draw(Canvas* canvas, void* ctx) {
    NfcScanModel* m = ctx;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64, 2, AlignCenter, AlignTop, m->link_mode ? "Link NFC tag" : "Scan NFC tag");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignTop, "Hold a tag to the back");
    canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignTop, "of the Flipper");
    canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignTop, "Back to cancel");
}

static void nfc_scan_exit(void* ctx) {
    nfc_scan_stop(ctx);
}

static void nfc_link_cb(void* ctx, uint32_t index) {
    TimeToApp* app = ctx;
    if(index < app->activity_count) {
        const char* name = app->activities[index];
        if(set_tag(app, app->scanned_uid, name)) {
            save_tags(app);
        }
        if(app->tracking && strcmp(app->current, name) == 0) {
            notification_message(app->notifications, &sequence_single_vibro);
        } else {
            switch_activity(app, name);
        }
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
}

static void nfc_link_rebuild(TimeToApp* app, const char* header) {
    submenu_reset(app->nfc_link_menu);
    submenu_set_header(app->nfc_link_menu, header);
    uint32_t selected = 0;
    int tag = find_tag(app, app->scanned_uid);
    for(size_t i = 0; i < app->activity_count; i++) {
        submenu_add_item(app->nfc_link_menu, app->activities[i], i, nfc_link_cb, app);
        if(tag >= 0 && strcmp(app->tags[tag].name, app->activities[i]) == 0) selected = i;
    }
    submenu_set_selected_item(app->nfc_link_menu, selected);
}

static void nfc_handle_tag(TimeToApp* app) {
    if(!app->nfc_scanning) return; /* scan was cancelled meanwhile */
    nfc_scan_stop(app);

    app->scanned_uid[0] = '\0';
    for(size_t i = 0; i < app->nfc_uid_len; i++) {
        snprintf(app->scanned_uid + i * 2, 3, "%02X", app->nfc_uid[i]);
    }
    if(app->scanned_uid[0] == '\0') {
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
        return;
    }

    int tag = find_tag(app, app->scanned_uid);
    /* A tag whose activity was deleted is treated like a new tag. */
    bool known = tag >= 0 && find_activity(app, app->tags[tag].name) >= 0;

    if(known && !app->nfc_link_mode) {
        const char* name = app->tags[tag].name;
        if(app->tracking && strcmp(app->current, name) == 0) {
            notification_message(app->notifications, &sequence_single_vibro);
        } else {
            switch_activity(app, name);
        }
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
        return;
    }

    if(app->activity_count == 0) {
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
        return;
    }
    nfc_link_rebuild(app, known ? "Link tag to" : "New tag, link to");
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewNfcLink);
}

static void nfc_unlink_cb(void* ctx, uint32_t index) {
    TimeToApp* app = ctx;
    if(index < app->tag_count) {
        for(size_t i = index; i + 1 < app->tag_count; i++) {
            app->tags[i] = app->tags[i + 1];
        }
        app->tag_count--;
        save_tags(app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewManage);
}

static void nfc_unlink_rebuild(TimeToApp* app) {
    submenu_reset(app->nfc_unlink_menu);
    submenu_set_header(app->nfc_unlink_menu, "Unlink NFC tag");
    char label[NAME_LEN + UID_STR_LEN + 4];
    for(size_t i = 0; i < app->tag_count; i++) {
        snprintf(label, sizeof(label), "%s %s", app->tags[i].name, app->tags[i].uid);
        submenu_add_item(app->nfc_unlink_menu, label, i, nfc_unlink_cb, app);
    }
}

/* ------------------------------------------------------------------------ */
/* Summary view                                                             */
/* ------------------------------------------------------------------------ */

static void summary_refresh(TimeToApp* app) {
    uint32_t now = now_ts();
    uint32_t today_start = now - (now % SECONDS_PER_DAY);
    uint32_t from = today_start - app->summary_day_offset * SECONDS_PER_DAY;
    uint32_t to = from + SECONDS_PER_DAY;

    size_t count = compute_totals(app, from, to, true);

    /* sort descending by time */
    for(size_t i = 1; i < count; i++) {
        for(size_t j = i; j > 0 && app->tmp_secs[j] > app->tmp_secs[j - 1]; j--) {
            uint32_t s = app->tmp_secs[j];
            app->tmp_secs[j] = app->tmp_secs[j - 1];
            app->tmp_secs[j - 1] = s;
            char n[NAME_LEN];
            strlcpy(n, app->tmp_names[j], NAME_LEN);
            strlcpy(app->tmp_names[j], app->tmp_names[j - 1], NAME_LEN);
            strlcpy(app->tmp_names[j - 1], n, NAME_LEN);
        }
    }

    with_view_model(
        app->summary_view,
        SummaryModel * model,
        {
            if(app->summary_day_offset == 0) {
                strlcpy(model->title, "Today", sizeof(model->title));
            } else if(app->summary_day_offset == 1) {
                strlcpy(model->title, "Yesterday", sizeof(model->title));
            } else {
                format_date(from, model->title, sizeof(model->title));
            }
            model->count = count;
            model->scroll = 0;
            model->total = 0;
            for(size_t i = 0; i < count; i++) {
                strlcpy(model->names[i], app->tmp_names[i], NAME_LEN);
                model->secs[i] = app->tmp_secs[i];
                model->total += app->tmp_secs[i];
            }
        },
        true);
}

static void summary_draw(Canvas* canvas, void* ctx) {
    SummaryModel* m = ctx;
    char buf[24];
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 0, 0, AlignLeft, AlignTop, "<");
    canvas_draw_str_aligned(canvas, 8, 0, AlignLeft, AlignTop, m->title);
    format_duration(m->total, buf, sizeof(buf));
    canvas_draw_str_aligned(canvas, 119, 0, AlignRight, AlignTop, buf);
    canvas_draw_str_aligned(canvas, 127, 0, AlignRight, AlignTop, ">");
    canvas_draw_line(canvas, 0, 11, 127, 11);

    canvas_set_font(canvas, FontSecondary);
    if(m->count == 0) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Nothing tracked");
        return;
    }

    uint32_t max = m->secs[0] ? m->secs[0] : 1;
    for(size_t row = 0; row < SUMMARY_ROWS; row++) {
        size_t i = m->scroll + row;
        if(i >= m->count) break;
        int y = 14 + (int)row * 12;
        /* proportional bar under the text */
        int bar = (int)((uint64_t)m->secs[i] * 120 / max);
        canvas_draw_box(canvas, 0, y + 9, bar, 2);
        canvas_draw_str_aligned(canvas, 0, y, AlignLeft, AlignTop, m->names[i]);
        format_duration(m->secs[i], buf, sizeof(buf));
        canvas_draw_str_aligned(canvas, 121, y, AlignRight, AlignTop, buf);
    }
    if(m->count > SUMMARY_ROWS) {
        elements_scrollbar_pos(canvas, 127, 13, 51, (uint16_t)m->scroll, (uint16_t)(m->count - SUMMARY_ROWS + 1));
    }
}

static bool summary_input(InputEvent* event, void* ctx) {
    TimeToApp* app = ctx;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    switch(event->key) {
    case InputKeyLeft:
        if(app->summary_day_offset < 3650) app->summary_day_offset++;
        summary_refresh(app);
        return true;
    case InputKeyRight:
        if(app->summary_day_offset > 0) {
            app->summary_day_offset--;
            summary_refresh(app);
        }
        return true;
    case InputKeyUp:
        with_view_model(
            app->summary_view, SummaryModel * m, { if(m->scroll > 0) m->scroll--; }, true);
        return true;
    case InputKeyDown:
        with_view_model(
            app->summary_view,
            SummaryModel * m,
            {
                if(m->scroll + SUMMARY_ROWS < m->count) m->scroll++;
            },
            true);
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Manage / delete / add                                                    */
/* ------------------------------------------------------------------------ */

static void delete_rebuild(TimeToApp* app);

static void text_input_done(void* ctx) {
    TimeToApp* app = ctx;
    if(add_activity(app, app->input_buf)) {
        save_activities(app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewManage);
}

static void delete_cb(void* ctx, uint32_t index) {
    TimeToApp* app = ctx;
    if(index < app->activity_count) {
        for(size_t i = index; i + 1 < app->activity_count; i++) {
            strlcpy(app->activities[i], app->activities[i + 1], NAME_LEN);
        }
        app->activity_count--;
        save_activities(app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewManage);
}

static void delete_rebuild(TimeToApp* app) {
    submenu_reset(app->delete_menu);
    submenu_set_header(app->delete_menu, "Delete activity");
    for(size_t i = 0; i < app->activity_count; i++) {
        submenu_add_item(app->delete_menu, app->activities[i], i, delete_cb, app);
    }
}

static void manage_cb(void* ctx, uint32_t index) {
    TimeToApp* app = ctx;
    switch(index) {
    case ManageStartTime:
        if(!app->tracking) break;
        start_edit_open(app);
        break;
    case ManageAdd:
        app->input_buf[0] = '\0';
        text_input_reset(app->text_input);
        text_input_set_header_text(app->text_input, "New activity name");
        text_input_set_result_callback(
            app->text_input, text_input_done, app, app->input_buf, NAME_LEN, true);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTextInput);
        break;
    case ManageDelete:
        delete_rebuild(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDelete);
        break;
    case ManageNfcLink:
        nfc_scan_start(app, true);
        break;
    case ManageNfcUnlink:
        if(app->tag_count == 0) break;
        nfc_unlink_rebuild(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewNfcUnlink);
        break;
    case ManageStop:
        stop_tracking(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------ */
/* Navigation, ticks                                                        */
/* ------------------------------------------------------------------------ */

static uint32_t nav_exit(void* ctx) {
    UNUSED(ctx);
    return VIEW_NONE;
}

static uint32_t nav_tracker(void* ctx) {
    UNUSED(ctx);
    return ViewTracker;
}

static uint32_t nav_manage(void* ctx) {
    UNUSED(ctx);
    return ViewManage;
}

static bool custom_event_cb(void* ctx, uint32_t event) {
    TimeToApp* app = ctx;
    if(event == EventTick) {
        if(now_ts() / SECONDS_PER_DAY != app->today_day) {
            refresh_today_prev(app);
        }
        tracker_update_model(app);
        with_view_model(
            app->start_edit_view, StartEditModel * m, { m->now = now_ts(); }, true);
        return true;
    }
    if(event == EventNfcTag) {
        nfc_handle_tag(app);
        return true;
    }
    return false;
}

static bool back_event_cb(void* ctx) {
    UNUSED(ctx);
    return false; /* let previous callbacks handle navigation */
}

static void timer_cb(void* ctx) {
    TimeToApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, EventTick);
}

/* ------------------------------------------------------------------------ */
/* Lifecycle                                                                */
/* ------------------------------------------------------------------------ */

static TimeToApp* timeto_alloc(void) {
    TimeToApp* app = malloc(sizeof(TimeToApp));
    memset(app, 0, sizeof(TimeToApp));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    storage_ensure_dir(app);
    load_activities(app);
    load_current(app);
    load_tags(app);
    refresh_today_prev(app);

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, custom_event_cb);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, back_event_cb);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    /* Tracker */
    app->tracker_view = view_alloc();
    view_allocate_model(app->tracker_view, ViewModelTypeLocking, sizeof(TrackerModel));
    view_set_context(app->tracker_view, app);
    view_set_draw_callback(app->tracker_view, tracker_draw);
    view_set_input_callback(app->tracker_view, tracker_input);
    view_set_previous_callback(app->tracker_view, nav_exit);
    view_dispatcher_add_view(app->view_dispatcher, ViewTracker, app->tracker_view);

    /* Summary */
    app->summary_view = view_alloc();
    view_allocate_model(app->summary_view, ViewModelTypeLocking, sizeof(SummaryModel));
    view_set_context(app->summary_view, app);
    view_set_draw_callback(app->summary_view, summary_draw);
    view_set_input_callback(app->summary_view, summary_input);
    view_set_previous_callback(app->summary_view, nav_tracker);
    view_dispatcher_add_view(app->view_dispatcher, ViewSummary, app->summary_view);

    /* Picker */
    app->picker = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->picker), nav_tracker);
    view_dispatcher_add_view(app->view_dispatcher, ViewPicker, submenu_get_view(app->picker));

    /* Manage */
    app->manage = submenu_alloc();
    submenu_set_header(app->manage, "Edit");
    submenu_add_item(app->manage, "Change start time", ManageStartTime, manage_cb, app);
    submenu_add_item(app->manage, "Add activity", ManageAdd, manage_cb, app);
    submenu_add_item(app->manage, "Delete activity", ManageDelete, manage_cb, app);
    submenu_add_item(app->manage, "Link NFC tag", ManageNfcLink, manage_cb, app);
    submenu_add_item(app->manage, "Unlink NFC tag", ManageNfcUnlink, manage_cb, app);
    submenu_add_item(app->manage, "Stop tracking", ManageStop, manage_cb, app);
    view_set_previous_callback(submenu_get_view(app->manage), nav_tracker);
    view_dispatcher_add_view(app->view_dispatcher, ViewManage, submenu_get_view(app->manage));

    /* Delete */
    app->delete_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->delete_menu), nav_manage);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewDelete, submenu_get_view(app->delete_menu));

    /* Text input */
    app->text_input = text_input_alloc();
    view_set_previous_callback(text_input_get_view(app->text_input), nav_manage);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewTextInput, text_input_get_view(app->text_input));

    /* NFC scan */
    app->nfc_scan_view = view_alloc();
    view_allocate_model(app->nfc_scan_view, ViewModelTypeLocking, sizeof(NfcScanModel));
    view_set_context(app->nfc_scan_view, app);
    view_set_draw_callback(app->nfc_scan_view, nfc_scan_draw);
    view_set_exit_callback(app->nfc_scan_view, nfc_scan_exit);
    view_set_previous_callback(app->nfc_scan_view, nav_tracker);
    view_dispatcher_add_view(app->view_dispatcher, ViewNfcScan, app->nfc_scan_view);

    /* NFC link: pick the activity for a scanned tag */
    app->nfc_link_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->nfc_link_menu), nav_tracker);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewNfcLink, submenu_get_view(app->nfc_link_menu));

    /* NFC unlink */
    app->nfc_unlink_menu = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->nfc_unlink_menu), nav_manage);
    view_dispatcher_add_view(
        app->view_dispatcher, ViewNfcUnlink, submenu_get_view(app->nfc_unlink_menu));

    /* Change start time */
    app->start_edit_view = view_alloc();
    view_allocate_model(app->start_edit_view, ViewModelTypeLocking, sizeof(StartEditModel));
    view_set_context(app->start_edit_view, app);
    view_set_draw_callback(app->start_edit_view, start_edit_draw);
    view_set_input_callback(app->start_edit_view, start_edit_input);
    view_set_previous_callback(app->start_edit_view, nav_manage);
    view_dispatcher_add_view(app->view_dispatcher, ViewStartEdit, app->start_edit_view);

    tracker_update_model(app);

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(1000));

    return app;
}

static void timeto_free(TimeToApp* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    nfc_scan_stop(app);

    view_dispatcher_remove_view(app->view_dispatcher, ViewTracker);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSummary);
    view_dispatcher_remove_view(app->view_dispatcher, ViewPicker);
    view_dispatcher_remove_view(app->view_dispatcher, ViewManage);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDelete);
    view_dispatcher_remove_view(app->view_dispatcher, ViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, ViewNfcScan);
    view_dispatcher_remove_view(app->view_dispatcher, ViewNfcLink);
    view_dispatcher_remove_view(app->view_dispatcher, ViewNfcUnlink);
    view_dispatcher_remove_view(app->view_dispatcher, ViewStartEdit);

    view_free(app->tracker_view);
    view_free(app->summary_view);
    submenu_free(app->picker);
    submenu_free(app->manage);
    submenu_free(app->delete_menu);
    text_input_free(app->text_input);
    view_free(app->nfc_scan_view);
    submenu_free(app->nfc_link_menu);
    submenu_free(app->nfc_unlink_menu);
    view_free(app->start_edit_view);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t timeto_app(void* p) {
    UNUSED(p);
    TimeToApp* app = timeto_alloc();
    /* Open the picker straight away on first use so tracking can start immediately. */
    if(!app->tracking) {
        picker_rebuild(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewPicker);
    } else {
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewTracker);
    }
    view_dispatcher_run(app->view_dispatcher);
    timeto_free(app);
    return 0;
}
