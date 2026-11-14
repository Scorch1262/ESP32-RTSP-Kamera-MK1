# P4-ETH-RTSP-Cam-MK1

**Version 1.3.0**

Firmware für **Waveshare ESP32-P4-ETH** und **Waveshare ESP32-S3-ETH**: Das Bild einer angeschlossenen **OV5640-Kamera** wird als **RTSP-Stream über die Ethernet-Schnittstelle** (RJ45, optional PoE) ins Netzwerk gesendet. Zusätzlich lässt sich eine **IR-Beleuchtung per MQTT ein- und ausschalten**.

![Aufbau: Kamera → ESP32-P4-ETH / ESP32-S3-ETH → RTSP / MQTT / Webseite, GPIO → Transistor → IR-Beleuchtung](docs/aufbau.svg)

## Unterstützte Boards

| | ESP32-P4-ETH | ESP32-S3-ETH |
|---|---|---|
| Kamera | OV5640 über MIPI-CSI (22-Pin) | OV5640 oder OV2640 über DVP (24-Pin) |
| Videoformat | **H.264** (Hardware-Encoder) | **MJPEG** (JPEG aus der Kamera, RFC 2435) |
| Auflösung | 1280×720 @ ~14 fps | 640×480 bis 1920×1080, auf der Webseite wählbar (Standard 1280×720) |
| Ethernet | interne EMAC + IP101 (100 Mbit/s) | W5500 über SPI |
| IR-GPIO (Standard) | 20 | 17 |

Der ESP32-S3 hat keinen H.264-Encoder. Deshalb liefert die Kamera dort fertige JPEG-Bilder, die unverändert als MJPEG-Stream verschickt werden. VLC, ffmpeg, go2rtc, Frigate und OpenCV spielen beides ab. MJPEG braucht aber deutlich mehr Bandbreite als H.264: Bei 1280×720 sind es je nach JPEG-Qualität etwa 5–15 Mbit/s. Wer mehr Bilder pro Sekunde braucht, wählt eine kleinere Auflösung oder eine höhere Qualitätszahl.

## Funktionen

- RTSP-Server mit RTP über **TCP (interleaved)** und **UDP (unicast)**, bis zu 4 Clients gleichzeitig
- P4: H.264 Baseline per Hardware, Bitrate/GOP/QP einstellbar; neue Clients bekommen sofort ein IDR-Frame
- S3: MJPEG mit Auflösung und JPEG-Qualität auf der Webseite einstellbar
- DHCP oder statische IP, Hostname einstellbar
- **IR-Beleuchtung ein/aus über MQTT**, z. B. `Tuer/Kamera` = `1` / `0` (GPIO frei wählbar)
- **Konfigurations-Webseite** unter `http://<IP-Adresse>/` (Netzwerk, MQTT-Broker, Schalt-Topic, IR-GPIO, Ein/Aus-Schalter, beim S3 auch Auflösung/Qualität) – gespeichert im NVS, kein Neukompilieren nötig
- Statusausgabe alle 10 s (fps, Bitrate, Clients, freier PSRAM) auf der seriellen Konsole

Stream-URL (beide Boards):

```
rtsp://<IP-Adresse>:554/stream
```

## Hardware

### ESP32-P4-ETH

| Funktion            | GPIO / Wert                                   |
|---------------------|-----------------------------------------------|
| Ethernet MDC / MDIO | 31 / 52                                       |
| PHY-Reset           | 51                                            |
| PHY                 | IP101, Adresse 1                              |
| RMII                | TXD0 34, TXD1 35, TX_EN 49, RXD0 29, RXD1 30, CRS_DV 28, REF_CLK 50 (Eingang) |
| Kamera SCCB SCL/SDA | 8 / 7                                         |

> Das Board ist für Raspberry-Pi-kompatible 22-Pin-Kameras (0,5 mm Raster) ausgelegt. Das OV5640-Modul muss MIPI-CSI-2 mit 2 Lanes und einen 22-Pin-Stecker haben (ggf. Adapterkabel 15→22 Pin).

### ESP32-S3-ETH

