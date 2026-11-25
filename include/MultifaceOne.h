/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Multiface One (Romantic Robot) — PCB rev 2.x (page-out 0x1F)

*/

#ifndef MultifaceOne_h
#define MultifaceOne_h

#include <inttypes.h>

#define MF1_ROM_SIZE 0x2000
#define MF1_RAM_SIZE 0x2000
#define MF1_PAGE_SIZE 0x4000
#define MF1_ROM_FILE "/sd/ESPectrum WIFI/mf1.rom"
#define MF1_ROM_FILE_FALLBACK "/sd/mf1.rom"

// Port decode: ---- ---- x001 --1-  (mask 0x0072, value 0x0012)
#define MF1_PORT_MASK  0x0072
#define MF1_PORT_VALUE 0x0012

class MultifaceOne {
public:

    static bool Init();
    static void Reset(bool hard);
    static bool LoadROM();
    static bool isReady();

    // Paged into 0x0000 (ROM) + 0x2000 (RAM)
    static bool paged;

    // Red button latched NMI (FF_NMI_PENDING / IC8b)
    static bool nmiPending;

    // Physical enable switch (J2). false = stealth / OFF
    static bool switchOn;
    static bool romLoaded;

    static uint8_t* rom;
    static uint8_t* ram;
    static uint8_t* page; // contiguous 16K: ROM|RAM for ramCurrent[0]

    static void pageIn();
    static void pageOut();

    // Spectrum/TR-DOS ROM bank change while MF may own 0x0000
    static void mapSpectrumRom(uint8_t* romPage);

    static void pressButton(); // red button → NMI
    static void checkFetch(uint16_t pc); // M1 @ 0x0066/0x0067

    static uint8_t portIn(uint16_t address);
    static void portOut(uint16_t address, uint8_t data);

    static inline bool isPort(uint16_t address) {
        return (address & MF1_PORT_MASK) == MF1_PORT_VALUE;
    }

    static inline bool writeAllowed(uint16_t address) {
        return paged && address >= 0x2000 && address < 0x4000;
    }

private:

    static uint8_t* savedRomPage; // Spectrum/TR-DOS page under MF

};

#endif
