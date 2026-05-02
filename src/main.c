/*
 * Nimbus — Weather app for NextUI
 * Apostrophe UI + libcurl + cJSON + WeatherAPI.com
 * QR-based API key setup via embedded web server.
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"
#define AP_WIDGETS_IMPLEMENTATION
#include "apostrophe_widgets.h"

#define CJSON_HIDE_SYMBOLS
#include "cJSON.h"
#include "cJSON.c"

#include "qrcodegen.h"
#include "qrcodegen.c"

#define PAKKIT_UI_IMPLEMENTATION
#include "pakkit_ui.h"

#include <string.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define NIMBUS_VERSION   "0.2.0"
#define MAX_PATH_LEN     1280
#define MAX_LOCATION     256
#define MAX_LINE         512
#define MAX_URL          1024
#define MAX_LABEL        256
#define MAX_FORECAST_DAYS 3

#define DEFAULT_BG_R     30
#define DEFAULT_BG_G     30
#define DEFAULT_BG_B     35
#define DEFAULT_TEXT_R   220
#define DEFAULT_TEXT_G   220
#define DEFAULT_TEXT_B   220
#define DEFAULT_HINT_R   140
#define DEFAULT_HINT_G   140
#define DEFAULT_HINT_B   150

#define SCROLL_STEP      20
#define SETUP_SERVER_PORT 8090

/* -----------------------------------------------------------------------
 * Data structures
 * ----------------------------------------------------------------------- */

typedef struct {
    char name[MAX_LOCATION];
    char lat_lon[64];
    int  id;
} location_t;

typedef struct {
    char   date[32];
    char   day_name[16];
    double max_temp_f;
    double max_temp_c;
    double min_temp_f;
    double min_temp_c;
    double max_wind_mph;
    double max_wind_kph;
    int    chance_rain;
    int    chance_snow;
    double total_precip_in;
    double total_precip_mm;
    int    avg_humidity;
    double uv;
    char   condition_text[MAX_LABEL];
    int    condition_code;
    char   icon_url[MAX_URL];

    char   sunrise[16];
    char   sunset[16];
    char   moonrise[16];
    char   moonset[16];
    char   moon_phase[32];

    SDL_Texture *icon_texture;
} forecast_day_t;

typedef struct {
    char location_name[MAX_LOCATION];
    char region[MAX_LOCATION];
    char country[MAX_LOCATION];

    double temp_f;
    double temp_c;
    double feels_like_f;
    double feels_like_c;
    int    humidity;
    double wind_mph;
    double wind_kph;
    char   wind_dir[16];
    char   condition_text[MAX_LABEL];
    int    condition_code;
    int    is_day;
    char   icon_url[MAX_URL];
    double precip_in;
    double precip_mm;
    int    cloud;
    double uv;
    char   last_updated[64];

    forecast_day_t forecast[MAX_FORECAST_DAYS];
    int            forecast_count;

    int    valid;
} weather_data_t;

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} fetch_buf_t;

typedef struct {
    const char  *url;
    fetch_buf_t  buf;
    int          result;
} fetch_task_t;

typedef struct {
    int use_fahrenheit;
} app_settings_t;

/* Setup server state */
typedef struct {
    int          server_fd;
    int          running;
    char         received_key[128];
    pthread_t    thread;
} setup_server_t;

/* -----------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------- */

static location_t      g_location;
static weather_data_t  g_weather;
static char            g_api_key[128]             = {0};
static char            g_config_dir[MAX_PATH_LEN] = {0};
static char            g_cache_dir[MAX_PATH_LEN]  = {0};
static app_settings_t  g_settings;
static SDL_Texture    *g_icon_texture             = NULL;
static SDL_Texture    *g_sunrise_icon             = NULL;
static SDL_Texture    *g_sunset_icon              = NULL;

/* -----------------------------------------------------------------------
 * String helpers
 * ----------------------------------------------------------------------- */

static void trim_inplace(char *s) {
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static const char *day_name_from_date(const char *date_str) {
    int y = 0, m = 0, d = 0;
    if (sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3) return "???";
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    tm_val.tm_year = y - 1900;
    tm_val.tm_mon  = m - 1;
    tm_val.tm_mday = d;
    mktime(&tm_val);
    static const char *names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    return names[tm_val.tm_wday % 7];
}

/* -----------------------------------------------------------------------
 * Settings load/save
 * ----------------------------------------------------------------------- */

static void settings_set_defaults(void) {
    g_settings.use_fahrenheit = 1;
}

static void settings_save(void) {
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/settings.txt", g_config_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "units=%s\n", g_settings.use_fahrenheit ? "F" : "C");
    fclose(f);
}

static void settings_load(void) {
    settings_set_defaults();
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/settings.txt", g_config_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line; char *val = eq + 1;
        trim_inplace(key); trim_inplace(val);
        if (strcmp(key, "units") == 0)
            g_settings.use_fahrenheit = (val[0] == 'F' || val[0] == 'f') ? 1 : 0;
    }
    fclose(f);
    ap_log("settings: loaded (units=%s)", g_settings.use_fahrenheit ? "F" : "C");
}

/* -----------------------------------------------------------------------
 * Config: API key
 * ----------------------------------------------------------------------- */

static int load_api_key(void) {
    if (g_config_dir[0] == '\0') return -1;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/api_key.txt", g_config_dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (fgets(g_api_key, sizeof(g_api_key), f))
        trim_inplace(g_api_key);
    fclose(f);
    if (g_api_key[0] == '\0') return -1;
    ap_log("config: API key loaded (%zu chars)", strlen(g_api_key));
    return 0;
}

static void save_api_key(const char *key) {
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/api_key.txt", g_config_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", key);
    fclose(f);
    ap_log("config: API key saved");
}

/* -----------------------------------------------------------------------
 * Config: Location
 * ----------------------------------------------------------------------- */

