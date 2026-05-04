/*
 * Nimbus — Weather app for NextUI
 * Apostrophe UI + libcurl + cJSON + WeatherAPI.com
 * QR-based API key setup via embedded web server.
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"

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

#define NIMBUS_VERSION   "1.0.0"
#define MAX_PATH_LEN     1280
#define MAX_LOCATION     256
#define MAX_LINE         512
#define MAX_URL          1024
#define MAX_LABEL        256
#define MAX_FORECAST_DAYS 3
#define MAX_HOURS_PER_DAY 24
#define MAX_HOURS        72
#define MAX_LOCATIONS    10

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

#define TAB_CURRENT  0
#define TAB_FORECAST 1
#define TAB_HOURLY   2
#define TAB_ASTRO    3
#define TAB_COUNT    4

/* -----------------------------------------------------------------------
 * Data structures
 * ----------------------------------------------------------------------- */

typedef struct {
    char name[MAX_LOCATION];
    char lat_lon[64];
    int  id;
    int  is_home;
} location_t;

typedef struct {
    char   time[32];        /* "2024-01-15 14:00" */
    char   hour_label[8];   /* "2 PM" */
    double temp_f;
    double temp_c;
    double feels_like_f;
    double feels_like_c;
    int    humidity;
    double wind_mph;
    double wind_kph;
    char   wind_dir[16];
    int    chance_rain;
    int    chance_snow;
    double precip_in;
    double precip_mm;
    int    cloud;
    double uv;
    int    is_day;
    char   condition_text[MAX_LABEL];
    int    condition_code;
    char   icon_url[MAX_URL];
    SDL_Texture *icon_texture;
} hourly_t;

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
    int    moon_illumination;

    SDL_Texture *icon_texture;
} forecast_day_t;

typedef struct {
    char location_name[MAX_LOCATION];
    char region[MAX_LOCATION];
    char country[MAX_LOCATION];

    double loc_lat;
    double loc_lon;
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

    hourly_t       hours[MAX_HOURS];
    int            hour_count;

    int    valid;
    int    is_cached;
    char   cached_time[32];
    SDL_Texture *icon_texture;
} weather_data_t;

typedef struct {
    char  *data;
    size_t size;
    size_t capacity;
} fetch_buf_t;

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

static location_t      g_locations[MAX_LOCATIONS];
static int             g_location_count = 0;
static int             g_current_location = 0;
static weather_data_t  g_weather_cache[MAX_LOCATIONS];

static char            g_api_key[128]             = {0};
static char            g_config_dir[MAX_PATH_LEN] = {0};
static char            g_cache_dir[MAX_PATH_LEN]  = {0};
static app_settings_t  g_settings;
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

static void format_hour_label(const char *time_str, char *out, size_t out_size) {
    /* Input: "2024-01-15 14:00", Output: "2 PM" */
    out[0] = '\0';
    const char *space = strchr(time_str, ' ');
    if (!space) return;
    int hour = 0;
    sscanf(space + 1, "%d", &hour);
    if (hour == 0) snprintf(out, out_size, "12 AM");
    else if (hour < 12) snprintf(out, out_size, "%d AM", hour);
    else if (hour == 12) snprintf(out, out_size, "12 PM");
    else snprintf(out, out_size, "%d PM", hour - 12);
}

/* -----------------------------------------------------------------------
 * WiFi check
 * ----------------------------------------------------------------------- */

static int check_wifi(void) {
    return ap__get_wifi_strength() > 0;
}

/* -----------------------------------------------------------------------
 * Offline weather cache (JSON per location)
 * ----------------------------------------------------------------------- */

static void weather_cache_path(int loc_idx, char *out, size_t out_size) {
    if (g_cache_dir[0] == '\0' || loc_idx < 0 || loc_idx >= g_location_count) {
        out[0] = '\0';
        return;
    }
    /* Use a hash of location name + lat_lon for a stable filename */
    location_t *loc = &g_locations[loc_idx];
    unsigned long hash = 5381;
    const char *p = loc->name;
    while (*p) { hash = ((hash << 5) + hash) + (unsigned char)*p; p++; }
    p = loc->lat_lon;
    while (*p) { hash = ((hash << 5) + hash) + (unsigned char)*p; p++; }
    snprintf(out, out_size, "%s/weather_%08lx.json", g_cache_dir, hash);
}

static void save_weather_json(int loc_idx, const char *json_str) {
    char path[MAX_PATH_LEN];
    weather_cache_path(loc_idx, path, sizeof(path));
    if (path[0] == '\0') return;
    FILE *f = fopen(path, "w");
    if (!f) { ap_log("weather_cache: could not write %s", path); return; }
    fprintf(f, "%s", json_str);
    fclose(f);
    ap_log("weather_cache: saved for location %d (%s)", loc_idx, g_locations[loc_idx].name);
}

static char *load_weather_json(int loc_idx) {
    char path[MAX_PATH_LEN];
    weather_cache_path(loc_idx, path, sizeof(path));
    if (path[0] == '\0') return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return NULL; }
    char *data = malloc((size_t)fsize + 1);
    if (!data) { fclose(f); return NULL; }
    size_t read_bytes = fread(data, 1, (size_t)fsize, f);
    fclose(f);
    data[read_bytes] = '\0';
    ap_log("weather_cache: loaded %zu bytes for location %d", read_bytes, loc_idx);
    return data;
}

