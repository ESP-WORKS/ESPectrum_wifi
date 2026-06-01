/*

ESPectrum Fork — NetworkMenu.h
Configurações salvas em NVS, namespace "storage" (igual ao ESPConfig).

*/

#ifndef ESPECTRUM_NETWORKMENU_H
#define ESPECTRUM_NETWORKMENU_H

#include <string>
#include <vector>
using namespace std;

class NetworkMenu {
public:
    // Chamados pelo dispatcher em OSDMain.cpp (opt == 7 do menu principal)
    static void wifiConfig();
    static void proxyConfig();

    static void wosSearch();

    static bool wifi_connected; // exposto para o event handler

    // Pausa WiFi durante I/O SD pesado (contador aninhado)
    static void wifiPauseForSd();
    static void wifiResumeAfterSd();

    // Liberta barramento SD antes de WiFi/FTP (jogo ja em RAM)
    static void prepareForNetwork();
    static void restoreAfterNetwork();
    static bool isNetworkSession();

    // true apos carregar jogo do FTP — OSDMain deve sair do menu (return)
    static bool shouldExitOsdAfterLoad();
    static void clearExitOsdAfterLoad();

private:
    // ── Config NVS ──────────────────────────────────────────────────────────
    struct NetConfig {
        string wifi_ssid = "";
        string wifi_pass = "";
        string proxy_url = "http://alternativebits.com/wos.php";
    };

    static NetConfig loadConfig();
    static void      saveConfig(const NetConfig &cfg);

    // ── WiFi ─────────────────────────────────────────────────────────────────
    static bool wifiInitOnce();
    static bool wifiConnect(const string &ssid, const string &pass);
    static bool ensureWifiReady(const string &ssid, const string &pass);

};

#endif // ESPECTRUM_NETWORKMENU_H