static int load_location(void) {
    if (g_config_dir[0] == '\0') return -1;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/location.txt", g_config_dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim_inplace(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char *sep = strchr(line, '|');
        if (sep) {
            *sep = '\0';
            snprintf(g_location.name, MAX_LOCATION, "%s", line);
            trim_inplace(g_location.name);
            char *rest = sep + 1;
            char *sep2 = strchr(rest, '|');
            if (sep2) {
                *sep2 = '\0';
                snprintf(g_location.lat_lon, sizeof(g_location.lat_lon), "%s", rest);
                trim_inplace(g_location.lat_lon);
                g_location.id = atoi(sep2 + 1);
            } else {
                snprintf(g_location.lat_lon, sizeof(g_location.lat_lon), "%s", rest);
                trim_inplace(g_location.lat_lon);
                g_location.id = 0;
            }
        }
        break;
    }
    fclose(f);
    ap_log("config: location = %s (id:%d)", g_location.name, g_location.id);
    return (g_location.name[0] && (g_location.lat_lon[0] || g_location.id > 0)) ? 0 : -1;
}

static void save_location(void) {
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/location.txt", g_config_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Nimbus — Location\n");
    fprintf(f, "# Format: Name|lat,lon|id\n");
    fprintf(f, "%s|%s|%d\n", g_location.name, g_location.lat_lon, g_location.id);
    fclose(f);
}

/* -----------------------------------------------------------------------
 * Network: get local IP
 * ----------------------------------------------------------------------- */

static int get_local_ip(char *out, size_t out_size) {
    struct ifaddrs *ifaddr, *ifa;
    out[0] = '\0';

    if (getifaddrs(&ifaddr) == -1) return -1;

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        /* Only use wlan0 — WiFi must be connected */
        if (strcmp(ifa->ifa_name, "wlan0") != 0) continue;

        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)out_size);
        break;
    }

    freeifaddrs(ifaddr);
    return out[0] ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Embedded setup web server
 * ----------------------------------------------------------------------- */

static const char *SETUP_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Nimbus Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:480px;margin:40px auto;text-align:center;"
    "padding:20px;background:#1e1e23;color:#dcdcdc;}"
    "h1{color:#4a90d9;font-size:1.5em;}"
    "input{width:100%%;padding:12px;font-size:16px;border:2px solid #555;"
    "border-radius:8px;background:#2a2a30;color:#dcdcdc;}"
    "input:focus{border-color:#4a90d9;outline:none;}"
    "button{width:100%%;padding:12px;font-size:16px;background:#4a90d9;"
    "color:white;border:none;border-radius:8px;cursor:pointer;margin-top:12px;}"
    "button:hover{background:#3a7bc8;}"
    "p{color:#8c8c96;font-size:14px;}"
    ".ok{color:#5cb85c;font-size:1.2em;text-align:center;padding:40px 0;}"
    "</style></head><body>"
    "<h1>Nimbus Setup</h1>"
    "<p>Enter your WeatherAPI.com API key below.</p>"
    "<form method='POST' action='/key'>"
    "<input type='text' name='key' placeholder='Your API key' autofocus>"
    "<button type='submit'>Save</button>"
    "</form>"
    "<p>Get a free key at <b>weatherapi.com</b></p>"
    "</body></html>";

static const char *SETUP_OK_HTML =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Nimbus Setup</title>"
    "<style>"
    "body{font-family:system-ui,sans-serif;max-width:480px;margin:40px auto;"
    "padding:20px;background:#1e1e23;color:#dcdcdc;text-align:center;}"
    "h1{color:#4a90d9;font-size:1.5em;}"
    "p{color:#8c8c96;font-size:14px;}"
    "</style></head><body>"
    "<h1>API key saved!</h1>"
    "<p>You can close this page.<br>Nimbus is loading...</p>"
    "</body></html>";