static void get_weather_cache_time(int loc_idx, char *out, size_t out_size) {
    out[0] = '\0';
    char path[MAX_PATH_LEN];
    weather_cache_path(loc_idx, path, sizeof(path));
    if (path[0] == '\0') return;
    struct stat st;
    if (stat(path, &st) != 0) return;
    struct tm *tm_info = localtime(&st.st_mtime);
    if (!tm_info) return;
    int hour = tm_info->tm_hour;
    int min = tm_info->tm_min;
    const char *ampm = (hour >= 12) ? "PM" : "AM";
    if (hour == 0) hour = 12;
    else if (hour > 12) hour -= 12;
    snprintf(out, out_size, "%d:%02d %s", hour, min, ampm);
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
 * Config: Locations (multi-location)
 * ----------------------------------------------------------------------- */

static int get_home_index(void) {
    for (int i = 0; i < g_location_count; i++) {
        if (g_locations[i].is_home) return i;
    }
    return 0;
}

static int load_locations(void) {
    g_location_count = 0;
    if (g_config_dir[0] == '\0') return -1;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/locations.txt", g_config_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "%s/location.txt", g_config_dir);
        f = fopen(path, "r");
        if (!f) return -1;
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            trim_inplace(line);
            if (line[0] == '#' || line[0] == '\0') continue;
            char *sep = strchr(line, '|');
            if (sep) {
                *sep = '\0';
                location_t *loc = &g_locations[0];
                memset(loc, 0, sizeof(*loc));
                snprintf(loc->name, MAX_LOCATION, "%s", line);
                trim_inplace(loc->name);
                char *rest = sep + 1;
                char *sep2 = strchr(rest, '|');
                if (sep2) {
                    *sep2 = '\0';
                    snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
                    trim_inplace(loc->lat_lon);
                    loc->id = atoi(sep2 + 1);
                } else {
                    snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
                    trim_inplace(loc->lat_lon);
                }
                loc->is_home = 1;
                g_location_count = 1;
            }
            break;
        }
        fclose(f);
        if (g_location_count > 0) {
            ap_log("config: migrated legacy location: %s", g_locations[0].name);
            return 0;
        }
        return -1;
    }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f) && g_location_count < MAX_LOCATIONS) {
        trim_inplace(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        location_t *loc = &g_locations[g_location_count];
        memset(loc, 0, sizeof(*loc));

        char *p1 = strchr(line, '|');
        if (!p1) continue;
        *p1 = '\0';
        snprintf(loc->name, MAX_LOCATION, "%s", line);
        trim_inplace(loc->name);

        char *rest = p1 + 1;
        char *p2 = strchr(rest, '|');
        if (p2) {
            *p2 = '\0';
            snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
            trim_inplace(loc->lat_lon);
            rest = p2 + 1;
            char *p3 = strchr(rest, '|');
            if (p3) {
                *p3 = '\0';
                loc->id = atoi(rest);
                loc->is_home = atoi(p3 + 1);
            } else {
                loc->id = atoi(rest);
            }
        } else {
            snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%s", rest);
            trim_inplace(loc->lat_lon);
        }

        if (loc->name[0]) {
            ap_log("config: location %d: %s (id:%d, home:%d)",
                   g_location_count, loc->name, loc->id, loc->is_home);
            g_location_count++;
        }
    }
    fclose(f);

    int home_found = 0;
    for (int i = 0; i < g_location_count; i++) {
        if (g_locations[i].is_home) {
            if (home_found) g_locations[i].is_home = 0;
            home_found = 1;
        }
    }
    if (!home_found && g_location_count > 0)
        g_locations[0].is_home = 1;

    ap_log("config: loaded %d location(s)", g_location_count);
    return (g_location_count > 0) ? 0 : -1;
}

static void save_locations(void) {
    if (g_config_dir[0] == '\0') return;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/locations.txt", g_config_dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# Nimbus — Locations\n");
    fprintf(f, "# Format: Name|lat,lon|id|home\n");
    for (int i = 0; i < g_location_count; i++) {
        fprintf(f, "%s|%s|%d|%d\n",
                g_locations[i].name, g_locations[i].lat_lon,
                g_locations[i].id, g_locations[i].is_home);
    }
    fclose(f);
    ap_log("config: saved %d location(s)", g_location_count);
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
        if (strcmp(ifa->ifa_name, "wlan0") != 0) continue;
        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        inet_ntop(AF_INET, &addr->sin_addr, out, (socklen_t)out_size);
        break;
    }
    freeifaddrs(ifaddr);
    return out[0] ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Embedded setup web server (unchanged)
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
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(srv->server_fd, &fds);
        struct timeval tv = {.tv_sec = 0,.tv_usec = 500000 };
        int sel = select(srv->server_fd + 1, &fds, NULL, NULL, &tv);
        if (sel <= 0) continue;
        int client_fd = accept(srv->server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;
        char req_buf[4096] = {0};
        ssize_t n = read(client_fd, req_buf, sizeof(req_buf) - 1);
        if (n <= 0) { close(client_fd); continue; }
        req_buf[n] = '\0';
        int is_post = (strncmp(req_buf, "POST", 4) == 0);
        int is_key_post = is_post && strstr(req_buf, "POST /key") != NULL;
        char response[8192];
        if (is_key_post) {
            const char *body = strstr(req_buf, "\r\n\r\n");
            if (body) body += 4; else body = "";
            char key_val[128] = {0};
            extract_form_value(body, "key", key_val, sizeof(key_val));
            trim_inplace(key_val);
            if (key_val[0]) {
                snprintf(srv->received_key, sizeof(srv->received_key), "%s", key_val);
                snprintf(response, sizeof(response),
                         "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n%s",
                         SETUP_OK_HTML);
                write(client_fd, response, strlen(response));
                close(client_fd);
                srv->running = 0;
                break;
            }
        }
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n%s",
                 SETUP_HTML);
        write(client_fd, response, strlen(response));
        close(client_fd);
    }
    return NULL;
}

static int setup_server_start(setup_server_t *srv) {
    memset(srv, 0, sizeof(*srv));
    srv->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->server_fd < 0) return -1;
    int opt = 1;
    setsockopt(srv->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SETUP_SERVER_PORT);
    if (bind(srv->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv->server_fd); return -1;
    }
    if (listen(srv->server_fd, 5) < 0) {
        close(srv->server_fd); return -1;
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
}

/* -----------------------------------------------------------------------
 * QR code rendering
 * ----------------------------------------------------------------------- */

static void draw_qr_code(const char *text, int cx, int cy, int max_size) {
    uint8_t qr_buf[qrcodegen_BUFFER_LEN_MAX];
    uint8_t temp_buf[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text, temp_buf, qr_buf,
                               qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                               qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true)) return;
    int qr_size = qrcodegen_getSize(qr_buf);
    int module_px = max_size / (qr_size + 4);
    if (module_px < 1) module_px = 1;
    int total_px = module_px * (qr_size + 4);
    int ox = cx - total_px / 2;
    int oy = cy - total_px / 2;
    ap_color white = {255, 255, 255, 255};
    ap_color black = {0, 0, 0, 255};
    ap_draw_rect(ox, oy, total_px, total_px, white);
    int quiet = module_px * 2;
    for (int y = 0; y < qr_size; y++)
        for (int x = 0; x < qr_size; x++)
            if (qrcodegen_getModule(qr_buf, x, y))
                ap_draw_rect(ox + quiet + x * module_px, oy + quiet + y * module_px,
                             module_px, module_px, black);
}

/* -----------------------------------------------------------------------
 * API key setup screen
 * ----------------------------------------------------------------------- */

