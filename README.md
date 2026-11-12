<p align="center">
  <img src="https://zxespectrum.speccy.org/wp-content/uploads/2023/05/ESPectrum-logo-v02-2.png" alt="ESPectrum" width="500">
</p>

# ESPectrum - World of Spectrum Wi-Fi Access (POC)

---

# 🇧🇷 Português

> [!WARNING]
> **ATENÇÃO!**
>
> Esta é uma versão de testes (**POC - Proof of Concept**) criada para verificar a possibilidade de acesso ao **World of Spectrum** utilizando a conexão Wi-Fi do ESP32.
>
> Testado apenas na **LillyGo ESP32 VGA**.

## Atualização

1. Baixe o arquivo **`firmware.upg`** na seção **Releases**.
2. Grave o arquivo no cartão SD.
3. No ESPectrum, acesse:
   ```
   F1 → Opções → Atualizar → Firmware
   ```

## Configuração de Rede

Após a atualização, um novo item será exibido no menu **F1**:

```
Acesso à Rede
```

1. Configure suas credenciais de Wi-Fi.
2. Configure o Proxy para:

```
http://alternativebits.com/wos.php
```

3. Busque o arquivo desejado e aproveite!

## Gamepad Bluetooth (Bluepad32)

Novo item no menu **F1**:

```
Bluetooth
```

- **Parear**: lista os gamepads em modo de pareamento e conecta automaticamente.
- **Desparear**: esquece o gamepad atual (ou todos os pareamentos).
- **Conectar**: tenta ligar um gamepad já pareado.
- Gamepads pareados reconectam sozinhos ao ligar o ESP32 (botão PS / Xbox / Home).
- O analog/D-pad vira joystick Kempston. O botão System (PS/Xbox) abre o menu F1.

Baseado em https://github.com/ricardoquesada/bluepad32

> [!CAUTION]
> **USE POR SUA CONTA E RISCO!**

### Repositório Original

https://github.com/EremusOne/ESPectrum

---

# 🇺🇸 English

> [!WARNING]
> **ATTENTION!**
>
> This is a test (**POC - Proof of Concept**) version created to verify the possibility of accessing **World of Spectrum** using the ESP32 Wi-Fi connection.
>
> Tested only on the **LillyGo ESP32 VGA**.

## Updating

1. Download the **`firmware.upg`** file from the **Releases** section.
2. Copy the file to the SD card.
3. On ESPectrum, navigate to:

   ```
   F1 → Options → Update → Firmware
   ```

## Network Configuration

After the update, a new menu item will appear under **F1**:

```
Network Access
```

1. Enter your Wi-Fi credentials.
2. Set the Proxy to:

```
http://alternativebits.com/wos.php
```

3. Search for the desired file and enjoy!

## Bluetooth gamepad (Bluepad32)

New item in the **F1** menu:

```
Bluetooth
```

- **Pair**: lists gamepads in pairing mode and connects automatically.
- **Unpair**: forgets the current gamepad (or all pairings).
- **Connect**: reconnects an already paired gamepad.
- Paired gamepads reconnect by themselves when the ESP32 boots (PS / Xbox / Home button).
- Analog/D-pad maps to Kempston. The System button (PS/Xbox) opens the F1 menu.

Based on https://github.com/ricardoquesada/bluepad32

> [!CAUTION]
> **USE AT YOUR OWN RISK!**

### Original Repository

https://github.com/EremusOne/ESPectrum

---

# 🇪🇸 Español

> [!WARNING]
> **¡ATENCIÓN!**
>
> Esta es una versión de prueba (**POC - Proof of Concept**) creada para verificar la posibilidad de acceder a **World of Spectrum** utilizando la conexión Wi-Fi del ESP32.
>
> Probado únicamente en la **LillyGo ESP32 VGA**.

## Actualización

1. Descargue el archivo **`firmware.upg`** desde la sección **Releases**.
2. Copie el archivo a la tarjeta SD.
3. En ESPectrum, vaya a:

   ```
   F1 → Opciones → Actualizar → Firmware
   ```

## Configuración de Red

Después de la actualización aparecerá un nuevo elemento en el menú **F1**:

```
Acceso a la Red
```

1. Introduzca sus credenciales de Wi-Fi.
2. Configure el Proxy en:

```
http://alternativebits.com/wos.php
```

3. Busque el archivo que desee y disfrútelo.

## Gamepad Bluetooth (Bluepad32)

Nuevo elemento en el menú **F1**:

```
Bluetooth
```

- **Emparejar**: lista los gamepads en modo de emparejamiento y conecta automáticamente.
- **Desemparejar**: olvida el gamepad actual (o todos los emparejamientos).
- **Conectar**: reconecta un gamepad ya emparejado.
- Los gamepads emparejados se reconectan solos al encender el ESP32 (botón PS / Xbox / Home).
- Analog/D-pad se mapea a Kempston. El botón System (PS/Xbox) abre el menú F1.

Basado en https://github.com/ricardoquesada/bluepad32

> [!CAUTION]
> **¡ÚSELO BAJO SU PROPIA RESPONSABILIDAD Y RIESGO!**

### Repositorio Original

https://github.com/EremusOne/ESPectrum
