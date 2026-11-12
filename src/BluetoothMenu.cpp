/*

ESPectrum Fork — BluetoothMenu.cpp
Gamepad Bluetooth via Bluepad32.

*/

#include "BluetoothMenu.h"
#include "OSDMain.h"
#include "ESPConfig.h"
#include "ESPectrum.h"
#include "Ports.h"
#include "ZXKeyb.h"
#include "messages.h"

#include "sdkconfig.h"

#ifndef CONFIG_BLUEPAD32_PLATFORM_CUSTOM
#error "sdkconfig must set CONFIG_BLUEPAD32_PLATFORM_CUSTOM"
#endif

extern "C" {
#include "btstack_port_esp32.h"
#include "btstack_run_loop.h"
#include "uni.h"
#include "uni_common.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

using namespace std;

static const char *TAG = "BTMENU";

#define BT_MAX_DISCOVERED 8

struct BtDisc {
    bd_addr_t addr;
    char name[28];
    bool used;
    bool connected;
};

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static BtDisc s_disc[BT_MAX_DISCOVERED];
static volatile bool s_bt_ready = false;
static volatile bool s_accept_new = true;
static volatile bool s_pairing = false;
static volatile bool s_connected = false;
static volatile uint8_t s_kempston = 0;
static volatile uint8_t s_prev_kempston = 0;
static volatile uint8_t s_fuller = 0xff;
static volatile uint8_t s_prev_fuller = 0xff;
static volatile uint8_t s_dpad = 0;
static volatile uint16_t s_buttons = 0;
static volatile uint8_t s_misc = 0;
static char s_connected_name[28] = {0};
static int s_connected_idx = -1;
static int64_t s_boot_scan_until = 0;
static bool s_sys_held = false;
static uint8_t s_osd_dpad = 0;
static uint16_t s_osd_buttons = 0;
static bool s_started = false;

static void addrToStr(const bd_addr_t addr, char *out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static int findDisc(const bd_addr_t addr) {
    for (int i = 0; i < BT_MAX_DISCOVERED; i++) {
        if (s_disc[i].used && memcmp(s_disc[i].addr, addr, 6) == 0)
            return i;
    }
    return -1;
}

static int addDisc(const bd_addr_t addr, const char *name) {
    int idx = findDisc(addr);
    if (idx >= 0) {
        if (name && name[0] && s_disc[idx].name[0] == 0)
            strncpy(s_disc[idx].name, name, sizeof(s_disc[idx].name) - 1);
        return idx;
    }
    for (int i = 0; i < BT_MAX_DISCOVERED; i++) {
        if (!s_disc[i].used) {
            memset(&s_disc[i], 0, sizeof(s_disc[i]));
            memcpy(s_disc[i].addr, addr, 6);
            if (name && name[0])
                strncpy(s_disc[i].name, name, sizeof(s_disc[i].name) - 1);
            s_disc[i].used = true;
            return i;
        }
    }
    return -1;
}

static void injectJoyKey(fabgl::VirtualKey vk, bool down) {
    auto kbd = ESPectrum::PS2Controller.keyboard();
    if (kbd)
        kbd->injectVirtualKey(vk, down, false);
}

static void updateOsdKeys(uint8_t dpad, uint16_t buttons) {
    if (!OSD::inMenu)
        return;

    auto edge = [](uint8_t now, uint8_t prev, uint8_t bit, fabgl::VirtualKey vk) {
        bool n = now & bit;
        bool p = prev & bit;
        if (n != p)
            injectJoyKey(vk, n);
    };

    edge(dpad, s_osd_dpad, DPAD_UP, fabgl::VK_JOY1UP);
    edge(dpad, s_osd_dpad, DPAD_DOWN, fabgl::VK_JOY1DOWN);
    edge(dpad, s_osd_dpad, DPAD_LEFT, fabgl::VK_JOY1LEFT);
    edge(dpad, s_osd_dpad, DPAD_RIGHT, fabgl::VK_JOY1RIGHT);

    bool fire = (buttons & (BUTTON_A | BUTTON_X)) != 0;
    bool back = (buttons & (BUTTON_B | BUTTON_Y)) != 0;
    bool prev_fire = (s_osd_buttons & (BUTTON_A | BUTTON_X)) != 0;
    bool prev_back = (s_osd_buttons & (BUTTON_B | BUTTON_Y)) != 0;
    if (fire != prev_fire)
        injectJoyKey(fabgl::VK_JOY1B, fire);
    if (back != prev_back)
        injectJoyKey(fabgl::VK_JOY1A, back);

    s_osd_dpad = dpad;
    s_osd_buttons = buttons;
}

static void platform_init(int argc, const char **argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    ESP_LOGI(TAG, "Bluepad32 platform init");
}

static void platform_on_init_complete(void) {
    ESP_LOGI(TAG, "Bluepad32 ready — auto-connect paired gamepads");
    uni_bt_enable_new_connections_unsafe(true);
    s_bt_ready = true;
    s_accept_new = true;
    s_boot_scan_until = esp_timer_get_time() + 25000000LL;
}

static uni_error_t platform_on_device_discovered(bd_addr_t addr, const char *name, uint16_t cod, uint8_t rssi) {
    ARG_UNUSED(rssi);
    if (((cod & UNI_BT_COD_MINOR_MASK) & UNI_BT_COD_MINOR_KEYBOARD) == UNI_BT_COD_MINOR_KEYBOARD)
        return UNI_ERROR_IGNORE_DEVICE;

    portENTER_CRITICAL(&s_lock);
    addDisc(addr, name);
    portEXIT_CRITICAL(&s_lock);

    if (s_connected && !s_pairing)
        return UNI_ERROR_IGNORE_DEVICE;
    if (!s_accept_new && !s_pairing)
        return UNI_ERROR_IGNORE_DEVICE;

    return UNI_ERROR_SUCCESS;
}

static void platform_on_device_connected(uni_hid_device_t *d) {
    ESP_LOGI(TAG, "device connected %p", d);
}

static void platform_on_device_disconnected(uni_hid_device_t *d) {
    ESP_LOGI(TAG, "device disconnected %p", d);
    portENTER_CRITICAL(&s_lock);
    if (d) {
        int idx = findDisc(d->conn.btaddr);
        if (idx >= 0)
            s_disc[idx].connected = false;
    }
    s_connected = false;
    s_connected_idx = -1;
    s_kempston = 0;
    s_fuller = 0xff;
    s_connected_name[0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static uni_error_t platform_on_device_ready(uni_hid_device_t *d) {
    if (!d)
        return UNI_ERROR_INVALID_DEVICE;

    const char *name = d->name[0] ? d->name : "Gamepad";
    ESP_LOGI(TAG, "device ready: %s", name);

    portENTER_CRITICAL(&s_lock);
    int idx = addDisc(d->conn.btaddr, name);
    if (idx >= 0)
        s_disc[idx].connected = true;
    strncpy(s_connected_name, name, sizeof(s_connected_name) - 1);
    s_connected_name[sizeof(s_connected_name) - 1] = 0;
    s_connected = true;
    s_connected_idx = uni_hid_device_get_idx_for_instance(d);
    s_accept_new = false;
    portEXIT_CRITICAL(&s_lock);

    uni_bt_enable_new_connections_safe(false);
    return UNI_ERROR_SUCCESS;
}

static void platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl) {
    ARG_UNUSED(d);
    if (!ctl || ctl->klass != UNI_CONTROLLER_CLASS_GAMEPAD)
        return;

    uni_gamepad_t *gp = &ctl->gamepad;
    uni_joystick_t joy;
    memset(&joy, 0, sizeof(joy));
    uni_joy_to_single_joy_from_gamepad(gp, &joy, 1);

    uint8_t k = 0;
    if (joy.right) k |= 1 << 0;
    if (joy.left)  k |= 1 << 1;
    if (joy.down)  k |= 1 << 2;
    if (joy.up)    k |= 1 << 3;
    if (joy.fire)  k |= 1 << 4;
    if (joy.button2 || (gp->buttons & (BUTTON_Y | BUTTON_SHOULDER_R)))
        k |= 1 << 5;

    uint8_t f = 0xff;
    if (joy.right) f &= ~(1 << 3);
    if (joy.left)  f &= ~(1 << 2);
    if (joy.down)  f &= ~(1 << 1);
    if (joy.up)    f &= ~(1 << 0);
    if (joy.fire)  f &= ~(1 << 7);

    s_kempston = k;
    s_fuller = f;
    s_dpad = gp->dpad;
    s_buttons = gp->buttons;
    s_misc = gp->misc_buttons;

    if (OSD::inMenu)
        updateOsdKeys(gp->dpad, gp->buttons);

    bool sys = (gp->misc_buttons & MISC_BUTTON_SYSTEM) != 0;
    if (sys && !s_sys_held && !OSD::inMenu)
        injectJoyKey(fabgl::VK_F1, true);
    else if (!sys && s_sys_held && !OSD::inMenu)
        injectJoyKey(fabgl::VK_F1, false);
    s_sys_held = sys;
}

static const uni_property_t *platform_get_property(uni_property_idx_t idx) {
    ARG_UNUSED(idx);
    return NULL;
}

static void platform_on_oob_event(uni_platform_oob_event_t event, void *data) {
    ARG_UNUSED(event);
    ARG_UNUSED(data);
}

static struct uni_platform *get_espectrum_platform(void) {
    static struct uni_platform plat = {};
    plat.name = "ESPectrum";
    plat.init = platform_init;
    plat.on_init_complete = platform_on_init_complete;
    plat.on_device_discovered = platform_on_device_discovered;
    plat.on_device_connected = platform_on_device_connected;
    plat.on_device_disconnected = platform_on_device_disconnected;
    plat.on_device_ready = platform_on_device_ready;
    plat.on_controller_data = platform_on_controller_data;
    plat.get_property = platform_get_property;
    plat.on_oob_event = platform_on_oob_event;
    return &plat;
}

static void btstack_task(void *arg) {
    ARG_UNUSED(arg);
    btstack_init();
    uni_platform_set_custom(get_espectrum_platform());
    uni_init(0, NULL);
    btstack_run_loop_execute();
    vTaskDelete(NULL);
}

void BluetoothMenu::setup() {
    if (s_started)
        return;
    s_started = true;
    memset(s_disc, 0, sizeof(s_disc));
    xTaskCreatePinnedToCore(&btstack_task, "btstack", 8192, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Bluepad32 task started");
}

bool BluetoothMenu::isConnected() {
    return s_connected;
}

string BluetoothMenu::connectedName() {
    portENTER_CRITICAL(&s_lock);
    string n = s_connected_name[0] ? s_connected_name : "";
    portEXIT_CRITICAL(&s_lock);
    return n;
}

void BluetoothMenu::applyToJoystick() {
    if (!s_bt_ready)
        return;

    if (!s_pairing && s_boot_scan_until && esp_timer_get_time() > s_boot_scan_until) {
        s_boot_scan_until = 0;
        s_accept_new = s_connected ? false : true;
        if (s_connected)
            uni_bt_enable_new_connections_safe(false);
    }

    if (!s_connected)
        return;

    uint8_t k = s_kempston;
    uint8_t f = s_fuller;

    Ports::port[0x1f] = (Ports::port[0x1f] & ~s_prev_kempston) | k;
    s_prev_kempston = k;

    if (ESPectrum::joystick1 == JOY_FULLER || ESPectrum::joystick2 == JOY_FULLER) {
        uint8_t keep = Ports::port[0x7f] | (~s_prev_fuller);
        Ports::port[0x7f] = keep & f;
        s_prev_fuller = f;
    }
}

static string discLabel(const BtDisc &d) {
    char mac[20];
    addrToStr(d.addr, mac, sizeof(mac));
    string n = d.name[0] ? d.name : mac;
    if (n.size() > 22)
        n = n.substr(0, 22);
    if (d.connected)
        n += " *";
    return n;
}

static bool waitForGamepad(int timeout_ms) {
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000LL;
    while (esp_timer_get_time() < until && !s_connected) {
        if (ZXKeyb::Exists)
            ZXKeyb::ZXKbdRead();
        ESPectrum::readKbdJoy();
        fabgl::VirtualKeyItem key;
        if (ESPectrum::PS2Controller.keyboard()->virtualKeyAvailable()) {
            if (ESPectrum::readKbd(&key) && key.down &&
                (key.vk == fabgl::VK_ESCAPE || key.vk == fabgl::VK_F1 ||
                 key.vk == fabgl::VK_JOY1A || key.vk == fabgl::VK_JOY2A))
                return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return s_connected;
}

void BluetoothMenu::pairDevices() {
    if (!s_bt_ready) {
        OSD::osdCenteredMsg(BT_MSG_FAIL[Config::lang], LEVEL_ERROR, 1500);
        return;
    }

    s_pairing = true;
    s_accept_new = true;
    uni_bt_enable_new_connections_safe(true);

    OSD::osdCenteredMsg(BT_MSG_PAIR_HINT[Config::lang], LEVEL_INFO, 1200);

    OSD::menu_level = 2;
    OSD::menu_curopt = 1;
    OSD::menu_saverect = true;

    while (1) {
        string mnu = string(MENU_BT_PAIR_TITLE[Config::lang]) + "\n";
        mnu += string(BT_MSG_REFRESH[Config::lang]) + "\n";

        BtDisc snap[BT_MAX_DISCOVERED];
        portENTER_CRITICAL(&s_lock);
        memcpy(snap, s_disc, sizeof(snap));
        bool connected = s_connected;
        char cname[28];
        memcpy(cname, s_connected_name, sizeof(cname));
        portEXIT_CRITICAL(&s_lock);

        int nshown = 0;
        for (int i = 0; i < BT_MAX_DISCOVERED; i++) {
            if (snap[i].used) {
                mnu += discLabel(snap[i]) + "\n";
                nshown++;
            }
        }
        if (nshown == 0)
            mnu += string(BT_MSG_SEARCHING[Config::lang]) + "\n";

        uint8_t opt = OSD::menuRun(mnu);
        if (opt == 0) {
            s_pairing = false;
            if (s_connected)
                uni_bt_enable_new_connections_safe(false);
            return;
        }

        if (s_connected || connected) {
            string msg = string(BT_MSG_CONNECTED[Config::lang]) + " " + (s_connected ? connectedName() : cname);
            OSD::osdCenteredMsg(msg, LEVEL_OK, 1500);
            s_pairing = false;
            s_accept_new = false;
            uni_bt_enable_new_connections_safe(false);
            return;
        }

        // opt==1 is Refresh (and Searching placeholder). Keep listing.
        if (opt == 1 || nshown == 0) {
            OSD::menu_curopt = 1;
            OSD::menu_saverect = false;
            continue;
        }

        // User picked a discovered pad. Auto-connect is already in progress;
        // wait instead of redrawing the OSD (PSRAM) over the BT controller.
        OSD::osdCenteredMsg(BT_MSG_CONNECTING[Config::lang], LEVEL_INFO, 400);
        if (waitForGamepad(20000)) {
            OSD::osdCenteredMsg(string(BT_MSG_CONNECTED[Config::lang]) + " " + connectedName(), LEVEL_OK, 1500);
            s_pairing = false;
            s_accept_new = false;
            uni_bt_enable_new_connections_safe(false);
            return;
        }

        OSD::menu_curopt = 1;
        OSD::menu_saverect = false;
    }
}

void BluetoothMenu::unpairDevices() {
    if (!s_bt_ready) {
        OSD::osdCenteredMsg(BT_MSG_FAIL[Config::lang], LEVEL_ERROR, 1500);
        return;
    }

    string title = BT_MSG_UNPAIR[Config::lang];
    string ask = s_connected
        ? (string(BT_MSG_UNPAIR_ONE[Config::lang]) + "\n" + connectedName())
        : string(BT_MSG_UNPAIR_ALL[Config::lang]);

    if (OSD::msgDialog(title, ask, MSGDIALOG_YESNO) != DLG_YES)
        return;

    if (s_connected_idx >= 0)
        uni_bt_disconnect_device_safe(s_connected_idx);
    uni_bt_del_keys_safe();
    uni_bt_allowlist_remove_all();

    portENTER_CRITICAL(&s_lock);
    memset(s_disc, 0, sizeof(s_disc));
    s_connected = false;
    s_connected_idx = -1;
    s_connected_name[0] = 0;
    s_kempston = 0;
    portEXIT_CRITICAL(&s_lock);

    OSD::osdCenteredMsg(BT_MSG_UNPAIRED[Config::lang], LEVEL_OK, 1200);
}

void BluetoothMenu::connectPaired() {
    if (!s_bt_ready) {
        OSD::osdCenteredMsg(BT_MSG_FAIL[Config::lang], LEVEL_ERROR, 1500);
        return;
    }
    if (s_connected) {
        OSD::osdCenteredMsg(string(BT_MSG_CONNECTED[Config::lang]) + " " + connectedName(), LEVEL_OK, 1200);
        return;
    }

    s_accept_new = true;
    uni_bt_enable_new_connections_safe(true);

    OSD::osdCenteredMsg(BT_MSG_WAITING[Config::lang], LEVEL_INFO, 800);

    if (waitForGamepad(20000)) {
        OSD::osdCenteredMsg(string(BT_MSG_CONNECTED[Config::lang]) + " " + connectedName(), LEVEL_OK, 1500);
        s_accept_new = false;
        uni_bt_enable_new_connections_safe(false);
    } else {
        OSD::osdCenteredMsg(BT_MSG_NOT_FOUND[Config::lang], LEVEL_WARN, 1500);
        uni_bt_enable_new_connections_safe(false);
    }
}

void BluetoothMenu::showStatus() {
    string msg;
    if (!s_bt_ready)
        msg = BT_MSG_FAIL[Config::lang];
    else if (s_connected)
        msg = string(BT_MSG_CONNECTED[Config::lang]) + " " + connectedName();
    else
        msg = BT_MSG_DISCONNECTED[Config::lang];
    OSD::osdCenteredMsg(msg, s_connected ? LEVEL_OK : LEVEL_INFO, 1600);
}

void BluetoothMenu::doMenu() {
    if (!s_bt_ready) {
        OSD::osdCenteredMsg(BT_MSG_FAIL[Config::lang], LEVEL_ERROR, 1800);
    }
    OSD::menu_saverect = true;
    OSD::menu_curopt = 1;
    while (1) {
        OSD::menu_level = 1;
        uint8_t opt = OSD::menuRun(MENU_BT[Config::lang]);
        if (opt == 1) {
            pairDevices();
            OSD::menu_curopt = 1;
            OSD::menu_saverect = false;
        } else if (opt == 2) {
            unpairDevices();
            OSD::menu_curopt = 2;
            OSD::menu_saverect = false;
        } else if (opt == 3) {
            connectPaired();
            OSD::menu_curopt = 3;
            OSD::menu_saverect = false;
        } else if (opt == 4) {
            showStatus();
            OSD::menu_curopt = 4;
            OSD::menu_saverect = false;
        } else {
            OSD::menu_curopt = 4;
            break;
        }
    }
}