static int show_api_key_setup(void) {
    int wifi_strength = ap__get_wifi_strength();
    char ip[64] = {0};
    if (wifi_strength == 0 || get_local_ip(ip, sizeof(ip)) != 0) {
        pakkit_message("WiFi not connected.\n\nConnect to WiFi and restart Nimbus,\n"
                       "or manually create:\n\n.userdata/tg5040/nimbus/\n  config/api_key.txt", "Quit");
        return -1;
    }
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d", ip, SETUP_SERVER_PORT);
    setup_server_t srv;
    if (setup_server_start(&srv) != 0) {
        pakkit_message("Could not start setup server.", "Quit");
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
                srv.running = 0; result = -1;
            }
        }
        ap_clear_screen();
        ap_draw_background();
        int y = pad * 3;
        const char *title = "Nimbus Setup";
        int title_w = ap_measure_text(font_large, title);
        ap_draw_text(font_large, title, (sw - title_w) / 2, y, text_color);
        y += TTF_FontHeight(font_large) + pad * 2;
        const char *inst = "Scan QR code or visit URL to enter API key";
        int inst_w = ap_measure_text(font_small, inst);
        ap_draw_text(font_small, inst, (sw - inst_w) / 2, y, hint_color);
        y += TTF_FontHeight(font_small) + pad * 3;
        int hint_font_h = TTF_FontHeight(font_tiny);
        int footer_h = hint_font_h + pad * 2;
        int avail_h = sh - y - footer_h - TTF_FontHeight(font_med) - TTF_FontHeight(font_small) - pad * 6;
        int qr_max = avail_h;
        if (qr_max > sw / 2) qr_max = sw / 2;
        int qr_cy = y + qr_max / 2;
        draw_qr_code(url, sw / 2, qr_cy, qr_max);
        y = qr_cy + qr_max / 2 + pad * 2;
        int url_w = ap_measure_text(font_med, url);
        ap_draw_text(font_med, url, (sw - url_w) / 2, y, text_color);
        y += TTF_FontHeight(font_med) + pad;
        const char *wait = "Waiting for API key...";
        int wait_w = ap_measure_text(font_small, wait);
        ap_draw_text(font_small, wait, (sw - wait_w) / 2, y, hint_color);
        int hint_y = sh - TTF_FontHeight(font_tiny) - pad;
        ap_draw_text(font_tiny, "B: Cancel", pad * 2, hint_y, hint_color);
        ap_present();
    }
    if (srv.received_key[0]) {
        snprintf(g_api_key, sizeof(g_api_key), "%s", srv.received_key);
        trim_inplace(g_api_key);
        save_api_key(g_api_key);
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
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nimbus/0.3");
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
        free(buf->data); buf->data = NULL; buf->size = 0;
        return -1;
    }
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

static SDL_Texture *load_cached_icon(const char *icon_url) {
    if (!icon_url || !icon_url[0] || g_cache_dir[0] == '\0') return NULL;
    const char *slash = strrchr(icon_url, '/');
    const char *fname = slash ? slash + 1 : "icon.png";
    const char *day_night = strstr(icon_url, "/day/") ? "day" : "night";
    char cache_path[MAX_PATH_LEN];
    snprintf(cache_path, sizeof(cache_path), "%s/%s_%s", g_cache_dir, day_night, fname);
    if (access(cache_path, R_OK) == 0)
        return ap_load_image(cache_path);
    return NULL;
}

/* -----------------------------------------------------------------------
 * WeatherAPI.com: fetch + parse
 * ----------------------------------------------------------------------- */

static void free_weather_textures(weather_data_t *w) {
    if (w->icon_texture) { SDL_DestroyTexture(w->icon_texture); w->icon_texture = NULL; }
    for (int i = 0; i < w->forecast_count; i++) {
        if (w->forecast[i].icon_texture) {
            SDL_DestroyTexture(w->forecast[i].icon_texture);
            w->forecast[i].icon_texture = NULL;
        }
    }
    for (int i = 0; i < w->hour_count; i++) {
        if (w->hours[i].icon_texture) {
            SDL_DestroyTexture(w->hours[i].icon_texture);
            w->hours[i].icon_texture = NULL;
        }
    }
}

static int parse_weather_data(const char *json_str, weather_data_t *weather) {
    free_weather_textures(weather);
    memset(weather, 0, sizeof(*weather));

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) { cJSON_Delete(root); return -1; }

    cJSON *loc = cJSON_GetObjectItem(root, "location");
    if (loc) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(loc, "name")) && v->valuestring)
            strncpy(weather->location_name, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(loc, "region")) && v->valuestring)
            strncpy(weather->region, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(loc, "country")) && v->valuestring)
            strncpy(weather->country, v->valuestring, MAX_LOCATION - 1);
        if ((v = cJSON_GetObjectItem(loc, "lat"))) weather->loc_lat = v->valuedouble;
        if ((v = cJSON_GetObjectItem(loc, "lon"))) weather->loc_lon = v->valuedouble;
    }

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(cur, "temp_f")))       weather->temp_f = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "temp_c")))       weather->temp_c = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "feelslike_f")))   weather->feels_like_f = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "feelslike_c")))   weather->feels_like_c = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "humidity")))      weather->humidity = v->valueint;
        if ((v = cJSON_GetObjectItem(cur, "wind_mph")))      weather->wind_mph = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "wind_kph")))      weather->wind_kph = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "wind_dir")) && v->valuestring)
            strncpy(weather->wind_dir, v->valuestring, sizeof(weather->wind_dir) - 1);
        if ((v = cJSON_GetObjectItem(cur, "precip_in")))     weather->precip_in = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "precip_mm")))     weather->precip_mm = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "cloud")))         weather->cloud = v->valueint;
        if ((v = cJSON_GetObjectItem(cur, "uv")))            weather->uv = v->valuedouble;
        if ((v = cJSON_GetObjectItem(cur, "is_day")))        weather->is_day = v->valueint;
        if ((v = cJSON_GetObjectItem(cur, "last_updated")) && v->valuestring)
            strncpy(weather->last_updated, v->valuestring, sizeof(weather->last_updated) - 1);
        cJSON *cond = cJSON_GetObjectItem(cur, "condition");
        if (cond) {
            if ((v = cJSON_GetObjectItem(cond, "text")) && v->valuestring)
                strncpy(weather->condition_text, v->valuestring, MAX_LABEL - 1);
            if ((v = cJSON_GetObjectItem(cond, "code")))
                weather->condition_code = v->valueint;
            if ((v = cJSON_GetObjectItem(cond, "icon")) && v->valuestring)
                strncpy(weather->icon_url, v->valuestring, MAX_URL - 1);
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
                forecast_day_t *day = &weather->forecast[weather->forecast_count];
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
                    if ((v = cJSON_GetObjectItem(astro, "moon_illumination"))) {
                        if (cJSON_IsString(v)) day->moon_illumination = atoi(v->valuestring);
                        else day->moon_illumination = v->valueint;
                    }
                }

                /* Parse hourly data */
                cJSON *hour_arr = cJSON_GetObjectItem(fd, "hour");
                if (hour_arr && cJSON_IsArray(hour_arr)) {
                    int h_size = cJSON_GetArraySize(hour_arr);
                    for (int h = 0; h < h_size && weather->hour_count < MAX_HOURS; h++) {
                        cJSON *hr = cJSON_GetArrayItem(hour_arr, h);
                        if (!hr) continue;
                        hourly_t *hour = &weather->hours[weather->hour_count];
                        memset(hour, 0, sizeof(*hour));

                        if ((v = cJSON_GetObjectItem(hr, "time")) && v->valuestring) {
                            strncpy(hour->time, v->valuestring, sizeof(hour->time) - 1);
                            format_hour_label(hour->time, hour->hour_label, sizeof(hour->hour_label));
                        }
                        if ((v = cJSON_GetObjectItem(hr, "temp_f")))       hour->temp_f = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "temp_c")))       hour->temp_c = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "feelslike_f")))   hour->feels_like_f = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "feelslike_c")))   hour->feels_like_c = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "humidity")))      hour->humidity = v->valueint;
                        if ((v = cJSON_GetObjectItem(hr, "wind_mph")))      hour->wind_mph = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "wind_kph")))      hour->wind_kph = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "wind_dir")) && v->valuestring)
                            strncpy(hour->wind_dir, v->valuestring, sizeof(hour->wind_dir) - 1);
                        if ((v = cJSON_GetObjectItem(hr, "chance_of_rain"))) {
                            if (cJSON_IsString(v)) hour->chance_rain = atoi(v->valuestring);
                            else hour->chance_rain = v->valueint;
                        }
                        if ((v = cJSON_GetObjectItem(hr, "chance_of_snow"))) {
                            if (cJSON_IsString(v)) hour->chance_snow = atoi(v->valuestring);
                            else hour->chance_snow = v->valueint;
                        }
                        if ((v = cJSON_GetObjectItem(hr, "precip_in")))    hour->precip_in = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "precip_mm")))    hour->precip_mm = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "cloud")))        hour->cloud = v->valueint;
                        if ((v = cJSON_GetObjectItem(hr, "uv")))           hour->uv = v->valuedouble;
                        if ((v = cJSON_GetObjectItem(hr, "is_day")))       hour->is_day = v->valueint;

                        cJSON *hcond = cJSON_GetObjectItem(hr, "condition");
                        if (hcond) {
                            if ((v = cJSON_GetObjectItem(hcond, "text")) && v->valuestring)
                                strncpy(hour->condition_text, v->valuestring, MAX_LABEL - 1);
                            if ((v = cJSON_GetObjectItem(hcond, "code")))
                                hour->condition_code = v->valueint;
                            if ((v = cJSON_GetObjectItem(hcond, "icon")) && v->valuestring)
                                strncpy(hour->icon_url, v->valuestring, MAX_URL - 1);
                        }

                        weather->hour_count++;
                    }
                }

                weather->forecast_count++;
            }
        }
    }

    weather->valid = 1;
    cJSON_Delete(root);
    ap_log("weather: %s, %.0f\xc2\xb0""F, %s, %d day forecast, %d hours",
           weather->location_name, weather->temp_f, weather->condition_text,
           weather->forecast_count, weather->hour_count);
    return 0;
}