static void url_decode(const char *src, char *dst, size_t dst_size) {
    char *end = dst + dst_size - 1;
    while (*src && dst < end) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void extract_form_value(const char *body, const char *field,
                                char *out, size_t out_size) {
    out[0] = '\0';
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s=", field);
    const char *start = strstr(body, prefix);
    if (!start) return;
    start += strlen(prefix);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    char encoded[256] = {0};
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    memcpy(encoded, start, len);
    encoded[len] = '\0';
    url_decode(encoded, out, out_size);
}

static void *setup_server_thread(void *arg) {
    setup_server_t *srv = (setup_server_t *)arg;

    while (srv->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Use select with timeout so we can check srv->running */
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv->server_fd, &fds);
        struct timeval tv = {.tv_sec = 0,.tv_usec = 500000 }; /* 500ms */

        int sel = select(srv->server_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue;

        int client_fd = accept(srv->server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        /* Read request */
        char req_buf[4096] = {0};
        ssize_t n = read(client_fd, req_buf, sizeof(req_buf) - 1);
        if (n <= 0) { close(client_fd); continue; }
        req_buf[n] = '\0';

        /* Parse method and path */
        int is_post = (strncmp(req_buf, "POST", 4) == 0);
        int is_key_post = is_post && strstr(req_buf, "POST /key") != NULL;

        char response[8192];

        if (is_key_post) {
            /* Find body (after \r\n\r\n) */
            const char *body = strstr(req_buf, "\r\n\r\n");
            if (body) body += 4; else body = "";

            char key_val[128] = {0};
            extract_form_value(body, "key", key_val, sizeof(key_val));
            trim_inplace(key_val);

            if (key_val[0]) {
                snprintf(srv->received_key, sizeof(srv->received_key), "%s", key_val);
                snprintf(response, sizeof(response),
                         "HTTP/1.1 200 OK\r\n"
                         "Content-Type: text/html\r\n"
                         "Connection: close\r\n\r\n%s",
                         SETUP_OK_HTML);
                write(client_fd, response, strlen(response));
                close(client_fd);
                /* Signal that we got the key */
                srv->running = 0;
                break;
            }
        }

        /* Serve the form page for GET or failed POST */
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html\r\n"
                 "Connection: close\r\n\r\n%s",
                 SETUP_HTML);
        write(client_fd, response, strlen(response));
        close(client_fd);
    }

    return NULL;
}

static int setup_server_start(setup_server_t *srv) {
    memset(srv, 0, sizeof(*srv));

    srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->server_fd < 0) {
        ap_log("setup: socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SETUP_SERVER_PORT);

    if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ap_log("setup: bind() failed: %s", strerror(errno));
        close(srv->server_fd);
        return -1;
    }

    if (listen(srv->server_fd, 5) < 0) {
        ap_log("setup: listen() failed: %s", strerror(errno));
        close(srv->server_fd);
        return -1;
    }

    srv->running = 1;
    pthread_create(&srv->thread, NULL, setup_server_thread, srv);
    ap_log("setup: server started on port %d", SETUP_SERVER_PORT);
    return 0;
}

static void setup_server_stop(setup_server_t *srv) {
    srv->running = 0;
    pthread_join(srv->thread, NULL);
    close(srv->server_fd);
    ap_log("setup: server stopped");
}

/* -----------------------------------------------------------------------
 * QR code rendering
 * ----------------------------------------------------------------------- */

static void draw_qr_code(const char *text, int cx, int cy, int max_size) {
    uint8_t qr_buf[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp_buf[qrcodegen_BUFFER_LEN_MAX];

    if (!qrcodegen_encodeText(text, temp_buf, qr_buf,
                               qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                               qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        ap_log("qr: encode failed for '%s'", text);
        return;
    }

    int qr_size = qrcodegen_getSize(qr_buf);
    int module_px = max_size / (qr_size + 4); /* +4 for quiet zone */
    if (module_px < 1) module_px = 1;
    int total_px = module_px * (qr_size + 4);

    int ox = cx - total_px / 2;
    int oy = cy - total_px / 2;

    /* White background with quiet zone */
    ap_color white = {255, 255, 255, 255};
    ap_color black = {0, 0, 0, 255};
    ap_draw_rect(ox, oy, total_px, total_px, white);

    /* Draw modules */
    int quiet = module_px * 2;
    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qrcodegen_getModule(qr_buf, x, y)) {
                ap_draw_rect(ox + quiet + x * module_px,
                             oy + quiet + y * module_px,
                             module_px, module_px, black);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * API key setup screen (QR + web server)
 * ----------------------------------------------------------------------- */

static int show_api_key_setup(void) {
    /* Check WiFi is actually connected using Apostrophe's wifi detection */
    int wifi_strength = ap__get_wifi_strength();
    ap_log("setup: wifi strength = %d", wifi_strength);

    char ip[64] = {0};
    if (wifi_strength == 0 || get_local_ip(ip, sizeof(ip)) != 0) {
        pakkit_message("WiFi not connected.\n\n"
                       "Connect to WiFi and restart Nimbus,\n"
                       "or manually create:\n\n"
                       ".userdata/tg5040/nimbus/\n"
                       "  config/api_key.txt", "Quit");
        return -1;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d", ip, SETUP_SERVER_PORT);
    ap_log("setup: URL = %s", url);

    setup_server_t srv;
    if (setup_server_start(&srv) != 0) {
        pakkit_message("Could not start setup server.\n\n"
                       "Manually create:\n"
                       ".userdata/tg5040/nimbus/\n"
                       "  config/api_key.txt", "Quit");
        return -1;
    }

    int sw = ap_get_screen_width();
    int sh = ap_get_screen_height();
    int pad = AP_DS(5);

    TTF_Font *font_large = ap_get_font(AP_FONT_LARGE);
    TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
    TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
    TTF_Font *font_tiny  = ap_get_font(AP_FONT_TINY);

    ap_theme *theme = ap_get_theme();
    ap_color text_color = theme->text;
    ap_color hint_color = theme->hint;

    int result = -1;

    while (srv.running) {
        ap_input_event ev;
        while (ap_poll_input(&ev)) {
            if (ev.pressed && !ev.repeated && ev.button == AP_BTN_B) {
                srv.running = 0;
                result = -1;
            }
        }

        ap_clear_screen();
        ap_draw_background();

        /* Title — no status bar on setup screen for more space */
        int y = pad * 3;
        const char *title = "Nimbus Setup";
        int title_w = ap_measure_text(font_large, title);
        ap_draw_text(font_large, title, (sw - title_w) / 2, y, text_color);
        y += TTF_FontHeight(font_large) + pad * 2;

        /* Instruction */
        const char *inst = "Scan QR code or visit URL to enter API key";
        int inst_w = ap_measure_text(font_small, inst);
        ap_draw_text(font_small, inst, (sw - inst_w) / 2, y, hint_color);
        y += TTF_FontHeight(font_small) + pad * 3;

        /* QR code — size to fit between instruction text and footer */
        int hint_font_h = TTF_FontHeight(font_tiny);
        int footer_h = hint_font_h + pad * 2;
        int avail_h = sh - y - footer_h - TTF_FontHeight(font_med) - TTF_FontHeight(font_small) - pad * 6;
        int qr_max = avail_h;
        if (qr_max > sw / 2) qr_max = sw / 2;
        int qr_cy = y + qr_max / 2;
        draw_qr_code(url, sw / 2, qr_cy, qr_max);
        y = qr_cy + qr_max / 2 + pad * 2;

        /* URL */
        int url_w = ap_measure_text(font_med, url);
        ap_draw_text(font_med, url, (sw - url_w) / 2, y, text_color);
        y += TTF_FontHeight(font_med) + pad;

        /* Waiting message */
        const char *wait = "Waiting for API key...";
        int wait_w = ap_measure_text(font_small, wait);
        ap_draw_text(font_small, wait, (sw - wait_w) / 2, y, hint_color);

        /* Minimal hint */
        int hint_y = sh - TTF_FontHeight(font_tiny) - pad;
        ap_draw_text(font_tiny, "B: Cancel", pad * 2, hint_y, hint_color);

        ap_present();
    }

    /* Check if we got a key */
    if (srv.received_key[0]) {
        snprintf(g_api_key, sizeof(g_api_key), "%s", srv.received_key);
        trim_inplace(g_api_key);
        save_api_key(g_api_key);
        ap_log("setup: API key received and saved");
        result = 0;
    }

    setup_server_stop(&srv);
    return result;
}

/* -----------------------------------------------------------------------
 * HTTP fetch via libcurl
 * ----------------------------------------------------------------------- */

static size_t fetch_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    fetch_buf_t *buf = (fetch_buf_t *)userdata;
    size_t bytes = size * nmemb;
    while (buf->size + bytes + 1 > buf->capacity) {
        size_t new_cap = buf->capacity * 2;
        if (new_cap == 0) new_cap = 8192;
        char *new_data = realloc(buf->data, new_cap);
        if (!new_data) return 0;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, ptr, bytes);
    buf->size += bytes;
    buf->data[buf->size] = '\0';
    return bytes;
}

static int fetch_url(const char *url, fetch_buf_t *buf) {
    buf->data = NULL; buf->size = 0; buf->capacity = 0;
    CURL *curl = curl_easy_init();
    if (!curl) return -1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nimbus/0.1");
    const char *ca = getenv("CURL_CA_BUNDLE");
    if (ca) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        ap_log("fetch: curl error: %s", curl_easy_strerror(res));
        free(buf->data); buf->data = NULL; buf->size = 0;
        return -1;
    }
    ap_log("fetch: HTTP %ld, %zu bytes", http_code, buf->size);
    return 0;
}

static int fetch_worker(void *userdata) {
    fetch_task_t *task = (fetch_task_t *)userdata;
    task->result = fetch_url(task->url, &task->buf);
    return 0;
}

/* -----------------------------------------------------------------------
 * Icon fetch + cache
 * ----------------------------------------------------------------------- */

static SDL_Texture *fetch_and_load_icon(const char *icon_url) {
    if (!icon_url || !icon_url[0] || g_cache_dir[0] == '\0') return NULL;

    char full_url[MAX_URL];
    if (strncmp(icon_url, "//", 2) == 0)
        snprintf(full_url, sizeof(full_url), "https:%s", icon_url);
    else
        snprintf(full_url, sizeof(full_url), "%s", icon_url);

    const char *slash = strrchr(icon_url, '/');
    const char *fname = slash ? slash + 1 : "icon.png";
    const char *day_night = strstr(icon_url, "/day/") ? "day" : "night";
    char cache_path[MAX_PATH_LEN];
    snprintf(cache_path, sizeof(cache_path), "%s/%s_%s", g_cache_dir, day_night, fname);

    if (access(cache_path, R_OK) == 0)
        return ap_load_image(cache_path);

    fetch_buf_t buf;
    int rc = fetch_url(full_url, &buf);
    if (rc != 0 || !buf.data) return NULL;

    FILE *f = fopen(cache_path, "wb");
    if (f) { fwrite(buf.data, 1, buf.size, f); fclose(f); }
    free(buf.data);

    return ap_load_image(cache_path);
}

/* -----------------------------------------------------------------------
 * WeatherAPI.com: fetch + parse forecast
 * ----------------------------------------------------------------------- */

static int parse_weather_data(const char *json_str) {
    for (int i = 0; i < g_weather.forecast_count; i++) {
        if (g_weather.forecast[i].icon_texture) {
            SDL_DestroyTexture(g_weather.forecast[i].icon_texture);
            g_weather.forecast[i].icon_texture = NULL;
        }
    }
    memset(&g_weather, 0, sizeof(g_weather));

    cJSON *root = cJSON_Parse(json_str);
    if (!root) { ap_log("weather: JSON parse failed"); return -1; }

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        ap_log("weather: API error: %s", msg ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return -1;
    }

    cJSON *loc = cJSON_GetObjectItem(root, "location");
    if (loc) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(loc, "name")) && v->valuestring)
            strncpy(g_weather.location_name, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(loc, "region")) && v->valuestring)
            strncpy(g_weather.region, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(loc, "country")) && v->valuestring)
            strncpy(g_weather.country, v->valuestring, MAX_LOCATION - 1);
    }

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(cur, "temp_f")))       g_weather.temp_f = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "temp_c")))       g_weather.temp_c = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "feelslike_f")))   g_weather.feels_like_f = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "feelslike_c")))   g_weather.feels_like_c = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "humidity")))      g_weather.humidity = v->valueint;
        if ((v = cJSON_GetObjectItem(cur, "wind_mph")))      g_weather.wind_mph = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "wind_kph")))      g_weather.wind_kph = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "wind_dir")) && v->valuestring)
            strncpy(g_weather.wind_dir, v->valuestring, sizeof(g_weather.wind_dir) - 1);
        if ((v = cJSON_GetObjectItem(cur, "precip_in")))     g_weather.precip_in = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "precip_mm")))     g_weather.precip_mm = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "cloud")))         g_weather.cloud = v->valueint;
        if ((v = cJSON_GetObjectItem(cur, "uv")))            g_weather.uv = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "is_day")))        g_weather.is_day = v->valueint;
        if ((v = cJSON_GetObjectItem(cur, "last_updated")) && v->valuestring)
            strncpy(g_weather.last_updated, v->valuestring, sizeof(g_weather.last_updated) - 1);

        cJSON *cond = cJSON_GetObjectItem(cur, "condition");
        if (cond) {
            if ((v = cJSON_GetObjectItem(cond, "text")) && v->valuestring)
                strncpy(g_weather.condition_text, v->valuestring, MAX_LABEL - 1);
            if ((v = cJSON_GetObjectItem(cond, "code")))
                g_weather.condition_code = v->valueint;
            if ((v = cJSON_GetObjectItem(cond, "icon")) && v->valuestring)
                strncpy(g_weather.icon_url, v->valuestring, MAX_URL - 1);
        }
    }

    cJSON *forecast = cJSON_GetObjectItem(root, "forecast");
    if (forecast) {
        cJSON *forecastday = cJSON_GetObjectItem(forecast, "forecastday");
        if (forecastday && cJSON_IsArray(forecastday)) {
            int arr_size = cJSON_GetArraySize(forecastday);
            for (int i = 0; i < arr_size && i < MAX_FORECAST_DAYS; i++) {
                cJSON *fd = cJSON_GetArrayItem(forecastday, i);
                if (!fd) continue;
                forecast_day_t *day = &g_weather.forecast[g_weather.forecast_count];
                memset(day, 0, sizeof(*day));

                cJSON *v;
                if ((v = cJSON_GetObjectItem(fd, "date")) && v->valuestring) {
                    strncpy(day->date, v->valuestring, sizeof(day->date) - 1);
                    const char *dn = day_name_from_date(day->date);
                    strncpy(day->day_name, dn, sizeof(day->day_name) - 1);
                }

                cJSON *d = cJSON_GetObjectItem(fd, "day");
                if (d) {
                    if ((v = cJSON_GetObjectItem(d, "maxtemp_f")))      day->max_temp_f = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "maxtemp_c")))      day->max_temp_c = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "mintemp_f")))      day->min_temp_f = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "mintemp_c")))      day->min_temp_c = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "maxwind_mph")))    day->max_wind_mph = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "maxwind_kph")))    day->max_wind_kph = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "daily_chance_of_rain"))) {
                        if (cJSON_IsString(v)) day->chance_rain = atoi(v->valuestring);
                        else day->chance_rain = v->valueint;
                    }
                    if ((v = cJSON_GetObjectItem(d, "daily_chance_of_snow"))) {
                        if (cJSON_IsString(v)) day->chance_snow = atoi(v->valuestring);
                        else day->chance_snow = v->valueint;
                    }
                    if ((v = cJSON_GetObjectItem(d, "totalprecip_in"))) day->total_precip_in = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "totalprecip_mm"))) day->total_precip_mm = v->valuedouble;
                    if ((v = cJSON_GetObjectItem(d, "avghumidity")))    day->avg_humidity = v->valueint;
                    if ((v = cJSON_GetObjectItem(d, "uv")))             day->uv = v->valuedouble;

                    cJSON *cond = cJSON_GetObjectItem(d, "condition");
                    if (cond) {
                        if ((v = cJSON_GetObjectItem(cond, "text")) && v->valuestring)
                            strncpy(day->condition_text, v->valuestring, MAX_LABEL - 1);
                        if ((v = cJSON_GetObjectItem(cond, "code")))
                            day->condition_code = v->valueint;
                        if ((v = cJSON_GetObjectItem(cond, "icon")) && v->valuestring)
                            strncpy(day->icon_url, v->valuestring, MAX_URL - 1);
                    }
                }

                cJSON *astro = cJSON_GetObjectItem(fd, "astro");
                if (astro) {
                    if ((v = cJSON_GetObjectItem(astro, "sunrise")) && v->valuestring)
                        strncpy(day->sunrise, v->valuestring, sizeof(day->sunrise) - 1);
                    if ((v = cJSON_GetObjectItem(astro, "sunset")) && v->valuestring)
                        strncpy(day->sunset, v->valuestring, sizeof(day->sunset) - 1);
                    if ((v = cJSON_GetObjectItem(astro, "moonrise")) && v->valuestring)
                        strncpy(day->moonrise, v->valuestring, sizeof(day->moonrise) - 1);
                    if ((v = cJSON_GetObjectItem(astro, "moonset")) && v->valuestring)
                        strncpy(day->moonset, v->valuestring, sizeof(day->moonset) - 1);
                    if ((v = cJSON_GetObjectItem(astro, "moon_phase")) && v->valuestring)
                        strncpy(day->moon_phase, v->valuestring, sizeof(day->moon_phase) - 1);
                }

                g_weather.forecast_count++;
            }
        }
    }

    g_weather.valid = 1;

    /* If we used auto:ip, save the resolved location for next time */
    if (strcmp(g_location.name, "auto:ip") == 0 && g_weather.location_name[0]) {
        if (g_weather.region[0])
            snprintf(g_location.name, MAX_LOCATION, "%s, %s",
                     g_weather.location_name, g_weather.region);
        else
            snprintf(g_location.name, MAX_LOCATION, "%s", g_weather.location_name);
        g_location.lat_lon[0] = '\0';
        g_location.id = 0;
        save_location();
        ap_log("weather: auto-detected location: %s", g_location.name);
    }

    cJSON_Delete(root);
    ap_log("weather: %s, %.0f\xc2\xb0""F, %s, %d day forecast",
           g_weather.location_name, g_weather.temp_f, g_weather.condition_text,
           g_weather.forecast_count);
    return 0;
}

