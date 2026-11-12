/*

ESPectrum Fork — NetworkMenu.cpp
Menu de Acesso à Rede: WiFi + cliente FTP browser.

Configurações salvas em NVS namespace "storage",
o mesmo usado pelo ESPConfig.cpp do projeto.

Coloque em: src/NetworkMenu.cpp
Inclua em OSDMain.cpp: #include "NetworkMenu.h"

*/

#include "NetworkMenu.h"
#include "OSDMain.h"
#include "ESPConfig.h"
#include "FileUtils.h"
#include "Snapshot.h"
#include "Tape.h"
#include "Video.h"
#include "messages.h"

#include "esp_heap_caps.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <fcntl.h>
#include <sys/select.h>

#include <stdio.h>
#include <string.h>
#include <cctype>
#include <cerrno>
#include <string>
#include <vector>

#include <dirent.h>
#include "esp_http_client.h"
// miniz nao e mais necessario — descompressao feita no proxy PHP

using namespace std;

// ─────────────────────────────────────────────────────────────────────────────
// Chaves NVS — namespace "storage" (mesmo do ESPConfig)
// ─────────────────────────────────────────────────────────────────────────────
#define NVS_NS        "storage"
#define NVS_WIFI_SSID "net_ssid"
#define NVS_WIFI_PASS "net_pass"
#define NVS_PROXY_URL "proxy_url"

// ─────────────────────────────────────────────────────────────────────────────
// Statics
// ─────────────────────────────────────────────────────────────────────────────
bool NetworkMenu::wifi_connected = false;

static EventGroupHandle_t s_wifi_eg = nullptr;
#define WIFI_CONN_BIT BIT0
#define WIFI_FAIL_BIT BIT1


static bool s_wifi_stack_ready = false;
static bool s_netif_sta_ready = false;
static bool s_wifi_handlers_registered = false;
static int  s_wifi_pause_depth = 0;
static bool s_wifi_stopped_for_sd = false;
static bool s_sd_released_for_net = false;
static bool s_network_session = false;
static bool s_wifi_awaiting_ip = false;
static bool s_exit_osd_after_load = false;

// Cache de credenciais WiFi lidas do SD antes de desmontar
static string s_cached_ssid = "";
static string s_cached_pass = "";
static bool   s_wifi_cache_loaded = false;


static uint8_t *netAllocBuf(size_t sz) {
    uint8_t *p = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = (uint8_t *)malloc(sz);
    return p;
}

static void netFreeBuf(void *p) {
    if (p) heap_caps_free(p);
}

static bool netFileIsGame(const string &path) {
    const string ext = FileUtils::getLCaseExt(path);
    return ext == "z80" || ext == "sna" || ext == "tap" || ext == "tzx" || ext == "p" || ext == "sp";
}

static string netFtpDestPath(const string &filename) {
    const string ext = FileUtils::getLCaseExt(filename);
    string sub = FileUtils::SNA_Path;
    if (ext == "tap" || ext == "tzx") sub = FileUtils::TAP_Path;
    else if (ext == "dsk") sub = FileUtils::DSK_Path;
    string dest = FileUtils::MountPoint + sub;
    if (dest.back() != '/') dest += '/';
    dest += filename;
    return dest;
}



static bool netLoadDownloadedFile(const string &dest) {
    const string ext = FileUtils::getLCaseExt(dest);

    if (ext == "tap" || ext == "tzx") {
        string tapBase = FileUtils::MountPoint + FileUtils::TAP_Path;
        if (tapBase.back() != '/') tapBase += '/';

        if (dest.compare(0, tapBase.size(), tapBase) != 0) {
            ESP_LOGE("FTP", "Fita fora de TAP_Path (esperado sob %s): %s", tapBase.c_str(), dest.c_str());
            return false;
        }
        const string rel = dest.substr(tapBase.size());

        const bool canFlash = (ext == "tap") && Config::flashload
            && (Config::romSet != "ZX81+") && (Config::romSet != "48Kcs")
            && (Config::romSet != "128Kcs") && (Config::romSet != "TKcs");
        const string prefix = canFlash ? "R" : "S";
        Tape::LoadTape(prefix + rel);
        ESP_LOGI("FTP", "Fita carregada (%s): %s%s", prefix.c_str(), tapBase.c_str(), rel.c_str());
        return true;
    }

    if (ext == "sna" || ext == "z80" || ext == "sp" || ext == "p" || ext == "esp") {
        const bool ok = LoadSnapshot(dest, "", "", 0xff);
        if (ok) {
            Config::ram_file = dest;
            Config::last_ram_file = dest;
            ESP_LOGI("FTP", "Snapshot carregado: %s", dest.c_str());
        } else {
            ESP_LOGE("FTP", "LoadSnapshot falhou: %s", dest.c_str());
        }
        return ok;
    }

    ESP_LOGW("FTP", "Extensao nao suportada para carregar: %s", ext.c_str());
    return false;
}

