# PoC — link VGA32 ↔ T-Display

Dois programas independentes. Um vai na LilyGO VGA32, o outro no T-Display.
Prova de conceito: mandar texto de um lado para o outro e vice-versa, pelo cabo
CLK + DAT (através do level shifter).

Não depende de emulador, bootloader, C64, VGA, nem de nenhum outro firmware.

## Pinagem

```
Multiloader (LilyGO VGA32)    →  Level Shifter  →    T-Display
GPIO 26 (CLK)                    HV2 ↔ LV2            GPIO 17
GPIO 27 (DAT)                    HV1 ↔ LV1            GPIO 25
GND                              GND ↔ GND            GND
```

Alimentação (separada do sinal):

```
LilyGO VGA32  →  Level Shifter  →  T-Display
+5V (USB/VBUS) → HV (módulo)    →  5V do T-Display
GND comum nas três partes
3V3 do T-Display → LV (módulo)
```

Conferência no cabo USB:

- CLK VGA32 = GPIO 26 → USB pino 2 (D−, verde) → HV2 → LV2 → GPIO 17 T-Display
- DAT VGA32 = GPIO 27 → USB pino 3 (D+, amarelo) → HV1 → LV1 → GPIO 25 T-Display

Os GPIOs dos dois ESP32 são 3,3 V. O shifter bidirecional (BSS138) serve para
isolar os lados. Os pull-ups do módulo já puxam CLK/DAT; o I2C precisa disso.

## Como funciona

Dois fios: **CLK** = SCL, **DAT** = SDA. I2C a 50 kHz (cabo USB + shifter).

| Placa            | Papel          | CLK     | DAT     | Endereço |
|------------------|----------------|---------|---------|----------|
| VGA32            | mestre         | GPIO 26 | GPIO 27 | —        |
| T-Display        | escravo `0x28` | GPIO 17 | GPIO 25 | `0x28`   |

O mestre envia quando você digita (ou a cada 3 s um tick). Depois pergunta ao
escravo se há mensagem na fila. Assim os dois lados falam.

Pacote de 36 bytes: `magic 0x3C` + seq + len + texto (até 32 chars) + xor.

## Compilar / gravar

Precisa do [PlatformIO](https://platformio.org/). Cada pasta é um projeto.

```bash
# VGA32 (mestre) — ligue o USB da VGA32
cd poc_link/vga32
pio run -t upload
pio device monitor

# T-Display (escravo) — ligue o USB do T-Display
cd poc_link/tdisplay
pio run -t upload
pio device monitor
```

Grave os dois. Abra um monitor serial em cada um, 115200 8N1.

## Uso

1. Liga o T-Display (escravo) primeiro, depois a VGA32 (mestre).
2. Nos dois Serial Monitors deve aparecer o banner do PoC.
3. A cada ~3 s:
   - VGA32 envia `vga32 tick N`
   - T-Display enfileira `tdisp tick N` (o mestre busca no poll)
4. Digite uma linha e Enter em qualquer monitor: o outro lado imprime `[RX]`.
5. No T-Display:
   - botão GPIO 0 envia `tdisp botao 0`
   - botão GPIO 35 envia `tdisp botao 35`
   - a tela mostra o último RX e o último TX

## Se não comunicar

No monitor da VGA32:

| Mensagem | Significado |
|----------|-------------|
| `NACK addr (T-Display nao respondeu)` | CLK/DAT invertidos, GND faltando, LV sem 3V3, escravo não gravado, ou T-Display ainda não ligou |
| `requestFrom=0` | cabo / shifter / endereço |
| `pacote invalido` | ruído, CLK/DAT trocados, ou velocidade demais |

Cheque com o multímetro: continuidade GPIO26↔GPIO17 (CLK) e GPIO27↔GPIO25 (DAT),
GND comum, HV=5 V, LV=3,3 V do T-Display.