static int fetch_weather(void) {
    if (g_api_key[0] == '\0') return -1;

    char url[MAX_URL];
    if (g_location.id > 0)
        snprintf(url, sizeof(url),
                 "https://api.weatherapi.com/v1/forecast.json?key=%s&q=id:%d&days=3&aqi=no",
                 g_api_key, g_location.id);
    else if (g_location.lat_lon[0])
        snprintf(url, sizeof(url),
                 "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=3&aqi=no",
                 g_api_key, g_location.lat_lon);
    else
        snprintf(url, sizeof(url),
                 "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=3&aqi=no",
                 g_api_key, g_location.name);

    fetch_task_t task;
    task.url = url;
    task.buf.data = NULL; task.buf.size = 0; task.buf.capacity = 0;
    task.result = -1;

    ap_process_opts proc = {.message = "Fetching weather...",.show_progress = false };
    ap_process_message(&proc, fetch_worker, &task);

    if (task.result != 0) return -1;

    int rc = parse_weather_data(task.buf.data);
    free(task.buf.data);
    if (rc != 0) return rc;

    if (g_weather.icon_url[0]) {
        if (g_icon_texture) SDL_DestroyTexture(g_icon_texture);
        g_icon_texture = fetch_and_load_icon(g_weather.icon_url);
    }
    for (int i = 0; i < g_weather.forecast_count; i++) {
        if (g_weather.forecast[i].icon_url[0])
            g_weather.forecast[i].icon_texture = fetch_and_load_icon(g_weather.forecast[i].icon_url);
    }
    if (!g_sunrise_icon)
        g_sunrise_icon = fetch_and_load_icon("//cdn.weatherapi.com/weather/64x64/day/113.png");
    if (!g_sunset_icon)
        g_sunset_icon = fetch_and_load_icon("//cdn.weatherapi.com/weather/64x64/night/113.png");

    return 0;
}