bool NetworkMenu::shouldExitOsdAfterLoad() {
    return s_exit_osd_after_load;
}

void NetworkMenu::clearExitOsdAfterLoad() {
    s_exit_osd_after_load = false;
}



// Grava no SD com WiFi parado durante toda a escrita (WiFi nao pode voltar apos mount).
static bool netFlushToSd(const string &local, const uint8_t *data, size_t len, bool &first) {
    if (len == 0) return true;

    NetworkMenu::wifiPauseForSd();
    vTaskDelay(pdMS_TO_TICKS(50));

    if (!FileUtils::SDReady) {
        FileUtils::initFileSystem(true);
    }
    if (!FileUtils::SDReady) {
        ESP_LOGE("FTP", "SD indisponivel para escrita");
        NetworkMenu::wifiResumeAfterSd();
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(30));

    FILE *f = fopen(local.c_str(), first ? "wb" : "ab");
    if (!f) {
        ESP_LOGE("FTP", "fopen falhou: %s (errno %d)", local.c_str(), errno);
        if (NetworkMenu::isNetworkSession()) FileUtils::unmountSDCard();
        NetworkMenu::wifiResumeAfterSd();
        return false;
    }

    size_t written = 0;
    const size_t chunk = 4096;
    while (written < len) {
        size_t n = len - written;
        if (n > chunk) n = chunk;
        size_t w = fwrite(data + written, 1, n, f);
        written += w;
        if (w != n) {
            ESP_LOGE("FTP", "fwrite falhou em %u/%u (errno %d)", (unsigned)written, (unsigned)len, errno);
            fclose(f);
            if (NetworkMenu::isNetworkSession()) FileUtils::unmountSDCard();
            NetworkMenu::wifiResumeAfterSd();
            return false;
        }
        vTaskDelay(1);
    }
    fflush(f);
    fclose(f);
    first = false;

    ESP_LOGI("FTP", "Gravado no SD: %u bytes -> %s", (unsigned)len, local.c_str());

    if (NetworkMenu::isNetworkSession()) {
        FileUtils::unmountSDCard();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    NetworkMenu::wifiResumeAfterSd();
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// NVS helpers
// ═════════════════════════════════════════════════════════════════════════════

static string sanitizeWifiText(const string &s, size_t maxLen) {
    string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c >= 32 && c <= 126) {
            out += (char)c;
            if (out.size() >= maxLen) break;
        }
    }
    return out;
}

static string sanitizeHost(const string &s) {
    string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '.' || c == '-' || c == '_') {
            out += (char)c;
            if (out.size() >= 64) break;
        }
    }
    return out;
}

