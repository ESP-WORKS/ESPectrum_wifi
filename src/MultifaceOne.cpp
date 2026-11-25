/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Multiface One (Romantic Robot) — PCB rev 2.x

Behaviour follows Kio circuit notes + Fuse multiface.c:
  IN  0x9F  page in  (if switch ON) + Kempston bits
  IN  0x1F  page out + Kempston bits
  OUT 0x1F  clear NMI pending
  Red button → NMI; M1 @ $0066/$0067 pages ROM/RAM in

ROM is not shipped (copyright). Place 8 KB file at /sd/ESPectrum WIFI/mf1.rom

*/

#include "MultifaceOne.h"
#include "ESPConfig.h"
#include "MemESP.h"
#include "ESPectrum.h"
#include "Ports.h"
#include "Z80_JLS/z80.h"
#include "Z80_JLS/z80operations.h"

#include <cstring>
#include <cstdio>
#include "esp_heap_caps.h"

bool MultifaceOne::paged = false;
bool MultifaceOne::nmiPending = false;
bool MultifaceOne::switchOn = true;
bool MultifaceOne::romLoaded = false;

uint8_t* MultifaceOne::rom = nullptr;
uint8_t* MultifaceOne::ram = nullptr;
uint8_t* MultifaceOne::page = nullptr;
uint8_t* MultifaceOne::savedRomPage = nullptr;

bool MultifaceOne::Init() {

    if (page) return true;

    page = (uint8_t *) heap_caps_calloc(MF1_PAGE_SIZE, 1, MALLOC_CAP_8BIT);
    if (!page && Config::psram_size)
        page = (uint8_t *) heap_caps_calloc(MF1_PAGE_SIZE, 1, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);

    if (!page) {
        printf("MultifaceOne: unable to allocate 16K page\n");
        return false;
    }

    rom = page;
    ram = page + MF1_ROM_SIZE;
    return true;

}

bool MultifaceOne::isReady() {
    return Config::MultifaceOne && romLoaded && page != nullptr && !Z80Ops::is2a3;
}

static void mf1_dump_state(const char* where) {
    printf("MF1:%s cfg=%d rom=%d page=%p ready=%d paged=%d nmiPend=%d sw=%d stealth=%d arch=%s is2a3=%d PC=%04X\n",
           where,
           (int)Config::MultifaceOne,
           (int)MultifaceOne::romLoaded,
           (void*)MultifaceOne::page,
           (int)MultifaceOne::isReady(),
           (int)MultifaceOne::paged,
           (int)MultifaceOne::nmiPending,
           (int)MultifaceOne::switchOn,
           (int)Config::MultifaceOneStealth,
           Config::arch.c_str(),
           (int)Z80Ops::is2a3,
           (unsigned)Z80::getRegPC());
}