/* -----------------------------------------------------------------------
 * Screens: About
 * ----------------------------------------------------------------------- */

static void show_about(void) {
    pakkit_info_pair info[] = {
        {.key = "Version",.value = NIMBUS_VERSION },
        {.key = "Platform",.value = AP_PLATFORM_NAME },
        {.key = "UI",.value = "PakKit / Apostrophe" },
        {.key = "Data",.value = "WeatherAPI.com" },
        {.key = "License",.value = "MIT" },
    };

    const char *credits[] = {
        "Nimbus by Eric Reinsmidt",
        "Built with PakKit and Apostrophe",
        "For NextUI by LoveRetro",
    };

    pakkit_detail_opts opts = {
.title = "Nimbus",
.subtitle = "Weather app for NextUI",
.info = info,
.info_count = 5,
.credits = credits,
.credit_count = 3,
    };
    pakkit_detail_screen(&opts);
}

/* -----------------------------------------------------------------------
 * Location search
 * ----------------------------------------------------------------------- */

#define MAX_SEARCH_RESULTS 10

typedef struct {
    char name[MAX_LOCATION];
    char region[MAX_LOCATION];
    char country[MAX_LOCATION];
    double lat;
    double lon;
    int    id;
} search_result_t;

static int search_locations(const char *query, search_result_t *results, int max_results) {
    if (g_api_key[0] == '\0' || !query || !query[0]) return 0;

    char url[MAX_URL];
    char encoded[256] = {0};
    const char *src = query;
    char *dst = encoded, *end = encoded + sizeof(encoded) - 4;
    while (*src && dst < end) {
        if (*src == ' ') { *dst++ = '%'; *dst++ = '2'; *dst++ = '0'; }
        else { *dst++ = *src; }
        src++;
    }
    *dst = '\0';

    snprintf(url, sizeof(url),
             "https://api.weatherapi.com/v1/search.json?key=%s&q=%s",
             g_api_key, encoded);

    fetch_task_t task;
    task.url = url;
    task.buf.data = NULL; task.buf.size = 0; task.buf.capacity = 0;
    task.result = -1;

    ap_process_opts proc = {.message = "Searching...",.show_progress = false };
    ap_process_message(&proc, fetch_worker, &task);
    if (task.result != 0) return 0;

    cJSON *root = cJSON_Parse(task.buf.data);
    free(task.buf.data);
    if (!root || !cJSON_IsArray(root)) { cJSON_Delete(root); return 0; }

    int count = 0, arr_size = cJSON_GetArraySize(root);
    for (int i = 0; i < arr_size && count < max_results; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        if (!item) continue;
        cJSON *v;
        search_result_t *r = &results[count];
        memset(r, 0, sizeof(*r));
        if ((v = cJSON_GetObjectItem(item, "id")))    r->id = v->valueint;
        if ((v = cJSON_GetObjectItem(item, "name")) && v->valuestring)
            strncpy(r->name, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(item, "region")) && v->valuestring)
            strncpy(r->region, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(item, "country")) && v->valuestring)
            strncpy(r->country, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(item, "lat"))) r->lat = v->valuedouble;
        if ((v = cJSON_GetObjectItem(item, "lon"))) r->lon = v->valuedouble;
        if (r->name[0]) count++;
    }
    cJSON_Delete(root);
    return count;
}