static bool wifiHasIp() {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

static string nvsGetStr(nvs_handle_t h, const char *key, const char *def = "") {
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0) return def;
    char *buf = new char[len];
    if (!buf) return def;
    size_t buflen = len;
    if (nvs_get_str(h, key, buf, &buflen) != ESP_OK) {
        delete[] buf;
        return def;
    }
    string s(buf);
    delete[] buf;
    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
// Config — NVS load / save
// ═════════════════════════════════════════════════════════════════════════════

// Lê /sd/wificonfig.rc enquanto SD ainda está montado e cacheia em memória.
// Deve ser chamado ANTES de prepareForNetwork() / desmonte do SD.
void NetworkMenu::cacheWifiFromSd() {
    if (s_wifi_cache_loaded) return; // ja leu

    // Se SD nao estiver montado, tenta montar agora (fallback MISO incluido)
    if (!FileUtils::SDReady) {
        ESP_LOGI("NET", "cacheWifiFromSd: SD desmontado, tentando montar...");
        FileUtils::initFileSystem(false);
    }

    if (!FileUtils::SDReady) {
        ESP_LOGW("NET", "cacheWifiFromSd: SD indisponivel, sem credenciais do arquivo");
        return;
    }

    FILE *rc = fopen("/sd/wificonfig.rc", "r");
    if (!rc) {
        ESP_LOGW("NET", "wificonfig.rc nao encontrado no SD");
        return;
    }
    char line[128];
    if (fgets(line, sizeof(line), rc)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        s_cached_ssid = sanitizeWifiText(string(line), 32);
    }
    if (fgets(line, sizeof(line), rc)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = 0;
        s_cached_pass = sanitizeWifiText(string(line), 64);
    }
    fclose(rc);
    s_wifi_cache_loaded = true;
    ESP_LOGI("NET", "WiFi cacheado do SD: ssid='%s'", s_cached_ssid.c_str());
}

NetworkMenu::NetConfig NetworkMenu::loadConfig() {
    NetConfig cfg;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        cfg.wifi_ssid = sanitizeWifiText(nvsGetStr(h, NVS_WIFI_SSID, ""), 32);
        cfg.wifi_pass = sanitizeWifiText(nvsGetStr(h, NVS_WIFI_PASS, ""), 64);
        cfg.proxy_url = nvsGetStr(h, NVS_PROXY_URL, "http://alternativebits.com/wos.php");
        nvs_close(h);
    }

    // Se temos cache do SD, ele tem prioridade sobre NVS
    if (s_wifi_cache_loaded && !s_cached_ssid.empty()) {
        cfg.wifi_ssid = s_cached_ssid;
        cfg.wifi_pass = s_cached_pass;
    }

    return cfg;
}

void NetworkMenu::saveConfig(const NetConfig &cfg) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    string ssid = sanitizeWifiText(cfg.wifi_ssid, 32);
    string pass = sanitizeWifiText(cfg.wifi_pass, 64);
    nvs_set_str(h, NVS_WIFI_SSID, ssid.c_str());
    nvs_set_str(h, NVS_WIFI_PASS, pass.c_str());
    nvs_set_str(h, NVS_PROXY_URL,  cfg.proxy_url.c_str());

    nvs_commit(h);
    nvs_close(h);
}

// ═════════════════════════════════════════════════════════════════════════════
// WiFi
// ═════════════════════════════════════════════════════════════════════════════

static bool s_wifi_msg_overlay_saved = false;

static void showWifiStatusOverlay(const string &msg, uint8_t level) {
    const unsigned short h = OSD_FONT_H * 3;
    const unsigned short y = OSD::scrAlignCenterY(h);
    string m = msg;
    if (m.length() > (OSD::scrW / 6) - 10) m = m.substr(0, (OSD::scrW / 6) - 10);
    const unsigned short w = (m.length() + 2) * OSD_FONT_W;
    const unsigned short x = OSD::scrAlignCenterX(w);
    OSD::saveBackbufferData(x, y, w, h, true);
    s_wifi_msg_overlay_saved = true;
    OSD::osdCenteredMsg(msg, level, 0);
}

static void hideWifiStatusOverlay() {
    if (s_wifi_msg_overlay_saved) {
        OSD::restoreBackbufferData(true);
        s_wifi_msg_overlay_saved = false;
    }
}

