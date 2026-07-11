/*
 * NMEA GPS Replay - Flipper Zero FAP
 *
 * Streams a captured GPS RS232 log out the USART TX (pin 13) at 115200 8N1,
 * feeding an ESP32 UART RX (GPIO18) as if it were the real RS232->TTL GPS.
 *
 * The capture format is:  "HH:MM:SS.mmm, <exact GPS bytes>\n"
 * The 14-byte terminal timestamp prefix is stripped; everything after it is the
 * verbatim GPS stream (NMEA + ###IMEI + binary I/O frames) and is sent as-is.
 * The timestamp is used only to reproduce the real inter-line pacing (~1 Hz),
 * so the ESP sees the same real-time cadence as when driving.
 *
 * Power for the ESP comes from the Flipper: pin 1 (5V, OTG) + pin 8 (GND).
 * Wiring:  Flipper pin13 (TX) -> ESP GPIO18,  pin1 (5V) -> ESP 5V,  pin8 -> GND.
 */

#include <furi.h>
#include <furi_hal_serial.h>
#include <furi_hal_serial_control.h>
#include <furi_hal_power.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>

#define TAG "NmeaReplay"

#define UART_BAUD 115200u
#define READ_CHUNK 512u
#define LINE_MAX 256u
#define PREFIX_LEN 14u /* "HH:MM:SS.mmm, " */
#define MAX_GAP_MS 2000u /* clamp inter-line delay */
#define SLICE_MS 25u /* sleep granularity so Back stays responsive */

#define LOG_FOLDER "/ext"
#define LOG_EXT ".log"

typedef struct {
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;

    /* model (updated by the replay loop, read by the draw callback) */
    char file_name[64];
    uint32_t lines_sent;
    uint32_t bytes_sent;
    uint32_t loops;
    uint32_t elapsed_s;
    bool running;
} NmeaReplayApp;

/* ---------------------------------------------------------------- GUI ---- */

static void nmea_replay_draw_callback(Canvas* canvas, void* ctx) {
    NmeaReplayApp* app = ctx;
    char buf[64];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, "NMEA GPS Replay");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 2, 24, app->file_name);

    snprintf(buf, sizeof(buf), "lines: %lu  loop: %lu", app->lines_sent, app->loops);
    canvas_draw_str(canvas, 2, 36, buf);

    snprintf(buf, sizeof(buf), "bytes: %lu  t: %lus", app->bytes_sent, app->elapsed_s);
    canvas_draw_str(canvas, 2, 48, buf);

    canvas_draw_str(canvas, 2, 62, app->running ? "TX 115200  Back=stop" : "done - Back=exit");
}

static void nmea_replay_input_callback(InputEvent* input_event, void* ctx) {
    NmeaReplayApp* app = ctx;
    furi_message_queue_put(app->input_queue, input_event, FuriWaitForever);
}

/* Returns true if the user pressed Back (short) since last check. */
static bool nmea_replay_back_pressed(NmeaReplayApp* app) {
    InputEvent event;
    while(furi_message_queue_get(app->input_queue, &event, 0) == FuriStatusOk) {
        if(event.type == InputTypeShort && event.key == InputKeyBack) {
            return true;
        }
    }
    return false;
}

/* Sleep `ms` in small slices; returns true if Back was pressed during the wait. */
static bool nmea_replay_sleep(NmeaReplayApp* app, uint32_t ms) {
    while(ms > 0) {
        uint32_t slice = ms < SLICE_MS ? ms : SLICE_MS;
        furi_delay_ms(slice);
        ms -= slice;
        if(nmea_replay_back_pressed(app)) return true;
    }
    return false;
}

/* --------------------------------------------------------- log parsing --- */

/* Parse the "HH:MM:SS.mmm" prefix into milliseconds-of-day.
 * Returns true and sets *ts_ms if the line has a valid timestamp prefix. */
static bool nmea_replay_parse_ts(const char* l, size_t len, uint32_t* ts_ms) {
    if(len < PREFIX_LEN) return false;
    if(l[2] != ':' || l[5] != ':' || l[8] != '.' || l[12] != ',') return false;
    for(int i = 0; i < 12; i++) {
        if(i == 2 || i == 5 || i == 8) continue; /* separators */
        if(l[i] < '0' || l[i] > '9') return false;
    }
    uint32_t hh = (l[0] - '0') * 10 + (l[1] - '0');
    uint32_t mm = (l[3] - '0') * 10 + (l[4] - '0');
    uint32_t ss = (l[6] - '0') * 10 + (l[7] - '0');
    uint32_t ms = (l[9] - '0') * 100 + (l[10] - '0') * 10 + (l[11] - '0');
    *ts_ms = ((hh * 60 + mm) * 60 + ss) * 1000 + ms;
    return true;
}

/* Transmit one captured line: strip the 14-byte prefix, send the exact GPS
 * bytes plus the terminating '\n'. */
