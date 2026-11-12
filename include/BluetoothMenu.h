/*

ESPectrum Fork — BluetoothMenu.h
Gamepad Bluetooth via Bluepad32 (https://github.com/ricardoquesada/bluepad32)

*/

#ifndef ESPECTRUM_BLUETOOTHMENU_H
#define ESPECTRUM_BLUETOOTHMENU_H

#include <stdint.h>
#include <string>

class BluetoothMenu {
public:
    static void setup();
    static void doMenu();
    static void applyToJoystick();
    static bool isConnected();
    static std::string connectedName();

private:
    static void pairDevices();
    static void unpairDevices();
    static void connectPaired();
    static void showStatus();
};

#endif