static void wifi_evt_handler(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI("NET", "WIFI_EVENT_STA_START");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI("NET", "WIFI_EVENT_STA_CONNECTED");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            NetworkMenu::wifi_connected = false;
            auto *ev = (wifi_event_sta_disconnected_t *)data;
            int reason = ev ? ev->reason : -1;
            ESP_LOGW("NET", "WIFI_EVENT_STA_DISCONNECTED reason=%d", reason);
            if (s_wifi_awaiting_ip) {
                esp_wifi_connect();
                break;
            }
            if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = (ip_event_got_ip_t *)data;
        if (ev) {
            ESP_LOGI("NET", "GOT_IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        }
        NetworkMenu::wifi_connected = true;
        if (s_wifi_eg) xEventGroupSetBits(s_wifi_eg, WIFI_CONN_BIT);
    }
}

bool NetworkMenu::wifiInitOnce() {
    if (s_wifi_stack_ready) return true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("NET", "esp_netif_init: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("NET", "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return false;
    }

    if (!s_netif_sta_ready) {
        if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr) {
            if (esp_netif_create_default_wifi_sta() == nullptr) {
                ESP_LOGE("NET", "esp_netif_create_default_wifi_sta failed");
                return false;
            }
        }
        s_netif_sta_ready = true;
    }

    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&icfg);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW("NET", "esp_wifi_init: already initialized");
    } else if (err != ESP_OK) {
        ESP_LOGE("NET", "esp_wifi_init: %s (reduza buffers WiFi no sdkconfig se malloc falhar)", esp_err_to_name(err));
        return false;
    }

    if (!s_wifi_handlers_registered) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    wifi_evt_handler, nullptr);
        esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, wifi_evt_handler, nullptr);
        s_wifi_handlers_registered = true;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    s_wifi_stack_ready = true;
    return true;
}

void NetworkMenu::wifiPauseForSd() {
    if (s_wifi_pause_depth == 0) {
        if (s_wifi_stack_ready) {
            esp_err_t err = esp_wifi_stop();
            if (err == ESP_OK) {
                s_wifi_stopped_for_sd = true;
                wifi_connected = false;
            } else if (err == ESP_ERR_WIFI_NOT_STARTED) {
                s_wifi_stopped_for_sd = false;
            }
        }
    }
    s_wifi_pause_depth++;
}

void NetworkMenu::wifiResumeAfterSd() {
    if (s_wifi_pause_depth <= 0) return;
    s_wifi_pause_depth--;
    if (s_wifi_pause_depth > 0) return;
    if (!s_wifi_stopped_for_sd) return;

    s_wifi_stopped_for_sd = false;
    esp_wifi_start();
    /* connect em WIFI_EVENT_STA_START */
}

bool NetworkMenu::isNetworkSession() {
    return s_network_session;
}

void NetworkMenu::prepareForNetwork() {
    s_network_session = true;
    s_wifi_pause_depth = 0;
    s_wifi_stopped_for_sd = false;
    if (FileUtils::SDReady) {
        ESP_LOGI("NET", "Desmontando SD para libertar SPI (WiFi/rede)");
        FileUtils::unmountSDCard();
        s_sd_released_for_net = true;
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        ESP_LOGI("NET", "Menu rede (SD ja desmontado)");
    }
}

/*void NetworkMenu::restoreAfterNetwork() {
    s_network_session = false;
    if (!s_sd_released_for_net) return;
    s_sd_released_for_net = false;
    ESP_LOGI("NET", "Remontando SD apos menu rede");
    FileUtils::initFileSystem();
}*/

void NetworkMenu::restoreAfterNetwork() {
    ESP_LOGI("NET", "restoreAfterNetwork: s_sd_released=%d s_network=%d SDReady=%d",
        s_sd_released_for_net ? 1 : 0,
        s_network_session ? 1 : 0,
        FileUtils::SDReady ? 1 : 0);
    s_network_session = false;
    if (!s_sd_released_for_net) return;
    s_sd_released_for_net = false;
    ESP_LOGI("NET", "Remontando SD...");
    FileUtils::initFileSystem();
    ESP_LOGI("NET", "SDReady apos remonte: %d", FileUtils::SDReady ? 1 : 0);
}