static void search_and_set_location(void) {
    pakkit_keyboard_opts kb_opts = {.prompt = "City, zip, or postal code" };
    pakkit_keyboard_result kb_result;
    int rc = pakkit_keyboard("", &kb_opts, &kb_result);
    if (rc != AP_OK || kb_result.text[0] == '\0') return;

    search_result_t results[MAX_SEARCH_RESULTS];
    int count = search_locations(kb_result.text, results, MAX_SEARCH_RESULTS);

    /*... rest unchanged... */

    if (count == 0) {
        pakkit_message("No locations found.\nTry a different search.", "OK");
        return;
    }

    static char result_labels[MAX_SEARCH_RESULTS][512];
    pakkit_list_item items[MAX_SEARCH_RESULTS];
    for (int i = 0; i < count; i++) {
        if (results[i].region[0])
            snprintf(result_labels[i], sizeof(result_labels[i]), "%s, %s, %s",
                     results[i].name, results[i].region, results[i].country);
        else
            snprintf(result_labels[i], sizeof(result_labels[i]), "%s, %s",
                     results[i].name, results[i].country);
        items[i].label = result_labels[i];
    }

    pakkit_hint hints[] = {
        {.button = "B",.label = "Cancel" },
        {.button = "A",.label = "Select" },
    };
    pakkit_list_opts opts = {.title = "Select Location",.hints = hints,.hint_count = 2,.secondary_button = AP_BTN_NONE,.tertiary_button = AP_BTN_NONE,
    };

    pakkit_list_result result;
    rc = pakkit_list(&opts, items, count, &result);
    if (rc != AP_OK || result.selected_index < 0) return;

    search_result_t *sel = &results[result.selected_index];
    if (sel->region[0])
        snprintf(g_location.name, MAX_LOCATION, "%s, %s", sel->name, sel->region);
    else
        snprintf(g_location.name, MAX_LOCATION, "%s", sel->name);
    snprintf(g_location.lat_lon, sizeof(g_location.lat_lon), "%.2f,%.2f", sel->lat, sel->lon);
    g_location.id = sel->id;
    save_location();
}

/* -----------------------------------------------------------------------
 * Screens: Settings
 * ----------------------------------------------------------------------- */

