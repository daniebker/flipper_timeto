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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "TimeTo"

#define TIMETO_DIR       EXT_PATH("apps_data/timeto")
#define ACTIVITIES_PATH  TIMETO_DIR "/activities.txt"
#define CURRENT_PATH     TIMETO_DIR "/current.txt"
#define LOG_PATH         TIMETO_DIR "/log.csv"
#define LOG_HEADER       "start_ts,end_ts,duration_s,start,end,activity\n"

#define MAX_ACTIVITIES   24
#define NAME_LEN         24
#define SECONDS_PER_DAY  86400UL
#define SUMMARY_ROWS     4
#define SUMMARY_MAX      (MAX_ACTIVITIES + 4)

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
} TimeToView;

typedef enum {
    EventTick = 1,
} TimeToEvent;

typedef enum {
    ManageAdd,
    ManageDelete,
    ManageStop,
} ManageItem;

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
    FuriTimer* timer;

    char activities[MAX_ACTIVITIES][NAME_LEN];
    size_t activity_count;

    bool tracking;
    char current[NAME_LEN];
    uint32_t current_start;

    uint32_t today_prev;
    uint32_t today_day; /* day index (ts / 86400) that today_prev belongs to */

    uint32_t summary_day_offset; /* 0 = today, 1 = yesterday, ... */
    char input_buf[NAME_LEN];

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

static void append_log(TimeToApp* app, uint32_t start, uint32_t end, const char* name) {
    if(end <= start) return;
    char start_str[32];
    char end_str[32];
    format_datetime(start, start_str, sizeof(start_str));
    format_datetime(end, end_str, sizeof(end_str));

    FuriString* line = furi_string_alloc_printf(
        "%lu,%lu,%lu,%s,%s,%s\n",
        (unsigned long)start,
        (unsigned long)end,
        (unsigned long)(end - start),
        start_str,
        end_str,
        name);

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

/* log line: start_ts,end_ts,duration_s,start,end,activity */
static void totals_line_cb(const char* line, void* ctx) {
    TotalsCtx* t = ctx;
    if(line[0] < '0' || line[0] > '9') return; /* header or garbage */

    char* p;
    uint32_t start = strtoul(line, &p, 10);
    if(*p != ',') return;
    uint32_t end = strtoul(p + 1, NULL, 10);

    /* activity is after the 5th comma */
    const char* name = line;
    for(int i = 0; i < 5; i++) {
        name = strchr(name, ',');
        if(!name) return;
        name++;
    }

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
        canvas_draw_str_aligned(canvas, 64, 24, AlignCenter, AlignCenter, "Not tracking");
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, "Pick an activity to start");
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

static bool tracker_input(InputEvent* event, void* ctx) {
    TimeToApp* app = ctx;
    if(event->type != InputTypeShort) return false;
    switch(event->key) {
    case InputKeyOk:
    case InputKeyUp:
    case InputKeyDown:
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
    submenu_add_item(app->manage, "Add activity", ManageAdd, manage_cb, app);
    submenu_add_item(app->manage, "Delete activity", ManageDelete, manage_cb, app);
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

    tracker_update_model(app);

    app->timer = furi_timer_alloc(timer_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(1000));

    return app;
}

static void timeto_free(TimeToApp* app) {
    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);

    view_dispatcher_remove_view(app->view_dispatcher, ViewTracker);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSummary);
    view_dispatcher_remove_view(app->view_dispatcher, ViewPicker);
    view_dispatcher_remove_view(app->view_dispatcher, ViewManage);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDelete);
    view_dispatcher_remove_view(app->view_dispatcher, ViewTextInput);

    view_free(app->tracker_view);
    view_free(app->summary_view);
    submenu_free(app->picker);
    submenu_free(app->manage);
    submenu_free(app->delete_menu);
    text_input_free(app->text_input);
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