static void nmea_replay_tx_line(
    NmeaReplayApp* app,
    FuriHalSerialHandle* serial,
    const char* line,
    size_t line_len) {
    const uint8_t* payload = (const uint8_t*)(line + PREFIX_LEN);
    size_t payload_len = line_len - PREFIX_LEN;
    static const uint8_t nl = '\n';

    if(payload_len > 0) {
        furi_hal_serial_tx(serial, payload, payload_len);
    }
    furi_hal_serial_tx(serial, &nl, 1);
    furi_hal_serial_tx_wait_complete(serial);

    app->lines_sent++;
    app->bytes_sent += payload_len + 1;
}

/* Replays `path` once. Returns false if the user aborted (Back). */
static bool nmea_replay_play_once(
    NmeaReplayApp* app,
    Storage* storage,
    FuriHalSerialHandle* serial,
    const char* path,
    uint32_t start_tick) {
    File* file = storage_file_alloc(storage);
    bool aborted = false;

    if(!storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        FURI_LOG_E(TAG, "open failed: %s", path);
        storage_file_free(file);
        return false; /* treat as abort so caller stops */
    }

    uint8_t chunk[READ_CHUNK];
    char line[LINE_MAX];
    size_t line_len = 0;
    bool have_prev_ts = false;
    uint32_t prev_ts = 0;
    size_t n;

    while((n = storage_file_read(file, chunk, READ_CHUNK)) > 0) {
        for(size_t i = 0; i < n; i++) {
            uint8_t c = chunk[i];
            if(c == '\n') {
                /* complete line (without the '\n') */
                uint32_t ts;
                if(nmea_replay_parse_ts(line, line_len, &ts)) {
                    /* pace by real inter-line delta */
                    if(have_prev_ts) {
                        uint32_t delay = (ts >= prev_ts) ? (ts - prev_ts) : 0;
                        if(delay > MAX_GAP_MS) delay = MAX_GAP_MS;
                        if(nmea_replay_sleep(app, delay)) {
                            aborted = true;
                        }
                    }
                    prev_ts = ts;
                    have_prev_ts = true;

                    if(!aborted) {
                        nmea_replay_tx_line(app, serial, line, line_len);
                        app->elapsed_s = (furi_get_tick() - start_tick) / furi_kernel_get_tick_frequency();
                        view_port_update(app->view_port);
                    }
                }
                /* lines without a valid prefix (header) are skipped */
                line_len = 0;
                if(aborted) goto done;
            } else {
                if(line_len < LINE_MAX - 1) {
                    line[line_len++] = (char)c;
                }
                /* overlong lines: keep first LINE_MAX-1 bytes, drop the rest */
            }
        }
        if(nmea_replay_back_pressed(app)) {
            aborted = true;
            goto done;
        }
    }

done:
    storage_file_close(file);
    storage_file_free(file);
    return !aborted;
}

/* ---------------------------------------------------------------- main --- */

int32_t nmea_replay_app(void* p) {
    UNUSED(p);
    NmeaReplayApp* app = malloc(sizeof(NmeaReplayApp));
    memset(app, 0, sizeof(NmeaReplayApp));

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, nmea_replay_draw_callback, app);
    view_port_input_callback_set(app->view_port, nmea_replay_input_callback, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);

    /* --- pick a .log via the file browser --- */
    FuriString* path = furi_string_alloc_set(LOG_FOLDER);
    DialogsFileBrowserOptions browser;
    dialog_file_browser_set_basic_options(&browser, LOG_EXT, NULL);
    browser.base_path = LOG_FOLDER;
    bool picked = dialog_file_browser_show(dialogs, path, path, &browser);

    if(picked) {
        /* short name for the header */
        const char* full = furi_string_get_cstr(path);
        const char* slash = strrchr(full, '/');
        strncpy(app->file_name, slash ? slash + 1 : full, sizeof(app->file_name) - 1);

        /* --- power the ESP + open the serial line --- */
        furi_hal_power_enable_otg(); /* 5V on pin 1 */
        FuriHalSerialHandle* serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(serial) {
            furi_hal_serial_init(serial, UART_BAUD);

            app->running = true;
            view_port_update(app->view_port);
            uint32_t start_tick = furi_get_tick();

            /* --- continuous loop until Back --- */
            while(app->running) {
                bool ok = nmea_replay_play_once(app, storage, serial, full, start_tick);
                if(!ok) {
                    app->running = false; /* aborted or open error */
                } else {
                    app->loops++;
                    view_port_update(app->view_port);
                }
            }

            furi_hal_serial_deinit(serial);
            furi_hal_serial_control_release(serial);
        } else {
            FURI_LOG_E(TAG, "serial acquire failed");
        }
        furi_hal_power_disable_otg();

        /* let the user read the final counters until Back */
        view_port_update(app->view_port);
        InputEvent event;
        while(furi_message_queue_get(app->input_queue, &event, FuriWaitForever) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) break;
        }
    }

    /* --- teardown --- */
    furi_string_free(path);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    gui_remove_view_port(app->gui, app->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);
    free(app);
    return 0;
}