| Funktion | GPIO |
|---|---|
| W5500 MOSI / MISO / SCLK / CS | 11 / 12 / 13 / 14 |
| W5500 INT / RST | 10 / 9 |
| Kamera-Versorgung (Low = ein) | 8 |
| Kamera XCLK / PCLK / VSYNC / HREF | 3 / 39 / 1 / 2 |
| Kamera SIOD (SDA) / SIOC (SCL) | 48 / 47 |
| Kamera D0 … D7 | 41, 45, 46, 42, 40, 38, 15, 18 |

> GPIO8 schaltet die Spannungsversorgung der Kamera und muss Low sein, sonst wird die Kamera nicht erkannt. Die Firmware erledigt das automatisch. Das OV5640-Modul aus dem CAM-KIT und die OV2640 aus älteren Kits werden beide erkannt.

Alle Werte lassen sich in `idf.py menuconfig` → **P4-ETH-RTSP-Cam-MK1** ändern.

## Fertige Firmware flashen (ohne ESP-IDF)

Im Release liegen Gesamt-Images (Bootloader + Partitionstabelle + App), die **an Adresse `0x0`** geschrieben werden:

| Datei | Board |
|---|---|
| `P4-ETH-RTSP-Cam-MK1-v1.3.0-esp32s3.bin` | ESP32-S3-ETH |
| `P4-ETH-RTSP-Cam-MK1-v1.3.0-esp32p4-chip-rev-v3.bin` | ESP32-P4-ETH mit Chip v3.x |
| `P4-ETH-RTSP-Cam-MK1-v1.3.0-esp32p4-chip-rev-v1.bin` | ESP32-P4-ETH mit Chip v0.x / v1.x |

**Im Browser (Chrome/Edge):** <https://espressif.github.io/esptool-js/> öffnen → *Connect* → Port des Boards wählen → Flash Address `0x0`, Datei wählen → *Program*. Beim P4 steht die Chip-Revision nach *Connect* im Log.

**Per Kommandozeile:**
```bash
esptool.py --chip esp32s3 -b 460800 write_flash 0x0 P4-ETH-RTSP-Cam-MK1-v1.3.0-esp32s3.bin
esptool.py --chip esp32p4 -b 460800 write_flash 0x0 P4-ETH-RTSP-Cam-MK1-v1.3.0-esp32p4-chip-rev-v3.bin
```

> Falls der ESP32-S3 nicht in den Flash-Modus geht: BOOT-Taste gedrückt halten, kurz RESET drücken, BOOT loslassen.

> UF2-Dateien gibt es für beide Boards nicht: Die Chips haben ab Werk keinen UF2-Bootloader (kein „Laufwerk zum Draufkopieren“ wie beim RP2040).

## IR-Beleuchtung über MQTT schalten

1. IR-LED/IR-Strahler anschließen (siehe unten), Board starten, `http://<IP>/` öffnen.
2. MQTT-Broker eintragen (z. B. `mqtt://192.168.1.10:1883`), ggf. Benutzer/Passwort, **Schalt-Topic** (Standard `Tuer/Kamera`) und GPIO → *Speichern & Neustart*.
3. Schalten:

```bash
mosquitto_pub -h 192.168.1.10 -t Tuer/Kamera -m 1 -r     # ein
mosquitto_pub -h 192.168.1.10 -t Tuer/Kamera -m 0 -r     # aus
```

Akzeptiert werden `1`/`0`, `on`/`off`, `ein`/`aus`, `true`/`false` sowie `toggle`.
Mit `-r` (retained) übernimmt das Board den Zustand auch nach einem Neustart; ohne Broker startet die Beleuchtung **aus**.

| Topic | Richtung | Inhalt |
|---|---|---|
| `Tuer/Kamera` | → Board | `0` / `1` – Beleuchtung aus / ein |
| `Tuer/Kamera/Status` | ← Board | aktueller Zustand `0` / `1` (retained) |
| `Tuer/Kamera/Verfuegbar` | ← Board | `online` / `offline` (retained, Last Will) |
| `Tuer/Kamera/Info` | ← Board | Version, IP, RTSP-URL (retained) |

