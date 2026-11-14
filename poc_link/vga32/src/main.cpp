/*
 * PoC: LilyGO VGA32 <-> T-Display
 *
 * Este firmware roda na VGA32 (lado HV do level shifter).
 * Mestre I2C. CLK = GPIO 26, DAT = GPIO 27.
 *
 * No Serial Monitor (115200):
 *   - digite uma linha e Enter para enviar ao T-Display
 *   - mensagens recebidas aparecem prefixadas com [RX]
 */

#include <Arduino.h>
#include <Wire.h>

static const int PIN_CLK = 26;  // SCL -> HV2 -> GPIO 17 do T-Display
static const int PIN_DAT = 27;  // SDA -> HV1 -> GPIO 25 do T-Display

static const uint8_t PEER_ADDR = 0x28;
static const uint32_t I2C_HZ = 50000;  // cabo USB + shifter: ir devagar

static const uint8_t PKT_MAGIC = 0x3C;
static const uint8_t PKT_MAX_TEXT = 32;
static const uint8_t PKT_SIZE = 36;  // magic + seq + len + 32 text + csum

static uint8_t g_tx_seq = 0;
static uint32_t g_ok_tx = 0;
static uint32_t g_ok_rx = 0;
static uint32_t g_fail = 0;
static uint32_t g_tick = 0;

static uint8_t checksum(const uint8_t *p, size_t n) {
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
    }
    return c;
}

static void pack(uint8_t *out, uint8_t seq, const char *text) {
    memset(out, 0, PKT_SIZE);
    size_t n = strlen(text);
    if (n > PKT_MAX_TEXT) {
        n = PKT_MAX_TEXT;
    }
    out[0] = PKT_MAGIC;
    out[1] = seq;
    out[2] = (uint8_t)n;
    memcpy(&out[3], text, n);
    out[PKT_SIZE - 1] = checksum(out, PKT_SIZE - 1);
}

static bool unpack(const uint8_t *in, uint8_t *seq, char *text, size_t text_cap) {
    if (in[0] != PKT_MAGIC) {
        return false;
    }
    if (checksum(in, PKT_SIZE - 1) != in[PKT_SIZE - 1]) {
        return false;
    }
    uint8_t n = in[2];
    if (n > PKT_MAX_TEXT) {
        return false;
    }
    *seq = in[1];
    if (text_cap == 0) {
        return false;
    }
    size_t copy = n;
    if (copy >= text_cap) {
        copy = text_cap - 1;
    }
    memcpy(text, &in[3], copy);
    text[copy] = '\0';
    return true;
}

static const char *tx_err(uint8_t e) {
    switch (e) {
        case 0: return "ok";
        case 1: return "buffer";
        case 2: return "NACK addr (T-Display nao respondeu)";
        case 3: return "NACK data";
        case 4: return "erro";
        case 5: return "timeout";
        default: return "desconhecido";
    }
}

static bool send_text(const char *text) {
    uint8_t pkt[PKT_SIZE];
    pack(pkt, g_tx_seq, text);

    Wire.beginTransmission(PEER_ADDR);
    Wire.write(pkt, PKT_SIZE);
    uint8_t err = Wire.endTransmission();
    if (err != 0) {
        g_fail++;
        Serial.printf("[TX FAIL] %s  err=%u  msg=\"%s\"\n", tx_err(err), err, text);
        return false;
    }

    Serial.printf("[TX] seq=%u \"%s\"\n", g_tx_seq, text);
    g_tx_seq++;
    g_ok_tx++;
    return true;
}

static void poll_peer() {
    uint8_t n = Wire.requestFrom((int)PEER_ADDR, (int)PKT_SIZE);
    if (n == 0) {
        g_fail++;
        Serial.println("[RX FAIL] requestFrom=0 (sem resposta)");
        return;
    }

    uint8_t pkt[PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));
    size_t got = 0;
    while (Wire.available() && got < PKT_SIZE) {
        pkt[got++] = (uint8_t)Wire.read();
    }
    while (Wire.available()) {
        Wire.read();
    }

    if (got < PKT_SIZE) {
        g_fail++;
        Serial.printf("[RX FAIL] curto: %u bytes\n", (unsigned)got);
        return;
    }

    uint8_t seq = 0;
    char text[PKT_MAX_TEXT + 1];
    if (!unpack(pkt, &seq, text, sizeof(text))) {
        /* pacote vazio (len=0, ainda sem mensagem) ou lixo */
        if (pkt[0] == PKT_MAGIC && pkt[2] == 0) {
            return;
        }
        g_fail++;
        Serial.printf("[RX FAIL] pacote invalido  magic=0x%02X len=%u\n", pkt[0], pkt[2]);
        return;
    }

    if (pkt[2] == 0 || text[0] == '\0') {
        return;
    }

    g_ok_rx++;
    Serial.printf("[RX] seq=%u \"%s\"\n", seq, text);
}

static void read_serial_line() {
    static char line[PKT_MAX_TEXT + 4];
    static size_t len = 0;

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            if (len > 0) {
                send_text(line);
            }
            len = 0;
            continue;
        }
        if (len < PKT_MAX_TEXT) {
            line[len++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    Serial.println();
    Serial.println("========================================");
    Serial.println(" PoC link  |  LilyGO VGA32  (mestre I2C)");
    Serial.println(" CLK = GPIO 26     DAT = GPIO 27");
    Serial.println(" 115200 baud. Digite uma linha + Enter.");
    Serial.println(" A cada 3s envia um tick automatico.");
    Serial.println("========================================");

    Wire.begin(PIN_DAT, PIN_CLK, I2C_HZ);
    Wire.setTimeOut(50);
}

void loop() {
    read_serial_line();

    static uint32_t last_poll = 0;
    static uint32_t last_tick = 0;
    uint32_t now = millis();

    if (now - last_poll >= 200) {
        last_poll = now;
        poll_peer();
    }

    if (now - last_tick >= 3000) {
        last_tick = now;
        char msg[PKT_MAX_TEXT + 1];
        snprintf(msg, sizeof(msg), "vga32 tick %lu", (unsigned long)g_tick++);
        send_text(msg);
        Serial.printf("[STAT] tx_ok=%lu rx_ok=%lu fail=%lu\n",
                      (unsigned long)g_ok_tx, (unsigned long)g_ok_rx, (unsigned long)g_fail);
    }
}