static void load_weather_icons(weather_data_t *weather, int offline) {
    if (weather->icon_url[0]) {
        if (weather->icon_texture) SDL_DestroyTexture(weather->icon_texture);
        weather->icon_texture = offline
            ? load_cached_icon(weather->icon_url)
            : fetch_and_load_icon(weather->icon_url);
    }
    for (int i = 0; i < weather->forecast_count; i++) {
        if (weather->forecast[i].icon_url[0])
            weather->forecast[i].icon_texture = offline
                ? load_cached_icon(weather->forecast[i].icon_url)
                : fetch_and_load_icon(weather->forecast[i].icon_url);
    }
    for (int i = 0; i < weather->hour_count; i++) {
        if (weather->hours[i].icon_url[0])
            weather->hours[i].icon_texture = offline
                ? load_cached_icon(weather->hours[i].icon_url)
                : fetch_and_load_icon(weather->hours[i].icon_url);
    }
    if (!g_sunrise_icon) {
        const char *sun_url = "//cdn.weatherapi.com/weather/64x64/day/113.png";
        g_sunrise_icon = offline ? load_cached_icon(sun_url) : fetch_and_load_icon(sun_url);
    }
    if (!g_sunset_icon) {
        const char *moon_url = "//cdn.weatherapi.com/weather/64x64/night/113.png";
        g_sunset_icon = offline ? load_cached_icon(moon_url) : fetch_and_load_icon(moon_url);
    }
}

static int load_weather_from_cache(int loc_idx) {
    if (loc_idx < 0 || loc_idx >= g_location_count) return -1;
    char *json = load_weather_json(loc_idx);
    if (!json) return -1;

    weather_data_t *weather = &g_weather_cache[loc_idx];
    int rc = parse_weather_data(json, weather);
    free(json);
    if (rc != 0) return -1;

    weather->is_cached = 1;
    get_weather_cache_time(loc_idx, weather->cached_time, sizeof(weather->cached_time));
    load_weather_icons(weather, 1);

    ap_log("weather_cache: loaded cached data for %s (cached at %s)",
           g_locations[loc_idx].name, weather->cached_time);
    return 0;
}

static int fetch_weather_for_location(int loc_idx) {
    if (g_api_key[0] == '\0' || loc_idx < 0 || loc_idx >= g_location_count) return -1;

    location_t *loc = &g_locations[loc_idx];
    weather_data_t *weather = &g_weather_cache[loc_idx];

    char url[MAX_URL];
    if (loc->id > 0)
        snprintf(url, sizeof(url),
                 "https://api.weatherapi.com/v1/forecast.json?key=%s&q=id:%d&days=3&aqi=no",
                 g_api_key, loc->id);
    else if (loc->lat_lon[0])
        snprintf(url, sizeof(url),
                 "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=3&aqi=no",
                 g_api_key, loc->lat_lon);
    else
        snprintf(url, sizeof(url),
                 "https://api.weatherapi.com/v1/forecast.json?key=%s&q=%s&days=3&aqi=no",
                 g_api_key, loc->name);

    char msg[256];
    snprintf(msg, sizeof(msg), "Fetching weather for\n%s...", loc->name);

    int sw = ap_get_screen_width();
    int sh = ap_get_screen_height();
    TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
    ap_color text_color = ap_get_theme()->text;
    int max_w = sw - AP_DS(5) * 8;
    int msg_h = ap_measure_wrapped_text_height(font_small, msg, max_w);
    ap_clear_screen();
    ap_draw_background();
    ap_draw_text_wrapped(font_small, msg, AP_DS(5) * 4, (sh - msg_h) / 2,
                         max_w, text_color, AP_ALIGN_CENTER);
    ap_present();

    fetch_buf_t buf;
    int rc = fetch_url(url, &buf);
    if (rc != 0) return -1;

    rc = parse_weather_data(buf.data, weather);
    if (rc != 0) { free(buf.data); return rc; }

    /* Save raw JSON to offline cache */
    save_weather_json(loc_idx, buf.data);
    free(buf.data);

    weather->is_cached = 0;
    weather->cached_time[0] = '\0';

    load_weather_icons(weather, 0);

    if (strcmp(loc->name, "auto:ip") == 0 && weather->location_name[0]) {
        if (weather->region[0])
            snprintf(loc->name, MAX_LOCATION, "%s, %s",
                     weather->location_name, weather->region);
        else
            snprintf(loc->name, MAX_LOCATION, "%s", weather->location_name);
        snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%.4f,%.4f",
                 weather->loc_lat, weather->loc_lon);
        loc->id = 0;
        save_locations();
    }

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
    pakkit_detail_opts opts = {.title = "Nimbus",.subtitle = "Weather app for NextUI",.info = info,.info_count = 5,.credits = credits,.credit_count = 3,
    };
    pakkit_detail_screen(&opts);
}