Wird über die Webseite geschaltet, schreibt das Board den neuen Wert retained auf das Schalt-Topic, damit Broker und Board übereinstimmen.

Ohne MQTT per HTTP:
```bash
curl -X POST http://<IP>/licht -d 1      # ein
curl -X POST http://<IP>/licht -d 0      # aus
```

### IR-Beleuchtung anschließen

Der GPIO liefert nur ein 3,3-V-Schaltsignal mit wenigen mA – die LED(s) immer über einen Transistor/MOSFET schalten:

**Einzelne IR-LED (bis ca. 100 mA):**
```
GPIO ──[ 1 kΩ ]── Basis  NPN (BC337 / 2N2222)
                  Emitter ── GND
5V ──[ Vorwiderstand ]──►|── Kollektor
```
Vorwiderstand ≈ (5 V − 1,4 V) / Strom, z. B. 47 Ω für ~75 mA.

**IR-Strahler / LED-Array (12 V):** Logic-Level-MOSFET (z. B. AO3400, IRLZ44N), Gate über 100 Ω an den GPIO, 100 kΩ Gate→GND, Strahler zwischen +12 V und Drain, Masse von Board und Netzteil verbinden. Fertige MOSFET-Module funktionieren ebenso.

Module mit Low-aktivem Eingang (z. B. viele Relais-Module): auf der Webseite „Ausgang Low-aktiv“ anhaken.

Der Standard-GPIO ist nur ein Vorschlag – auf der Webseite lässt sich jeder freie Pin der Stiftleiste einstellen.

- **ESP32-P4-ETH** (Standard 20): nicht verwenden 7/8 (Kamera), 28–35, 49–52 (Ethernet), 24–27 (USB), 37/38 (UART0).
- **ESP32-S3-ETH** (Standard 17): mit Kamera frei sind praktisch nur **16** und **17**. Nicht verwenden: alle Kamera-Pins (1–3, 8, 15, 18, 38–42, 45–48), 4–7 (TF-Karte), 9–14 (W5500), 21 (RGB-LED), 0 (BOOT), 43/44 (UART0).

## Webseite

`http://<IP>/` zeigt Status (IP, RTSP-URL, Clients, MQTT-Verbindung), einen Ein/Aus-Schalter für die IR-Beleuchtung und die Einstellungen. Zusätzlich gibt es `GET /api/status` (JSON). Die Seite hat keinen Passwortschutz – nur im eigenen Netz betreiben.

## Selbst bauen

Voraussetzung: **ESP-IDF v5.5.4** (von Waveshare empfohlen, mindestens v5.4).

```bash
idf.py set-target esp32p4      # oder: idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor      # Windows: -p COM5
```

Der IDF Component Manager lädt beim ersten Build automatisch die passenden Komponenten: `espressif/esp_video` und `espressif/esp_h264` für den P4, `espressif/esp32-camera` für den S3. Zielabhängige Einstellungen stehen in `sdkconfig.defaults.esp32p4` bzw. `sdkconfig.defaults.esp32s3`.

### Chip-Revision beachten (nur ESP32-P4)

ESP32-P4-Chips **v3.x** und **v0.x/v1.x** sind in ESP-IDF ≥ 5.5.2 nicht binärkompatibel. Revision prüfen:

```bash
esptool.py --chip esp32p4 chip_id
```

- **v3.x** → normal bauen (Standard)
- **v1.x** → so bauen:
  ```bash
  idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.rev_v1" build
  ```

Die Firmware erkennt beim Start selbst, ob die CSI-Einheit direkt YUV420 liefern kann (Revision v3: kein Kopieren) oder ob RGB565 per PPA (2D-DMA) umgewandelt werden muss (Revision v1).

## Stream abrufen