bool NetworkMenu::wifiConnect(const string &ssid, const string &pass) {
    if (wifi_connected && wifiHasIp()) return true;

    if (FileUtils::SDReady && !s_network_session) {
        prepareForNetwork();
    }

    const string clean_ssid = sanitizeWifiText(ssid, 32);
    const string clean_pass = sanitizeWifiText(pass, 64);
    if (clean_ssid.empty()) {
        ESP_LOGE("NET", "SSID vazio ou invalido");
        return false;
    }
    if (!wifiInitOnce()) return false;

    showWifiStatusOverlay(NET_MSG_WIFI_CONNECTING[Config::lang], LEVEL_INFO);

    s_wifi_eg = xEventGroupCreate();
    if (!s_wifi_eg) {
        ESP_LOGE("NET", "EventGroup create failed");
        hideWifiStatusOverlay();
        return false;
    }
    xEventGroupClearBits(s_wifi_eg, WIFI_CONN_BIT | WIFI_FAIL_BIT);
    s_wifi_awaiting_ip = true;

    wifi_config_t wcfg = {};
    memset(&wcfg, 0, sizeof(wcfg));
    memcpy(wcfg.sta.ssid, clean_ssid.c_str(), clean_ssid.length());
    memcpy(wcfg.sta.password, clean_pass.c_str(), clean_pass.length());
    if (clean_pass.empty()) {
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI("NET", "Conectando SSID='%s' len=%u (pass %s)",
        clean_ssid.c_str(), (unsigned)clean_ssid.length(),
        clean_pass.empty() ? "vazia" : "ok");

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) {
        ESP_LOGE("NET", "esp_wifi_set_config: %s", esp_err_to_name(err));
        s_wifi_awaiting_ip = false;
        vEventGroupDelete(s_wifi_eg);
        s_wifi_eg = nullptr;
        hideWifiStatusOverlay();
        return false;
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGE("NET", "esp_wifi_start: %s", esp_err_to_name(err));
        s_wifi_awaiting_ip = false;
        vEventGroupDelete(s_wifi_eg);
        s_wifi_eg = nullptr;
        hideWifiStatusOverlay();
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONN_BIT,
        pdTRUE, pdFALSE,
        pdMS_TO_TICKS(25000));

    s_wifi_awaiting_ip = false;
    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = nullptr;

    hideWifiStatusOverlay();

    if ((bits & WIFI_CONN_BIT) || wifiHasIp()) {
        wifi_connected = true;
        ESP_LOGI("NET", "WiFi conectado com IP");
        return true;
    }

    ESP_LOGW("NET", "WiFi timeout (25s) sem IP");
    return false;
}

bool NetworkMenu::ensureWifiReady(const string &ssid, const string &pass) {
    if (wifi_connected && wifiHasIp()) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
        ESP_LOGI("NET", "WiFi ativo " IPSTR, IP2STR(&ip.ip));
        return true;
    }
    if (wifi_connected && !wifiHasIp()) {
        ESP_LOGW("NET", "WiFi sem IP, reconectando");
        wifi_connected = false;
    }
    return wifiConnect(ssid, pass);
}


// wifiConfig() removida — credenciais lidas de /sd/wificonfig.rc automaticamente

// ═════════════════════════════════════════════════════════════════════════════
// Menu: Configurar Proxy (URL completa do wos.php)
// ═════════════════════════════════════════════════════════════════════════════

// Sanitiza URL — aceita caracteres validos de URL (mais permissivo que host)
static string sanitizeUrl(const string &s, size_t maxLen) {
    string out;
    for (unsigned char c : s) {
        // imprimiveis, sem espacos nem aspas
        if (c >= 33 && c <= 126 && c != '"' && c != '\'' && c != '\\') {
            out += (char)c;
            if (out.size() >= maxLen) break;
        }
    }
    return out;
}

void NetworkMenu::proxyConfig() {
    NetConfig cfg = loadConfig();

    OSD::saveBackbufferData(0, 0, OSD::scrW, OSD::scrH, true);

    string url = OSD::input(3, 3, NET_LBL_PROXY[Config::lang],
        cfg.proxy_url, 20, 96, zxColor(7,1), zxColor(1,0), false);

    OSD::restoreBackbufferData(true);

    if (url.empty()) return; // ESC
    cfg.proxy_url = sanitizeUrl(url, 96);
    if (cfg.proxy_url.empty()) return;

    saveConfig(cfg);
    OSD::osdCenteredMsg(NET_MSG_WIFI_SAVED[Config::lang], LEVEL_OK, 1000);
}