/* -----------------------------------------------------------------------
 * Location search + management
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
    pakkit_loading("Searching...");
    fetch_buf_t buf;
    int rc = fetch_url(url, &buf);
    if (rc != 0) return 0;
    cJSON *root = cJSON_Parse(buf.data);
    free(buf.data);
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

static int search_and_add_location(void) {
    if (g_location_count >= MAX_LOCATIONS) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Maximum of %d locations reached.", MAX_LOCATIONS);
        pakkit_message(msg, "OK");
        return -1;
    }
    pakkit_keyboard_opts kb_opts = {.prompt = "City, zip, or postal code" };
    pakkit_keyboard_result kb_result;
    int rc = pakkit_keyboard("", &kb_opts, &kb_result);
    if (rc != AP_OK || kb_result.text[0] == '\0') return -1;
    search_result_t results[MAX_SEARCH_RESULTS];
    int count = search_locations(kb_result.text, results, MAX_SEARCH_RESULTS);
    if (count == 0) {
        pakkit_message("No locations found.\nTry a different search.", "OK");
        return -1;
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
    if (rc != AP_OK || result.selected_index < 0) return -1;
    search_result_t *sel = &results[result.selected_index];
    location_t *loc = &g_locations[g_location_count];
    memset(loc, 0, sizeof(*loc));
    if (sel->region[0])
        snprintf(loc->name, MAX_LOCATION, "%s, %s", sel->name, sel->region);
    else
        snprintf(loc->name, MAX_LOCATION, "%s", sel->name);
    snprintf(loc->lat_lon, sizeof(loc->lat_lon), "%.2f,%.2f", sel->lat, sel->lon);
    loc->id = sel->id;
    loc->is_home = (g_location_count == 0) ? 1 : 0;
    memset(&g_weather_cache[g_location_count], 0, sizeof(weather_data_t));
    g_location_count++;
    save_locations();
    return g_location_count - 1;
}

static void show_location_options(int loc_idx) {
    if (loc_idx < 0 || loc_idx >= g_location_count) return;
    char title[256];
    snprintf(title, sizeof(title), "%s", g_locations[loc_idx].name);
    int item_count = 2;
    pakkit_menu_item items[2];
    items[0] = (pakkit_menu_item){.label = "Set as Home" };
    items[1] = (pakkit_menu_item){.label = "Delete" };
    if (g_location_count <= 1) item_count = 1;
    pakkit_menu_result result;
    int rc = pakkit_menu(title, items, item_count, &result);
    if (rc != AP_OK) return;
    switch (result.selected_index) {
        case 0:
            for (int i = 0; i < g_location_count; i++) g_locations[i].is_home = 0;
            g_locations[loc_idx].is_home = 1;
            save_locations();
            break;
        case 1: {
            char msg[256];
            snprintf(msg, sizeof(msg), "Delete \"%s\"?", g_locations[loc_idx].name);
            if (pakkit_confirm(msg, "Delete", "Cancel")) {
                int was_home = g_locations[loc_idx].is_home;
                free_weather_textures(&g_weather_cache[loc_idx]);
                for (int i = loc_idx; i < g_location_count - 1; i++) {
                    g_locations[i] = g_locations[i + 1];
                    g_weather_cache[i] = g_weather_cache[i + 1];
                }
                g_location_count--;
                memset(&g_weather_cache[g_location_count], 0, sizeof(weather_data_t));
                if (was_home && g_location_count > 0) g_locations[0].is_home = 1;
                if (g_current_location >= g_location_count)
                    g_current_location = g_location_count > 0 ? g_location_count - 1 : 0;
                save_locations();
            }
            break;
        }
    }
}

static void show_locations(void) {
    while (1) {
        static char loc_labels[MAX_LOCATIONS][MAX_LOCATION + 16];
        pakkit_list_item items[MAX_LOCATIONS];
        for (int i = 0; i < g_location_count; i++) {
            if (g_locations[i].is_home)
                snprintf(loc_labels[i], sizeof(loc_labels[i]), "%s (Home)", g_locations[i].name);
            else
                snprintf(loc_labels[i], sizeof(loc_labels[i]), "%s", g_locations[i].name);
            items[i].label = loc_labels[i];
        }
        pakkit_hint hints[] = {
            {.button = "B",.label = "Back" },
            {.button = "X",.label = "Add" },
            {.button = "A",.label = "Options" },
        };
        pakkit_list_opts opts = {.title = "Locations",.hints = hints,.hint_count = 3,.secondary_button = AP_BTN_X,.tertiary_button = AP_BTN_NONE,
        };
        pakkit_list_result result;
        pakkit_list(&opts, items, g_location_count, &result);
        if (result.action == PAKKIT_ACTION_BACK) return;
        if (result.action == PAKKIT_ACTION_SECONDARY) { search_and_add_location(); continue; }
        if (result.selected_index >= 0) show_location_options(result.selected_index);
    }
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
            {.label = "Locations" },
            {.label = "Change API Key" },
            {.label = "About" },
        };
        pakkit_menu_result result;
        int rc = pakkit_menu("Settings", items, 4, &result);
        if (rc != AP_OK) return;
        switch (result.selected_index) {
            case 0: g_settings.use_fahrenheit = !g_settings.use_fahrenheit; settings_save(); break;
            case 1: show_locations(); break;
            case 2: show_api_key_setup(); break;
            case 3: show_about(); break;
        }
    }
}

/* -----------------------------------------------------------------------
 * Weather screen: tab drawing helpers
 * ----------------------------------------------------------------------- */

static void draw_tab_current(weather_data_t *weather, int content_y, int content_h,
                              int scroll_y, int *total_h) {
    int sw = ap_get_screen_width();
    int pad = AP_DS(5);

    TTF_Font *font_xl    = ap_get_font(AP_FONT_EXTRA_LARGE);
    TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
    TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);

    ap_theme *theme = ap_get_theme();
    ap_color text_color = theme->text;
    ap_color hint_color = theme->hint;

    int y = content_y - scroll_y;

    /* Icon + Temperature */
    int icon_size = AP_DS(64);
    int icon_x = pad * 3;
    if (weather->icon_texture)
        ap_draw_image(weather->icon_texture, icon_x, y, icon_size, icon_size);

    char temp_str[32];
    if (g_settings.use_fahrenheit)
        snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0""F", weather->temp_f);
    else
        snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0""C", weather->temp_c);

    int temp_x = icon_x + icon_size + pad * 2;
    int temp_y = y + (icon_size / 2) - TTF_FontHeight(font_xl) / 2 - pad;
    ap_draw_text(font_xl, temp_str, temp_x, temp_y, text_color);
    int cond_y = temp_y + TTF_FontHeight(font_xl) + 2;
    ap_draw_text(font_med, weather->condition_text, temp_x, cond_y, hint_color);
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
        snprintf(feels_str, sizeof(feels_str), "Feels like %.0f\xc2\xb0""F", weather->feels_like_f);
        snprintf(wind_str, sizeof(wind_str), "Wind: %.0f mph %s", weather->wind_mph, weather->wind_dir);
        snprintf(precip_str, sizeof(precip_str), "Precip: %.2f in", weather->precip_in);
    } else {
        snprintf(feels_str, sizeof(feels_str), "Feels like %.0f\xc2\xb0""C", weather->feels_like_c);
        snprintf(wind_str, sizeof(wind_str), "Wind: %.0f km/h %s", weather->wind_kph, weather->wind_dir);
        snprintf(precip_str, sizeof(precip_str), "Precip: %.1f mm", weather->precip_mm);
    }
    snprintf(humidity_str, sizeof(humidity_str), "Humidity: %d%%", weather->humidity);
    snprintf(cloud_str, sizeof(cloud_str), "Cloud: %d%%", weather->cloud);
    snprintf(uv_str, sizeof(uv_str), "UV: %.0f", weather->uv);

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
    if (weather->forecast_count > 0) {
        forecast_day_t *today = &weather->forecast[0];
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
    }

    y += pad * 2;

    *total_h = y + scroll_y - content_y;
}