bool MultifaceOne::LoadROM() {

    if (!page && !Init()) {
        printf("MF1:LoadROM FAIL Init/page alloc\n");
        return false;
    }

    const char* paths[] = { MF1_ROM_FILE, MF1_ROM_FILE_FALLBACK };
    FILE *f = nullptr;
    const char* used = nullptr;

    for (const char* path : paths) {
        printf("MF1:LoadROM looking for %s\n", path);
        f = fopen(path, "rb");
        if (f) {
            used = path;
            printf("MF1:LoadROM FOUND %s\n", path);
            break;
        }
        printf("MF1:LoadROM NOT FOUND %s\n", path);
    }

    if (!f) {
        romLoaded = false;
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    printf("MF1:LoadROM size=%ld (need >= %d) from %s\n", sz, MF1_ROM_SIZE, used);

    if (sz < MF1_ROM_SIZE) {
        printf("MF1:LoadROM FAIL too small\n");
        fclose(f);
        romLoaded = false;
        return false;
    }

    size_t n = fread(rom, 1, MF1_ROM_SIZE, f);
    fclose(f);

    romLoaded = (n == MF1_ROM_SIZE);
    if (romLoaded)
        printf("MF1:LoadROM OK read %u bytes from %s, @0066=%02X\n",
               (unsigned)n, used, rom[0x66]);
    else
        printf("MF1:LoadROM FAIL short read got %u\n", (unsigned)n);

    return romLoaded;

}

void MultifaceOne::Reset(bool hard) {

    printf("MF1:Reset hard=%d cfg=%d romLoaded=%d\n",
           (int)hard, (int)Config::MultifaceOne, (int)romLoaded);

    pageOut();
    nmiPending = false;
    switchOn = !Config::MultifaceOneStealth;

    if (hard && ram)
        memset(ram, 0, MF1_RAM_SIZE);

    if (Config::MultifaceOne && !romLoaded) {
        printf("MF1:Reset will try LoadROM\n");
        LoadROM();
    } else if (Config::MultifaceOne && romLoaded) {
        printf("MF1:Reset ROM already loaded, skip fopen\n");
    } else {
        printf("MF1:Reset MF disabled, skip ROM\n");
    }

}

void MultifaceOne::pageIn() {

    if (!isReady() || paged) return;

    savedRomPage = MemESP::ramCurrent[0];
    MemESP::ramCurrent[0] = page;
    paged = true;
    // No printf here: may run from IRAM fetch path

}

void MultifaceOne::pageOut() {

    if (!paged) return;

    if (savedRomPage)
        MemESP::ramCurrent[0] = savedRomPage;
    else if (ESPectrum::trdos)
        MemESP::ramCurrent[0] = MemESP::rom[4];
    else
        MemESP::ramCurrent[0] = MemESP::rom[MemESP::romInUse];

    savedRomPage = nullptr;
    paged = false;

}

void MultifaceOne::mapSpectrumRom(uint8_t* romPage) {

    if (paged)
        savedRomPage = romPage;
    else
        MemESP::ramCurrent[0] = romPage;

}

void MultifaceOne::pressButton() {

    // Config on but ROM not in RAM (reboot after path change, etc.)
    if (Config::MultifaceOne && !romLoaded) {
        printf("MF1:pressButton romLoaded=0 → retry LoadROM\n");
        LoadROM();
    }

    mf1_dump_state("pressButton");

    if (!isReady()) {
        if (Config::MultifaceOne) {
            // MF enabled but no ROM: plain Spectrum NMI @ $0066 looks like a reset
            printf("MF1:pressButton BLOCKED — put mf1.rom in %s (or %s)\n",
                   MF1_ROM_FILE, MF1_ROM_FILE_FALLBACK);
            return;
        }
        printf("MF1:pressButton → plain NMI (MF disabled)\n");
        Z80::triggerNMI();
        return;
    }

    // Switch OFF: no NMI (Fuse + manual: interface unusable when OFF)
    if (!switchOn) {
        printf("MF1:pressButton BLOCKED switch OFF (stealth)\n");
        return;
    }

    // Already latched
    if (nmiPending) {
        printf("MF1:pressButton BLOCKED nmi already pending\n");
        return;
    }

    nmiPending = true;
    Z80::triggerNMI();
    printf("MF1:pressButton → MF NMI latched, activeNMI=%d\n", (int)Z80::isNMI());

}

void MultifaceOne::checkFetch(uint16_t pc) {

    if (!nmiPending || !isReady()) return;

    // M1 at $0066 or $0067 pages MF in
    if ((pc & 0xFFFE) != 0x0066) return;

    pageIn();
    // nmiPending stays set until OUT 0x1F (real FF_NMI_PENDING)

}

uint8_t MultifaceOne::portIn(uint16_t address) {

    if (!isReady()) return 0xff;

    bool a7 = (address & 0x0080) != 0;

    if (a7) {
        // IN 0x9F — page in if switch ON
        if (switchOn) {
            pageIn();
        }
    } else {
        // IN 0x1F — page out
        pageOut();
    }

    // Kempston-compatible joystick (D0–D4); D5–D7 tied low (default bridge)
    return Ports::port[0x1f] & 0x1f;

}

void MultifaceOne::portOut(uint16_t address, uint8_t /*data*/) {

    if (!isReady()) return;

    // Any matching OUT clears NMI pending (typically OUT 0x1F)
    (void)address;
    nmiPending = false;

}