// ═════════════════════════════════════════════════════════════════════════════
// World of Spectrum Search — busca via API HTTP + download direto
// ═════════════════════════════════════════════════════════════════════════════

// Buffer global para HTTP (PSRAM) — evita alocação no stack
static uint8_t *s_http_buf = nullptr;
static size_t   s_http_len = 0;

// URL-encode: escapa espacos e caracteres especiais
static string urlEncode(const string &s) {
    static const char hex[] = "0123456789ABCDEF";
    string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0f];
        }
    }
    return out;
}

static esp_err_t wos_http_event(esp_http_client_event_t *evt) {
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_http_buf && (s_http_len + evt->data_len) < (512 * 1024)) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// Extrai valor de string de JSON simples: "key":"value"
static string jsonGetStr(const char *json, const char *key) {
    string k = string("\"") + key + "\":\"";
    const char *p = strstr(json, k.c_str());
    if (!p) return "";
    p += k.size();
    const char *e = strchr(p, '"');
    if (!e) return "";
    string val(p, e - p);
    // Unescape \/ -> /
    string out;
    for (size_t i = 0; i < val.size(); i++) {
        if (val[i] == '\\' && i+1 < val.size() && val[i+1] == '/') {
            out += '/'; i++;
        } else {
            out += val[i];
        }
    }
    return out;
}

// Extrai valor inteiro: "key":123
static int jsonGetInt(const char *json, const char *key) {
    string k = string("\"") + key + "\":";
    const char *p = strstr(json, k.c_str());
    if (!p) return 0;
    p += k.size();
    return atoi(p);
}

struct WosTitle {
    int    id;
    string title;
    string file_tap;   // path do TAP ou TZX (sem zip)
    string file_zip;   // path do zip
};