static void show_settings(void) {
    while (1) {
        char units_label[32];
        snprintf(units_label, sizeof(units_label), "Units: %s",
                 g_settings.use_fahrenheit ? "\xc2\xb0""F" : "\xc2\xb0""C");

        pakkit_menu_item items[] = {
            {.label = units_label },
            {.label = "Set Location" },
            {.label = "Change API Key" },
            {.label = "About" },
        };

        pakkit_menu_result result;
        int rc = pakkit_menu("Settings", items, 4, &result);
        if (rc != AP_OK) return;

        switch (result.selected_index) {
            case 0:
                g_settings.use_fahrenheit = !g_settings.use_fahrenheit;
                settings_save();
                break;
            case 1:
                search_and_set_location();
                break;
            case 2:
                show_api_key_setup();
                break;
            case 3:
                show_about();
                break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Screens: Custom weather display
 * ----------------------------------------------------------------------- */

static void show_weather_screen(void) {
    int running = 1;
    int scroll_y = 0;

    while (running) {
        ap_input_event ev;
        while (ap_poll_input(&ev)) {
            if (ev.pressed) {
                switch (ev.button) {
                    case AP_BTN_B:
                        if (!ev.repeated) running = 0;
                        break;
                    case AP_BTN_Y:
                        if (!ev.repeated) {
                            show_settings();
                            fetch_weather();
                            scroll_y = 0;
                        }
                        break;
                    case AP_BTN_UP:
                        if (scroll_y > 0) scroll_y -= SCROLL_STEP;
                        if (scroll_y < 0) scroll_y = 0;
                        break;
                    case AP_BTN_DOWN:
                        scroll_y += SCROLL_STEP;
                        break;
                    default:
                        break;
                }
            }
        }

        ap_clear_screen();
        ap_draw_background();

        int sw = ap_get_screen_width();
        int sh = ap_get_screen_height();
        int pad = AP_DS(5);

        TTF_Font *font_xl    = ap_get_font(AP_FONT_EXTRA_LARGE);
        TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
        TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
        TTF_Font *font_tiny  = ap_get_font(AP_FONT_TINY);

        ap_theme *theme = ap_get_theme();
        ap_color text_color = theme->text;
        ap_color hint_color = theme->hint;

        int hint_font_h = TTF_FontHeight(font_tiny);
        int footer_h = hint_font_h + pad * 2;

        int content_top = pad;
        int content_bottom = sh - footer_h;
        int content_h = content_bottom - content_top;

        SDL_Rect clip = { 0, content_top, sw, content_h };
        SDL_RenderSetClipRect(ap__g.renderer, &clip);

        int y = content_top - scroll_y;

        if (!g_weather.valid) {
            scroll_y = 0;
            ap_draw_text(font_med, "No weather data", pad * 3, content_top + content_h / 3, hint_color);
            ap_draw_text(font_small, "Press Y for settings",
                         pad * 3, content_top + content_h / 3 + TTF_FontHeight(font_med) + pad, hint_color);
        } else {
            /* Location */
            char location_full[512];
            if (g_weather.region[0])
                snprintf(location_full, sizeof(location_full), "%s, %s",
                         g_weather.location_name, g_weather.region);
            else
                snprintf(location_full, sizeof(location_full), "%s",
                         g_weather.location_name);
            ap_draw_text(font_med, location_full, pad * 3, y, hint_color);
            y += TTF_FontHeight(font_med) + pad;

            /* Icon + Temperature */
            int icon_size = AP_DS(64);
            int icon_x = pad * 3;

            if (g_icon_texture)
                ap_draw_image(g_icon_texture, icon_x, y, icon_size, icon_size);

            char temp_str[32];
            if (g_settings.use_fahrenheit)
                snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0""F", g_weather.temp_f);
            else
                snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0""C", g_weather.temp_c);

            int temp_x = icon_x + icon_size + pad * 2;
            int temp_y = y + (icon_size / 2) - TTF_FontHeight(font_xl) / 2 - pad;
            ap_draw_text(font_xl, temp_str, temp_x, temp_y, text_color);

            int cond_y = temp_y + TTF_FontHeight(font_xl) + 2;
            ap_draw_text(font_med, g_weather.condition_text, temp_x, cond_y, hint_color);

            y += icon_size + pad * 2;

            /* Divider */
            ap_draw_rect(pad * 3, y, sw - pad * 6, 1, hint_color);
            y += pad * 2;

            /* Details */
            int col1_x = pad * 3;
            int col2_x = sw / 2 + pad;
            int row_h = TTF_FontHeight(font_small) + pad;

            char feels_str[32], humidity_str[32], wind_str[64];
            char precip_str[32], cloud_str[32], uv_str[32];

            if (g_settings.use_fahrenheit) {
                snprintf(feels_str, sizeof(feels_str), "Feels like %.0f\xc2\xb0""F", g_weather.feels_like_f);
                snprintf(wind_str, sizeof(wind_str), "Wind: %.0f mph %s", g_weather.wind_mph, g_weather.wind_dir);
                snprintf(precip_str, sizeof(precip_str), "Precip: %.2f in", g_weather.precip_in);
            } else {
                snprintf(feels_str, sizeof(feels_str), "Feels like %.0f\xc2\xb0""C", g_weather.feels_like_c);
                snprintf(wind_str, sizeof(wind_str), "Wind: %.0f km/h %s", g_weather.wind_kph, g_weather.wind_dir);
                snprintf(precip_str, sizeof(precip_str), "Precip: %.1f mm", g_weather.precip_mm);
            }
            snprintf(humidity_str, sizeof(humidity_str), "Humidity: %d%%", g_weather.humidity);
            snprintf(cloud_str, sizeof(cloud_str), "Cloud: %d%%", g_weather.cloud);
            snprintf(uv_str, sizeof(uv_str), "UV: %.0f", g_weather.uv);

            ap_draw_text(font_small, feels_str, col1_x, y, text_color);
            ap_draw_text(font_small, humidity_str, col2_x, y, text_color);
            y += row_h;
            ap_draw_text(font_small, wind_str, col1_x, y, text_color);
            ap_draw_text(font_small, cloud_str, col2_x, y, text_color);
            y += row_h;
            ap_draw_text(font_small, precip_str, col1_x, y, text_color);
            ap_draw_text(font_small, uv_str, col2_x, y, text_color);
            y += row_h;

            /* Sunrise / Sunset */
            if (g_weather.forecast_count > 0) {
                forecast_day_t *today = &g_weather.forecast[0];
                if (today->sunrise[0] && today->sunset[0]) {
                    int sun_icon_size = TTF_FontHeight(font_small);
                    int text_offset = sun_icon_size + pad;

                    if (g_sunrise_icon)
                        ap_draw_image(g_sunrise_icon, col1_x, y, sun_icon_size, sun_icon_size);
                    ap_draw_text(font_small, today->sunrise, col1_x + text_offset, y, text_color);

                    if (g_sunset_icon)
                        ap_draw_image(g_sunset_icon, col2_x, y, sun_icon_size, sun_icon_size);
                    ap_draw_text(font_small, today->sunset, col2_x + text_offset, y, text_color);

                    y += row_h;
                }
                if (today->moon_phase[0]) {
                    char moon_str[64];
                    snprintf(moon_str, sizeof(moon_str), "Moon: %s", today->moon_phase);
                    ap_draw_text(font_small, moon_str, col1_x, y, hint_color);
                    y += row_h;
                }
            }

            y += pad;

            /* Divider */
            ap_draw_rect(pad * 3, y, sw - pad * 6, 1, hint_color);
            y += pad * 2;

            /* Forecast */
            ap_draw_text(font_med, "Forecast", pad * 3, y, text_color);
            y += TTF_FontHeight(font_med) + pad;

            int fc_icon_size = AP_DS(40);
            for (int i = 0; i < g_weather.forecast_count; i++) {
                forecast_day_t *day = &g_weather.forecast[i];

                char day_label[64];
                if (i == 0) snprintf(day_label, sizeof(day_label), "Today");
                else snprintf(day_label, sizeof(day_label), "%s", day->day_name);

                int row_top = y;

                if (day->icon_texture)
                    ap_draw_image(day->icon_texture, col1_x, y, fc_icon_size, fc_icon_size);

                int text_x = col1_x + fc_icon_size + pad * 2;

                char line1[128];
                snprintf(line1, sizeof(line1), "%s  %s", day_label, day->condition_text);
                ap_draw_text(font_small, line1, text_x, y, text_color);
                y += TTF_FontHeight(font_small) + 2;

                char line2[128];
                if (g_settings.use_fahrenheit)
                    snprintf(line2, sizeof(line2), "H:%.0f\xc2\xb0  L:%.0f\xc2\xb0  Rain:%d%%",
                             day->max_temp_f, day->min_temp_f, day->chance_rain);
                else
                    snprintf(line2, sizeof(line2), "H:%.0f\xc2\xb0  L:%.0f\xc2\xb0  Rain:%d%%",
                             day->max_temp_c, day->min_temp_c, day->chance_rain);
                ap_draw_text(font_tiny, line2, text_x, y, hint_color);

                int row_bottom = y + TTF_FontHeight(font_tiny);
                int min_bottom = row_top + fc_icon_size;
                y = (row_bottom > min_bottom ? row_bottom : min_bottom) + pad;
            }

            y += pad;

            /* Updated */
            char updated_str[128];
            snprintf(updated_str, sizeof(updated_str), "Updated: %s", g_weather.last_updated);
            ap_draw_text(font_tiny, updated_str, pad * 3, y, hint_color);
            y += TTF_FontHeight(font_tiny) + pad * 2;

            /* Clamp scroll */
            int total_content = y + scroll_y - content_top;
            int max_scroll = total_content - content_h;
            if (max_scroll < 0) max_scroll = 0;
            if (scroll_y > max_scroll) scroll_y = max_scroll;
        }

        SDL_RenderSetClipRect(ap__g.renderer, NULL);

        /* Minimal text hints */
        int hint_y = sh - hint_font_h - pad;
        ap_draw_text(font_tiny, "B: Quit", pad * 2, hint_y, hint_color);
        int menu_w = ap_measure_text(font_tiny, "Y: Menu");
        ap_draw_text(font_tiny, "Y: Menu", sw - menu_w - pad * 2, hint_y, hint_color);

        ap_present();
    }
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *config_env = getenv("NIMBUS_CONFIG_DIR");
    if (config_env) strncpy(g_config_dir, config_env, sizeof(g_config_dir) - 1);

    const char *cache_env = getenv("NIMBUS_CACHE_DIR");
    if (cache_env) strncpy(g_cache_dir, cache_env, sizeof(g_cache_dir) - 1);

    settings_load();

    /* Truncate log file so it only contains the current session */
    const char *log_path = ap_resolve_log_path("nimbus");
    if (log_path) {
        FILE *lf = fopen(log_path, "w");
        if (lf) fclose(lf);
    }

    ap_config cfg = {.window_title       = "Nimbus",.log_path           = log_path,.is_nextui          = AP_PLATFORM_IS_DEVICE,.disable_background = true,
    };
    if (ap_init(&cfg) != AP_OK) {
        fprintf(stderr, "Failed to initialise Apostrophe\n");
        curl_global_cleanup();
        return 1;
    }

    ap_theme *theme = ap_get_theme();
    theme->background = (ap_color){DEFAULT_BG_R, DEFAULT_BG_G, DEFAULT_BG_B, 255};
    theme->text       = (ap_color){DEFAULT_TEXT_R, DEFAULT_TEXT_G, DEFAULT_TEXT_B, 255};
    theme->hint       = (ap_color){DEFAULT_HINT_R, DEFAULT_HINT_G, DEFAULT_HINT_B, 255};

    ap_log("=== Nimbus v%s starting ===", NIMBUS_VERSION);

    /* API key: load or run setup */
    if (load_api_key() != 0) {
        if (show_api_key_setup() != 0) {
            ap_quit();
            curl_global_cleanup();
            return 0;
        }
    }

    /* Location — if none configured, use IP geolocation */
    if (load_location() != 0) {
        snprintf(g_location.name, MAX_LOCATION, "auto:ip");
        g_location.lat_lon[0] = '\0';
        g_location.id = 0;
    }

    /* Check WiFi before fetching */
    if (ap__get_wifi_strength() == 0) {
        pakkit_message("WiFi not connected.\n\n"
                       "Connect to WiFi and\n"
                       "reopen Nimbus.", "Quit");
        ap_quit();
        curl_global_cleanup();
        return 0;
    }

    fetch_weather();
    show_weather_screen();

    if (g_icon_texture) SDL_DestroyTexture(g_icon_texture);
    if (g_sunrise_icon) SDL_DestroyTexture(g_sunrise_icon);
    if (g_sunset_icon) SDL_DestroyTexture(g_sunset_icon);
    for (int i = 0; i < g_weather.forecast_count; i++) {
        if (g_weather.forecast[i].icon_texture)
            SDL_DestroyTexture(g_weather.forecast[i].icon_texture);
    }

    ap_log("=== Nimbus shutting down ===");
    ap_quit();
    curl_global_cleanup();
    return 0;
}