static void draw_tab_forecast(weather_data_t *weather, int content_y, int content_h,
                               int scroll_y, int *total_h) {
    int sw = ap_get_screen_width();
    int pad = AP_DS(5);

    TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
    TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
    TTF_Font *font_tiny  = ap_get_font(AP_FONT_TINY);

    ap_theme *theme = ap_get_theme();
    ap_color text_color = theme->text;
    ap_color hint_color = theme->hint;

    int y = content_y - scroll_y;
    int col1_x = pad * 3;

    /* Section title */
    ap_draw_text(font_med, "3-Day Forecast", col1_x, y, hint_color);
    y += TTF_FontHeight(font_med) + pad * 2;

    int fc_icon_size = AP_DS(40);
    for (int i = 0; i < weather->forecast_count; i++) {
        forecast_day_t *day = &weather->forecast[i];
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

        /* Divider between days */
        if (i < weather->forecast_count - 1) {
            ap_draw_rect(pad * 3, y, sw - pad * 6, 1, hint_color);
            y += pad * 2;
        }
    }

    y += pad * 2;
    *total_h = y + scroll_y - content_y;
}

static void draw_tab_hourly(weather_data_t *weather, int content_y, int content_h,
                             int scroll_y, int *total_h) {
    int sw = ap_get_screen_width();
    int pad = AP_DS(5);

    TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
    TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
    TTF_Font *font_tiny  = ap_get_font(AP_FONT_TINY);

    ap_theme *theme = ap_get_theme();
    ap_color text_color = theme->text;
    ap_color hint_color = theme->hint;

    int y = content_y - scroll_y;
    int col1_x = pad * 3;

    /* Section title */
    ap_draw_text(font_med, "Hourly", col1_x, y, hint_color);
    y += TTF_FontHeight(font_med) + pad * 2;

    int icon_size = AP_DS(28);
    int row_h = icon_size + pad;

    int start_hour = 0;
    if (weather->last_updated[0]) {
        int cur_hour = 0;
        const char *space = strchr(weather->last_updated, ' ');
        if (space) sscanf(space + 1, "%d", &cur_hour);
        char date_prefix[16] = {0};
        strncpy(date_prefix, weather->last_updated, 10);
        for (int i = 0; i < weather->hour_count; i++) {
            int h_hour = 0;
            const char *hs = strchr(weather->hours[i].time, ' ');
            if (hs) sscanf(hs + 1, "%d", &h_hour);
            if (strncmp(weather->hours[i].time, date_prefix, 10) == 0 && h_hour >= cur_hour) {
                start_hour = i;
                break;
            }
        }
    }

    int shown = 0;
    for (int i = start_hour; i < weather->hour_count && shown < 24; i++, shown++) {
        hourly_t *hr = &weather->hours[i];

        if (hr->icon_texture)
            ap_draw_image(hr->icon_texture, col1_x, y, icon_size, icon_size);

        int text_x = col1_x + icon_size + pad * 2;
        int text_y = y + (icon_size - TTF_FontHeight(font_small)) / 2;

        char temp_str[32];
        if (g_settings.use_fahrenheit)
            snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0""F", hr->temp_f);
        else
            snprintf(temp_str, sizeof(temp_str), "%.0f\xc2\xb0""C", hr->temp_c);

        /* Reserve space for rain % on right */
        int rain_reserve = AP_DS(35);
        int max_text_w = sw - text_x - pad * 3 - rain_reserve;

        char line[128];
        snprintf(line, sizeof(line), "%-6s  %s  %s", hr->hour_label, temp_str, hr->condition_text);
        ap_draw_text_ellipsized(font_small, line, text_x, text_y, text_color, max_text_w);

        if (hr->chance_rain > 0) {
            char rain[16];
            snprintf(rain, sizeof(rain), "%d%%", hr->chance_rain);
            int rain_w = ap_measure_text(font_tiny, rain);
            ap_draw_text(font_tiny, rain, sw - rain_w - pad * 3,
                         text_y + (TTF_FontHeight(font_small) - TTF_FontHeight(font_tiny)) / 2,
                         hint_color);
        }

        y += row_h;
    }

    if (shown == 0) {
        ap_draw_text(font_small, "No hourly data available", pad * 3, y, hint_color);
        y += TTF_FontHeight(font_small) + pad;
    }

    *total_h = y + scroll_y - content_y;
}

static void draw_tab_astro(weather_data_t *weather, int content_y, int content_h,
                            int scroll_y, int *total_h) {
    int sw = ap_get_screen_width();
    int pad = AP_DS(5);

    TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
    TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);

    ap_theme *theme = ap_get_theme();
    ap_color text_color = theme->text;
    ap_color hint_color = theme->hint;

    int y = content_y - scroll_y;
    int col1_x = pad * 3;
    int val_x = pad * 3 + AP_DS(100);
    int row_h = TTF_FontHeight(font_small) + pad;

    /* Section title */
    ap_draw_text(font_med, "Sun & Moon", col1_x, y, hint_color);
    y += TTF_FontHeight(font_med) + pad * 2;

    for (int d = 0; d < weather->forecast_count; d++) {
        forecast_day_t *day = &weather->forecast[d];

        char header[64];
        if (d == 0) snprintf(header, sizeof(header), "Today  (%s)", day->date);
        else snprintf(header, sizeof(header), "%s  (%s)", day->day_name, day->date);
        ap_draw_text(font_small, header, col1_x, y, text_color);
        y += TTF_FontHeight(font_small) + pad;

        if (day->sunrise[0]) {
            int sun_icon_size = TTF_FontHeight(font_small);
            if (g_sunrise_icon)
                ap_draw_image(g_sunrise_icon, col1_x, y, sun_icon_size, sun_icon_size);
            ap_draw_text(font_small, "Sunrise", col1_x + sun_icon_size + pad, y, hint_color);
            ap_draw_text(font_small, day->sunrise, val_x, y, text_color);
            y += row_h;
        }
        if (day->sunset[0]) {
            int sun_icon_size = TTF_FontHeight(font_small);
            if (g_sunset_icon)
                ap_draw_image(g_sunset_icon, col1_x, y, sun_icon_size, sun_icon_size);
            ap_draw_text(font_small, "Sunset", col1_x + sun_icon_size + pad, y, hint_color);
            ap_draw_text(font_small, day->sunset, val_x, y, text_color);
            y += row_h;
        }

        if (day->sunrise[0] && day->sunset[0]) {
            int sr_h = 0, sr_m = 0, ss_h = 0, ss_m = 0;
            char sr_ampm[4] = {0}, ss_ampm[4] = {0};
            sscanf(day->sunrise, "%d:%d %2s", &sr_h, &sr_m, sr_ampm);
            sscanf(day->sunset, "%d:%d %2s", &ss_h, &ss_m, ss_ampm);
            if (sr_ampm[0] == 'P' && sr_h != 12) sr_h += 12;
            if (sr_ampm[0] == 'A' && sr_h == 12) sr_h = 0;
            if (ss_ampm[0] == 'P' && ss_h != 12) ss_h += 12;
            if (ss_ampm[0] == 'A' && ss_h == 12) ss_h = 0;
            int sr_mins = sr_h * 60 + sr_m;
            int ss_mins = ss_h * 60 + ss_m;
            int day_len = ss_mins - sr_mins;
            if (day_len > 0) {
                char daylen[32];
                snprintf(daylen, sizeof(daylen), "%dh %dm", day_len / 60, day_len % 60);
                ap_draw_text(font_small, "Day Length", col1_x, y, hint_color);
                ap_draw_text(font_small, daylen, val_x, y, text_color);
                y += row_h;
            }
        }

        y += pad;

        if (day->moonrise[0]) {
            ap_draw_text(font_small, "Moonrise", col1_x, y, hint_color);
            ap_draw_text(font_small, day->moonrise, val_x, y, text_color);
            y += row_h;
        }
        if (day->moonset[0]) {
            ap_draw_text(font_small, "Moonset", col1_x, y, hint_color);
            ap_draw_text(font_small, day->moonset, val_x, y, text_color);
            y += row_h;
        }
        if (day->moon_phase[0]) {
            ap_draw_text(font_small, "Phase", col1_x, y, hint_color);
            ap_draw_text(font_small, day->moon_phase, val_x, y, text_color);
            y += row_h;
        }
        if (day->moon_illumination > 0) {
            char illum[16];
            snprintf(illum, sizeof(illum), "%d%%", day->moon_illumination);
            ap_draw_text(font_small, "Illumination", col1_x, y, hint_color);
            ap_draw_text(font_small, illum, val_x, y, text_color);
            y += row_h;
        }

        y += pad;
        if (d < weather->forecast_count - 1) {
            ap_draw_rect(pad * 3, y, sw - pad * 6, 1, hint_color);
            y += pad * 2;
        }
    }

    *total_h = y + scroll_y - content_y;
}

