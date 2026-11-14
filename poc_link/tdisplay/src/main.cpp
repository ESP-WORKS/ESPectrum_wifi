/*
 * PoC: LilyGO VGA32 <-> T-Display
 *
 * Este firmware roda no T-Display (lado LV do level shifter).
 * Escravo I2C. CLK = GPIO 17, DAT = GPIO 25.
 *
 * No Serial Monitor (115200):
 *   - digite uma linha e Enter para enviar a VGA32
 * Botoes:
 *   - GPIO 0  (BOT)  -> envia "tdisp botao 0"
 *   - GPIO 35 (BOT2) -> envia "tdisp botao 35"
 * A tela mostra o ultimo TX e o ultimo RX.
 */

#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>

static const int PIN_CLK = 17;  // SCL <- LV2 <- GPIO 26 da VGA32
static const int PIN_DAT = 25;  // SDA <- LV1 <- GPIO 27 da VGA32
static const int PIN_BTN0 = 0;
static const int PIN_BTN35 = 35;

static const uint8_t SLAVE_ADDR = 0x28;
static const uint32_t I2C_HZ = 50000;

static const uint8_t PKT_MAGIC = 0x3C;
static const uint8_t PKT_MAX_TEXT = 32;
static const uint8_t PKT_SIZE = 36;

static TFT_eSPI tft;

static uint8_t g_tx_seq = 0;
static uint8_t g_out_pkt[PKT_SIZE];
static uint8_t g_empty_pkt[PKT_SIZE];
static volatile bool g_out_ready = false;

static char g_last_rx[PKT_MAX_TEXT + 1] = "-";
static char g_last_tx[PKT_MAX_TEXT + 1] = "-";
static volatile bool g_got_rx = false;
static char g_rx_isr[PKT_MAX_TEXT + 1];

static uint32_t g_ok_rx = 0;
static uint32_t g_ok_tx = 0;
static uint32_t g_tick = 0;
static bool g_ui_dirty = true;

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
    return n > 0;
}

static void queue_text(const char *text) {
    noInterrupts();
    pack(g_out_pkt, g_tx_seq, text);
    g_out_ready = true;
    interrupts();

    strncpy(g_last_tx, text, sizeof(g_last_tx) - 1);
    g_last_tx[sizeof(g_last_tx) - 1] = '\0';
    g_ok_tx++;
    g_ui_dirty = true;
    Serial.printf("[TX queued] seq=%u \"%s\"\n", g_tx_seq, text);
    g_tx_seq++;
}

static void on_receive(int n) {
    uint8_t pkt[PKT_SIZE];
    memset(pkt, 0, sizeof(pkt));
    size_t got = 0;
    while (Wire.available() && got < PKT_SIZE) {
        pkt[got++] = (uint8_t)Wire.read();
    }
    while (Wire.available()) {
        Wire.read();
    }
    (void)n;

    uint8_t seq = 0;
    char text[PKT_MAX_TEXT + 1];
    if (!unpack(pkt, &seq, text, sizeof(text))) {
        return;
    }
    strncpy(g_rx_isr, text, sizeof(g_rx_isr) - 1);
    g_rx_isr[sizeof(g_rx_isr) - 1] = '\0';
    g_got_rx = true;
}

static void on_request() {
    if (g_out_ready) {
        Wire.write(g_out_pkt, PKT_SIZE);
        g_out_ready = false;
    } else {
        Wire.write(g_empty_pkt, PKT_SIZE);
    }
}

static void draw_ui() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.setCursor(4, 4);
    tft.print("T-Display  escravo I2C");

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(4, 18);
    tft.print("CLK17 DAT25  addr 0x28");

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.setCursor(4, 40);
    tft.print("RX:");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, 54);
    tft.print(g_last_rx);

    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(4, 78);
    tft.print("TX:");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setCursor(4, 92);
    tft.print(g_last_tx);

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.setCursor(4, 116);
    tft.printf("rx=%lu tx=%lu  BTN0/35", (unsigned long)g_ok_rx, (unsigned long)g_ok_tx);
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
                queue_text(line);
            }
            len = 0;
            continue;
        }
        if (len < PKT_MAX_TEXT) {
            line[len++] = c;
        }
    }
}

static bool btn_edge(int pin, bool *was_down) {
    bool down = digitalRead(pin) == LOW;
    bool edge = down && !(*was_down);
    *was_down = down;
    return edge;
}

void setup() {
    Serial.begin(115200);
    delay(200);

    pack(g_out_pkt, 0, "");
    pack(g_empty_pkt, 0, "");

    pinMode(PIN_BTN0, INPUT_PULLUP);
    pinMode(PIN_BTN35, INPUT);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    Serial.println();
    Serial.println("========================================");
    Serial.println(" PoC link  |  LilyGO T-Display (escravo)");
    Serial.println(" CLK = GPIO 17     DAT = GPIO 25");
    Serial.println(" 115200 baud. Digite uma linha + Enter.");
    Serial.println(" Botoes GPIO0 / GPIO35 enviam mensagem.");
    Serial.println("========================================");

    Wire.begin(SLAVE_ADDR, PIN_DAT, PIN_CLK, I2C_HZ);
    Wire.onReceive(on_receive);
    Wire.onRequest(on_request);

    draw_ui();
}

void loop() {
    if (g_got_rx) {
        noInterrupts();
        strncpy(g_last_rx, g_rx_isr, sizeof(g_last_rx) - 1);
        g_last_rx[sizeof(g_last_rx) - 1] = '\0';
        g_got_rx = false;
        interrupts();
        g_ok_rx++;
        g_ui_dirty = true;
        Serial.printf("[RX] \"%s\"\n", g_last_rx);
    }

    read_serial_line();

    static bool was0 = false;
    static bool was35 = false;
    if (btn_edge(PIN_BTN0, &was0)) {
        queue_text("tdisp botao 0");
    }
    if (btn_edge(PIN_BTN35, &was35)) {
        queue_text("tdisp botao 35");
    }

    static uint32_t last_tick = 0;
    uint32_t now = millis();
    if (now - last_tick >= 3000) {
        last_tick = now;
        char msg[PKT_MAX_TEXT + 1];
        snprintf(msg, sizeof(msg), "tdisp tick %lu", (unsigned long)g_tick++);
        queue_text(msg);
        Serial.printf("[STAT] rx_ok=%lu tx_q=%lu\n",
                      (unsigned long)g_ok_rx, (unsigned long)g_ok_tx);
    }

    if (g_ui_dirty) {
        g_ui_dirty = false;
        draw_ui();
    }

    delay(10);
}