| Programm | Befehl / Einstellung |
|---|---|
| VLC | Medien → Netzwerkstream öffnen → `rtsp://<IP>:554/stream` |
| ffplay | `ffplay -rtsp_transport tcp rtsp://<IP>:554/stream` |
| ffmpeg (Aufnahme, P4/H.264) | `ffmpeg -rtsp_transport tcp -i rtsp://<IP>:554/stream -c copy aufnahme.mp4` |
| ffmpeg (Aufnahme, S3/MJPEG) | `ffmpeg -rtsp_transport tcp -i rtsp://<IP>:554/stream -c copy aufnahme.mkv` |
| Frigate / go2rtc / Home Assistant | Kamera-URL `rtsp://<IP>:554/stream`, Transport TCP empfohlen. Beim S3 (MJPEG) muss Frigate für die Aufnahme nach H.264 umkodieren (go2rtc mit `#video=h264`). |

## Einstellungen (menuconfig)

| Menü | Option | Standard |
|---|---|---|
| Ethernet (P4) | PHY-Typ / Adresse / Reset-GPIO | IP101 / 1 / 51 |
| Ethernet (S3) | W5500-Pins / SPI-Takt | siehe Tabelle oben / 36 MHz |
| Ethernet | Statische IP | aus (DHCP) |
| Kamera (P4) | SCCB-Pins, Reset/PWDN | 8 / 7, keine |
| Kamera (S3) | Auflösung / JPEG-Qualität / Spiegeln / alle DVP-Pins | 1280×720 / 12 / aus |
| H.264 (P4) | Bitrate | 2,5 Mbit/s |
| H.264 (P4) | GOP | 28 (≈ 2 s) |
| H.264 (P4) | QP min/max | 20 / 38 |
| RTSP | Port / Pfad / max. Clients | 554 / `stream` / 4 |
| MQTT / IR-Beleuchtung | Startwerte für Broker, Schalt-Topic, GPIO, Low-aktiv | leer / `Tuer/Kamera` / 20 (P4), 17 (S3) / nein |

## Fehlersuche

- **`esp_video_init fehlgeschlagen`** (P4) – Kabel/Stecker der Kamera prüfen (Kontakte richtig herum?), SCCB-Pins im menuconfig prüfen.
- **`esp_camera_init: ESP_ERR_NOT_FOUND`** (S3) – Kamera nicht erkannt: Flachbandkabel richtig herum eingesteckt? Board einmal stromlos machen.
- **Stream ruckelt beim S3** – Auflösung verkleinern oder JPEG-Qualitätszahl erhöhen (z. B. 15–20); über TCP abrufen.
- **Kein Link** – Kabel/Switch prüfen; beim P4 mit anderem PHY im menuconfig „Generischer IEEE-802.3-PHY“ und PHY-Adresse `-1` (Autosuche) wählen.
- **Ruckeln über UDP** – Client auf TCP umstellen (`-rtsp_transport tcp`, VLC: „RTP über RTSP (TCP)“).
- **Bootloop direkt nach dem Flashen** – falsches Image gewählt (Board bzw. P4-Chip-Revision, siehe oben).

## Projektstruktur

```
main/
  main.c          Start, Netzwerk, Webseite, MQTT
  eth_init.c      P4: EMAC + IP101 / S3: W5500 (SPI); DHCP / statische IP
  video_p4.c      P4: Kamera -> H.264-Hardware-Encoder -> RTSP
  camera.c        P4: OV5640 via esp_video (V4L2), YUV420-Ausgabe (direkt oder per PPA)
  video_s3.c      S3: OV5640/OV2640 via esp32-camera, JPEG -> RTSP
  rtsp_server.c   RTSP-Server, RTP-Paketierung H.264 (RFC 6184) und MJPEG (RFC 2435)
  ir_light.c      Schaltausgang der IR-Beleuchtung
  mqtt_ctrl.c     MQTT-Client: <topic> = 0/1, Status/Verfuegbar/Info
  web_ui.c        Konfigurations-Webseite, /licht, /api/status
  app_config.c    Einstellungen im NVS
  Kconfig.projbuild
sdkconfig.defaults(.esp32p4/.esp32s3)  gemeinsame und zielabhängige Einstellungen
.github/workflows/build.yml   Firmware für ESP32-S3 und beide P4-Chip-Revisionen, Release bei Tag v*
```