void NetworkMenu::wosSearch() {
    // Le URL base do proxy do NVS
    NetConfig ncfg = loadConfig();
    const string proxy = ncfg.proxy_url.empty()
        ? string("http://alternativebits.com/wos.php")
        : ncfg.proxy_url;

    // Garante WiFi conectado antes de buscar
    if (!ensureWifiReady(ncfg.wifi_ssid, ncfg.wifi_pass)) {
        ESP_LOGE("NET", "WiFi indisponivel");
        OSD::osdCenteredMsg(NET_MSG_FTP_NOWIFI[Config::lang], LEVEL_ERROR, 2000);
        return;
    }

    // Campo de busca — salva tela antes pra apagar o input ao sair
    OSD::saveBackbufferData(0, 0, OSD::scrW, OSD::scrH, true);
    string term = OSD::input(3, 3,
        Config::lang == 0 ? "Title: " : "Titulo: ", "",
        20, 32, zxColor(7,1), zxColor(1,0), false);
    OSD::restoreBackbufferData(true);
    if (term.empty()) return;

    // Aloca buffer HTTP na PSRAM
    s_http_buf = (uint8_t *)heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_http_buf) {
        OSD::osdCenteredMsg("Sem memoria PSRAM", LEVEL_ERROR, 1500);
        return;
    }
    s_http_len = 0;

    // Busca na API (via proxy configurado)
    string term_enc = urlEncode(term);
    char url[320];
    snprintf(url, sizeof(url),
        "%s?action=search&title=%s",
        proxy.c_str(), term_enc.c_str());

    showWifiStatusOverlay("Buscando WoS...", LEVEL_INFO);

    esp_http_client_config_t cfg = {};
    cfg.url            = url;
    cfg.event_handler  = wos_http_event;
    cfg.timeout_ms     = 10000;
    cfg.buffer_size    = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE("WOS", "esp_http_client_init falhou para url=%s", url);
        hideWifiStatusOverlay();
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("URL invalida", LEVEL_ERROR, 1500);
        return;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI("WOS", "Busca HTTP err=%d status=%d len=%u url=%s", (int)err, status, (unsigned)s_http_len, url);
    esp_http_client_cleanup(client);
    hideWifiStatusOverlay();

    if (err != ESP_OK || s_http_len == 0) {
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("Erro HTTP busca", LEVEL_ERROR, 1500);
        return;
    }
    s_http_buf[s_http_len] = 0;
    ESP_LOGI("WOS", "Busca OK: %u bytes", (unsigned)s_http_len);

    // Parseia títulos — extrai id e title
    // Procura "id": seguido de digito (evita "turn_type_id":null etc)
    vector<WosTitle> titles;
    const char *p = (const char *)s_http_buf;
    while (titles.size() < 10) {
        const char *tid = strstr(p, "\"id\":");
        if (!tid) break;
        const char *idval = tid + 5;
        // pula espacos
        while (*idval == ' ') idval++;
        // so aceita se for digito (id de jogo e numero puro: "id":10595)
        if (*idval < '0' || *idval > '9') {
            p = tid + 5;
            continue;
        }
        int id = atoi(idval);
        const char *ttl = strstr(tid, "\"title\":\"");
        if (!ttl) break;
        ttl += 9;
        const char *te = strchr(ttl, '"');
        if (!te) break;
        WosTitle wt;
        wt.id = id;
        wt.title = string(ttl, te - ttl);
        titles.push_back(wt);
        p = te + 1;
    }

    if (titles.empty()) {
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("Sem resultados", LEVEL_WARN, 1500);
        return;
    }

    // Menu de títulos
    string smenu = "World of Spectrum\n";
    for (auto &t : titles)
        smenu += t.title.substr(0, 36) + "\n";

    OSD::menu_level    = 2;
    OSD::menu_curopt   = 1;
    OSD::menu_saverect = true;
    uint8_t sel = OSD::menuRun(smenu);
    if (sel == 0) {
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        return;
    }

    WosTitle &chosen = titles[sel - 1];

    // Busca versões e arquivos
    snprintf(url, sizeof(url),
        "%s?action=versions&sid=%d",
        proxy.c_str(), chosen.id);

    s_http_len = 0;
    cfg.url = url;
    showWifiStatusOverlay("Buscando versoes...", LEVEL_INFO);
    client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE("WOS", "init falhou para url=%s", url);
        hideWifiStatusOverlay();
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("URL invalida", LEVEL_ERROR, 1500);
        return;
    }
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    ESP_LOGI("WOS", "Versoes HTTP err=%d status=%d len=%u url=%s", (int)err, status, (unsigned)s_http_len, url);
    esp_http_client_cleanup(client);
    hideWifiStatusOverlay();

    if (err != ESP_OK || s_http_len == 0) {
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("Erro HTTP versoes", LEVEL_ERROR, 1500);
        return;
    }
    s_http_buf[s_http_len] = 0;

    // Parseia TODAS as versoes do JSON (qualquer extensao filtrada pelo proxy)
    struct WosFile {
        string path;   // path completo (ex: /pub/sinclair/games/r/Exolon.tzx.zip)
        string name;   // nome amigavel sem .zip (ex: Exolon.tzx)
    };
    vector<WosFile> files;
    const char *fp = (const char *)s_http_buf;
    while (files.size() < 16) {
        const char *fid = strstr(fp, "\"file_id\":\"");
        if (!fid) break;
        fid += 11;
        const char *fe = strchr(fid, '"');
        if (!fe) break;
        string fpath(fid, fe - fid);
        fp = fe + 1;

        // Unescape \/ -> /
        string clean;
        for (size_t i = 0; i < fpath.size(); i++) {
            if (fpath[i] == '\\' && i+1 < fpath.size() && fpath[i+1] == '/') {
                clean += '/'; i++;
            } else clean += fpath[i];
        }

        WosFile wf;
        wf.path = clean;
        // Nome amigavel: ultimo segmento, sem .zip
        wf.name = clean.substr(clean.rfind('/') + 1);
        if (wf.name.size() > 4 && wf.name.substr(wf.name.size() - 4) == ".zip")
            wf.name = wf.name.substr(0, wf.name.size() - 4);
        files.push_back(wf);
    }

    heap_caps_free(s_http_buf); s_http_buf = nullptr;

    if (files.empty()) {
        OSD::osdCenteredMsg("Sem arquivos", LEVEL_WARN, 1500);
        return;
    }

    // Menu de versoes — usuario escolhe ou ESC para voltar.
    //
    // Nao usamos menuRun aqui porque o auto-ajuste de borda em OSDMenu.cpp
    // (if x + cols*FONT_W > rmax*FONT_W → x = (rmax - cols)*FONT_W) cola o
    // menu na direita independente do menu_level, dando x=120 em tela 320
    // tanto pra menu_level=2 quanto 3. simpleMenuRun aceita x,y explicitos
    // (mesmo esquema usado para os popups de banks/joy) e nos deixa colocar
    // o menu de versoes colado a esquerda com largura cheia.
    string vmenu = chosen.title.substr(0, 46) + "\n";
    for (auto &f : files)
        vmenu += f.name.substr(0, 46) + "\n";

    OSD::menu_curopt = 1;
    uint8_t vsel = OSD::simpleMenuRun(vmenu, 12, 24, 15, 48);
    if (vsel == 0) return; // ESC volta

    const string file_zip  = files[vsel - 1].path;
    const string file_name = files[vsel - 1].name;

    // Limpa a tela antes do download. O menu principal (level 0) foi aberto
    // com menu_saverect=false em OSDMain, entao nao existe backbuffer dele
    // para restaurar — restoreBackbufferData sozinho nao resolve esse caso.
    // Pintamos a tela toda com a cor PAPER do Spectrum em idle (branco),
    // que fica visualmente consistente com o emulador recem-ligado em vez
    // da bagunça de menus empilhados.
    VIDEO::vga.fillRect(0, 0, OSD::scrW, OSD::scrH, zxColor(7, 0));

    // Download via proxy
    string dl_url = proxy + "?action=download&file=" + file_zip;
    ESP_LOGI("WOS", "Download: %s", dl_url.c_str());


    s_http_buf = (uint8_t *)heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_http_buf) {
        OSD::osdCenteredMsg("Sem memoria PSRAM", LEVEL_ERROR, 1500);
        return;
    }
    s_http_len = 0;

    showWifiStatusOverlay("Baixando...", LEVEL_INFO);
    cfg.url = dl_url.c_str();
    client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE("WOS", "init falhou para url=%s", dl_url.c_str());
        hideWifiStatusOverlay();
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("URL invalida", LEVEL_ERROR, 1500);
        return;
    }
    err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    hideWifiStatusOverlay();

    if (err != ESP_OK || s_http_len == 0) {
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("Erro download", LEVEL_ERROR, 1500);
        return;
    }

    ESP_LOGI("WOS", "Arquivo recebido: %u bytes", (unsigned)s_http_len);

    // O proxy ja descomprimiu o ZIP e devolveu o .tap/.tzx puro.
    // Diagnostico: se vier "NOT FOUND" ou texto curto, e erro do proxy.
    if (s_http_len < 16) {
        s_http_buf[s_http_len] = 0;
        ESP_LOGE("WOS", "Resposta invalida: %s", (char *)s_http_buf);
        heap_caps_free(s_http_buf); s_http_buf = nullptr;
        OSD::osdCenteredMsg("Download invalido", LEVEL_ERROR, 2000);
        return;
    }

    // Grava no SD direto (sem descompressao — o proxy ja entregou o .tap)
    const string dest = netFtpDestPath(file_name);
    bool first = true;
    bool ok = netFlushToSd(dest, s_http_buf, s_http_len, first);
    heap_caps_free(s_http_buf); s_http_buf = nullptr;

    if (!ok) {
        OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_FAIL[Config::lang], LEVEL_ERROR, 2000);
        return;
    }

    OSD::osdCenteredMsg(NET_MSG_DOWNLOAD_OK[Config::lang], LEVEL_OK, 1500);

    // Sinaliza para OSDMain fechar o OSD inteiro em vez de voltar ao menu
    // de rede. O check pos-loop em OSDMain detecta esse flag e executa
    // return do do_OSD, resultando em todos os menus fechados e emulador
    // assumindo a tela — mesmo comportamento da troca de idioma.
    s_exit_osd_after_load = true;
}