/* -----------------------------------------------------------------------
 * Page indicator dots (drawn with circles)
 * ----------------------------------------------------------------------- */

static void draw_page_dots(int count, int active, int x, int y) {
    ap_theme *theme = ap_get_theme();
    ap_color active_color = theme->text;
    ap_color inactive_color = { theme->hint.r, theme->hint.g, theme->hint.b, 80 };

    int dot_r = AP_DS(3);
    int dot_spacing = dot_r * 4;

    for (int i = 0; i < count; i++) {
        int dx = x + i * dot_spacing;
        ap_color c = (i == active) ? active_color : inactive_color;
        ap_draw_circle(dx, y + dot_r, dot_r, c);
    }
}

/* -----------------------------------------------------------------------
 * Screens: Weather display with page dots
 * ----------------------------------------------------------------------- */

static void show_weather_screen(void) {
    int running = 1;
    pakkit_scroll_state scroll = {0};
    int active_page = TAB_CURRENT;

    while (running) {
        weather_data_t *weather = &g_weather_cache[g_current_location];

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
                            if (g_location_count > 0) {
                                if (g_current_location >= g_location_count)
                                    g_current_location = get_home_index();
                                if (check_wifi()) {
                                    fetch_weather_for_location(g_current_location);
                                } else {
                                    load_weather_from_cache(g_current_location);
                                }
                            }
                            scroll = (pakkit_scroll_state){0};
                        }
                        break;
                    case AP_BTN_LEFT:
                        if (!ev.repeated) {
                            active_page--;
                            if (active_page < 0) active_page = TAB_COUNT - 1;
                            scroll = (pakkit_scroll_state){0};
                        }
                        break;
                    case AP_BTN_RIGHT:
                        if (!ev.repeated) {
                            active_page++;
                            if (active_page >= TAB_COUNT) active_page = 0;
                            scroll = (pakkit_scroll_state){0};
                        }
                        break;
                    case AP_BTN_L1:
                        if (!ev.repeated && g_location_count > 1) {
                            int next = g_current_location - 1;
                            if (next < 0) next = g_location_count - 1;
                            /* Try network fetch first, fall back to cache */
                            if (check_wifi()) {
                                free_weather_textures(&g_weather_cache[next]);
                                memset(&g_weather_cache[next], 0, sizeof(weather_data_t));
                                if (fetch_weather_for_location(next) == 0) {
                                    g_current_location = next;
                                } else if (load_weather_from_cache(next) == 0) {
                                    g_current_location = next;
                                } else {
                                    pakkit_message("Could not load weather.\nCheck connection and try again.", "OK");
                                }
                            } else if (load_weather_from_cache(next) == 0) {
                                g_current_location = next;
                            } else {
                                pakkit_message("No WiFi and no cached data\nfor this location.", "OK");
                            }
                            scroll = (pakkit_scroll_state){0};
                        }
                        break;
                    case AP_BTN_R1:
                        if (!ev.repeated && g_location_count > 1) {
                            int next = g_current_location + 1;
                            if (next >= g_location_count) next = 0;
                            /* Try network fetch first, fall back to cache */
                            if (check_wifi()) {
                                free_weather_textures(&g_weather_cache[next]);
                                memset(&g_weather_cache[next], 0, sizeof(weather_data_t));
                                if (fetch_weather_for_location(next) == 0) {
                                    g_current_location = next;
                                } else if (load_weather_from_cache(next) == 0) {
                                    g_current_location = next;
                                } else {
                                    pakkit_message("Could not load weather.\nCheck connection and try again.", "OK");
                                }
                            } else if (load_weather_from_cache(next) == 0) {
                                g_current_location = next;
                            } else {
                                pakkit_message("No WiFi and no cached data\nfor this location.", "OK");
                            }
                            scroll = (pakkit_scroll_state){0};
                        }
                        break;
                    case AP_BTN_UP:
                        pakkit_scroll_handle_input(&scroll, -1, SCROLL_STEP);
                        break;
                    case AP_BTN_DOWN:
                        pakkit_scroll_handle_input(&scroll, 1, SCROLL_STEP);
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

        TTF_Font *font_med   = ap_get_font(AP_FONT_MEDIUM);
        TTF_Font *font_tiny  = ap_get_font(AP_FONT_TINY);

        ap_theme *theme = ap_get_theme();
        ap_color hint_color = theme->hint;

        int hint_font_h = TTF_FontHeight(font_tiny);
        int footer_h = hint_font_h + pad * 2;

        /* Header line: location name (left) + dots (right) */
        int y = pad * 3;

        if (weather->valid) {
            char location_full[512];
            if (weather->region[0])
                snprintf(location_full, sizeof(location_full), "%s, %s",
                         weather->location_name, weather->region);
            else
                snprintf(location_full, sizeof(location_full), "%s",
                         weather->location_name);

            char header_text[640];
            if (g_location_count > 1) {
                snprintf(header_text, sizeof(header_text), "%s (%d/%d)",
                         location_full, g_current_location + 1, g_location_count);
            } else {
                snprintf(header_text, sizeof(header_text), "%s", location_full);
            }

            /* Ellipsize header to leave room for dots */
            int dot_r = AP_DS(3);
            int dot_spacing = dot_r * 4;
            int dots_total_w = (TAB_COUNT - 1) * dot_spacing + dot_r * 2;
            int max_header_w = sw - pad * 3 - dots_total_w - pad * 3;
            ap_draw_text_ellipsized(font_med, header_text, pad * 3, y, hint_color, max_header_w);
        } else {
            ap_draw_text(font_med, "Nimbus", pad * 3, y, hint_color);
        }

        /* Dots right-aligned on same line */
        int dot_r = AP_DS(3);
        int dot_spacing = dot_r * 4;
        int dots_total_w = (TAB_COUNT - 1) * dot_spacing;
        int dots_x = sw - pad * 3 - dots_total_w;
        int dots_y = y + (TTF_FontHeight(font_med) - dot_r * 2) / 2;
        draw_page_dots(TAB_COUNT, active_page, dots_x, dots_y);

        y += TTF_FontHeight(font_med) + pad * 2;

        /* Divider */
        ap_draw_rect(pad * 3, y, sw - pad * 6, 1, hint_color);
        y += pad;

        /* Cached indicator — below divider, right-aligned */
        if (weather->valid && weather->is_cached && weather->cached_time[0]) {
            char cached_label[64];
            snprintf(cached_label, sizeof(cached_label), "Cached %s", weather->cached_time);
            int cached_w = ap_measure_text(font_tiny, cached_label);
            ap_draw_text(font_tiny, cached_label, sw - cached_w - pad * 3, y, hint_color);
            y += TTF_FontHeight(font_tiny) + pad;
        }

        int content_y = y;
        int content_bottom = sh - footer_h;
        int content_h = content_bottom - content_y;

        SDL_Rect clip = { 0, content_y, sw, content_h };
        SDL_RenderSetClipRect(ap__g.renderer, &clip);

        if (!weather->valid) {
            scroll.scroll_y = 0;
            ap_draw_text(font_med, "No weather data", pad * 3,
                         content_y + content_h / 3, hint_color);
            TTF_Font *font_small = ap_get_font(AP_FONT_SMALL);
            ap_draw_text(font_small, "Press Y for settings",
                         pad * 3, content_y + content_h / 3 + TTF_FontHeight(font_med) + pad,
                         hint_color);
        } else {
            int total_h = 0;
            switch (active_page) {
                case TAB_CURRENT:
                    draw_tab_current(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
                case TAB_FORECAST:
                    draw_tab_forecast(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
                case TAB_HOURLY:
                    draw_tab_hourly(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
                case TAB_ASTRO:
                    draw_tab_astro(weather, content_y, content_h, scroll.scroll_y, &total_h);
                    break;
            }

            /* Update cached max_scroll for next frame's input clamping */
            pakkit_scroll_update(&scroll, total_h, content_h);
        }

        SDL_RenderSetClipRect(ap__g.renderer, NULL);

        /* Hints */

        if (g_location_count > 1) {
            pakkit_hint hints[] = {
                {.button = "B",.label = "Quit" },
                {.button = "L/R",.label = "View" },
                {.button = "L1/R1",.label = "City" },
                {.button = "Y",.label = "Menu" },
            };
            pakkit_draw_hints(hints, 4);
        } else {
            pakkit_hint hints[] = {
                {.button = "B",.label = "Quit" },
                {.button = "L/R",.label = "View" },
                {.button = "Y",.label = "Menu" },
            };
            pakkit_draw_hints(hints, 3);
        }

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

    /* Splash screen */
    {
        const char *pak_dir = getenv("NIMBUS_PAK_DIR");
        char splash_path[MAX_PATH_LEN];
        if (pak_dir)
            snprintf(splash_path, sizeof(splash_path), "%s/res/splash.png", pak_dir);
        else
            snprintf(splash_path, sizeof(splash_path), "res/splash.png");
        SDL_Texture *splash = ap_load_image(splash_path);
        if (splash) {
            int sw = ap_get_screen_width();
            int sh = ap_get_screen_height();
            int img_w, img_h;
            SDL_QueryTexture(splash, NULL, NULL, &img_w, &img_h);
            int max_w = sw - AP_DS(5) * 8;
            int max_h = sh - AP_DS(5) * 8;
            float scale_w = (float)max_w / (float)img_w;
            float scale_h = (float)max_h / (float)img_h;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;
            int draw_w = (int)(img_w * scale);
            int draw_h = (int)(img_h * scale);
            int x = (sw - draw_w) / 2;
            int y = (sh - draw_h) / 2;
            ap_clear_screen();
            ap_draw_background();
            ap_draw_image(splash, x, y, draw_w, draw_h);
            ap_present();
            int waited = 0;
            while (waited < 1000) {
                ap_input_event ev;
                while (ap_poll_input(&ev)) {
                    if (ev.pressed && !ev.repeated) waited = 1000;
                }
                SDL_Delay(16);
                waited += 16;
            }
            SDL_DestroyTexture(splash);
        }
    }

    ap_log("=== Nimbus v%s starting ===", NIMBUS_VERSION);
    memset(g_weather_cache, 0, sizeof(g_weather_cache));

    if (load_api_key() != 0) {
        if (show_api_key_setup() != 0) {
            ap_quit(); curl_global_cleanup(); return 0;
        }
    }

    if (load_locations() != 0) {
        g_locations[0] = (location_t){.is_home = 1 };
        snprintf(g_locations[0].name, MAX_LOCATION, "auto:ip");
        g_location_count = 1;
    }

    g_current_location = get_home_index();

    /* WiFi check with retry/continue + offline cache fallback */
    if (!check_wifi()) {
        /* Try loading from offline cache */
        int has_cache = (load_weather_from_cache(g_current_location) == 0);

        while (!check_wifi()) {
            if (has_cache) {
                int retry = pakkit_confirm(
                    "No WiFi connection.\nCached weather data available.",
                    "Retry", "Continue");
                if (!retry) break; /* Continue with cached data */
            } else {
                int retry = pakkit_confirm(
                    "No WiFi connection.\nNo cached data available.",
                    "Retry", "Quit");
                if (!retry) {
                    ap_quit(); curl_global_cleanup(); return 0;
                }
            }
        }

        if (check_wifi()) {
            /* WiFi came back — fetch fresh data */
            fetch_weather_for_location(g_current_location);
        }
        /* else: continuing with cached data already loaded */
    } else {
        /* WiFi available — fetch normally */
        fetch_weather_for_location(g_current_location);
    }

    show_weather_screen();

    for (int i = 0; i < g_location_count; i++)
        free_weather_textures(&g_weather_cache[i]);
    if (g_sunrise_icon) SDL_DestroyTexture(g_sunrise_icon);
    if (g_sunset_icon) SDL_DestroyTexture(g_sunset_icon);

    ap_log("=== Nimbus shutting down ===");
    ap_quit();
    curl_global_cleanup();
    return 0;
}