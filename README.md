# P4-ETH-RTSP-Cam-MK1

**Version 1.1.0**

Firmware für das **Waveshare ESP32-P4-ETH**: Das Bild einer angeschlossenen **OV5640-Kamera (MIPI-CSI, 2 Lanes)** wird mit dem **Hardware-H.264-Encoder** des ESP32-P4 kodiert und als **RTSP-Stream über die Ethernet-Schnittstelle** (RJ45, optional PoE) ins Netzwerk gesendet.

```
OV5640 ──MIPI-CSI──► ESP32-P4 (CSI/ISP ► [PPA] ► H.264-HW-Encoder) ──RTP/RTSP──► Ethernet
```

## Funktionen

- 1280×720 @ ~14 fps (Standardformat des OV5640-Treibers über MIPI-CSI)
- H.264 Baseline per Hardware, Bitrate/GOP/QP einstellbar
- RTSP-Server mit RTP über **TCP (interleaved)** und **UDP (unicast)**, bis zu 4 Clients gleichzeitig
- Neue Clients bekommen sofort ein IDR-Frame (kein Warten auf das nächste GOP)
- DHCP oder statische IP, Hostname einstellbar
- **IR-Sender über MQTT**: Steuerbefehle (NEC, Samsung, Sony, RC5, Rohdaten, Pronto-Hex) an eine IR-LED an einem frei wählbaren GPIO
- **Konfigurations-Webseite** unter `http://<IP-Adresse>/` (Netzwerk, MQTT-Broker, IR-GPIO, IR-Testsender) – gespeichert im NVS, kein Neukompilieren nötig
- Statusausgabe alle 10 s (fps, Bitrate, Clients, freier PSRAM) auf der seriellen Konsole

Stream-URL:

```
rtsp://<IP-Adresse>:554/stream
```

## Hardware

| Funktion            | GPIO / Wert                                   |
|---------------------|-----------------------------------------------|
| Ethernet MDC / MDIO | 31 / 52                                       |
| PHY-Reset           | 51                                            |
| PHY                 | IP101, Adresse 1                              |
| RMII                | TXD0 34, TXD1 35, TX_EN 49, RXD0 29, RXD1 30, CRS_DV 28, REF_CLK 50 (Eingang) |
| Kamera SCCB SCL/SDA | 8 / 7                                         |

Alle Werte lassen sich in `idf.py menuconfig` → **P4-ETH-RTSP-Cam-MK1** ändern.

> **Hinweis zur Kamera:** Das Board ist für Raspberry-Pi-kompatible 22-Pin-Kameras (0,5 mm Raster) ausgelegt. Das OV5640-Modul muss MIPI-CSI-2 mit 2 Lanes und einen 22-Pin-Stecker haben (ggf. Adapterkabel 15→22 Pin).

## Fertige Firmware flashen (ohne ESP-IDF)

Im Release liegen zwei Gesamt-Images (Bootloader + Partitionstabelle + App), die **an Adresse `0x0`** geschrieben werden:

| Datei | Chip-Revision |
|---|---|
| `P4-ETH-RTSP-Cam-MK1-v1.1.0-chip-rev-v3.bin` | ESP32-P4 v3.x |
| `P4-ETH-RTSP-Cam-MK1-v1.1.0-chip-rev-v1.bin` | ESP32-P4 v0.x / v1.x |

**Im Browser (Chrome/Edge):** <https://espressif.github.io/esptool-js/> öffnen → *Connect* → Port des Boards wählen → Flash Address `0x0`, Datei wählen → *Program*. Die Chip-Revision steht nach *Connect* im Log.

**Per Kommandozeile:**
```bash
esptool.py --chip esp32p4 -b 460800 write_flash 0x0 P4-ETH-RTSP-Cam-MK1-v1.1.0-chip-rev-v3.bin
```

> Eine UF2-Datei gibt es für den ESP32-P4 nicht: Der Chip hat keinen UF2-Bootloader (kein „Laufwerk zum Draufkopieren“ wie beim RP2040).

## IR-Steuerung über MQTT

1. IR-LED anschließen (siehe unten), Board starten, `http://<IP>/` öffnen.
2. MQTT-Broker eintragen (z. B. `mqtt://192.168.1.10:1883`), ggf. Benutzer/Passwort, Basis-Topic und IR-GPIO → *Speichern & Neustart*.
3. Befehle als JSON an `<Basis-Topic>/ir/send` schicken (Standard-Basis-Topic: `p4cam`).

| Topic | Richtung | Inhalt |
|---|---|---|
| `p4cam/ir/send` | → Board | IR-Befehl (JSON) |
| `p4cam/ir/result` | ← Board | `{"ok":true,"message":"NEC gesendet (...)"}` |
| `p4cam/status` | ← Board | `online` / `offline` (retained, Last Will) |
| `p4cam/info` | ← Board | Version, IP, RTSP-URL (retained) |

**Befehlsformate** (Zahlen dezimal oder als String `"0x.."`, `repeat` optional):

```json
{"protocol":"NEC","address":"0x04","command":"0x08"}
{"protocol":"NEC","address":"0x04","command":"0x08","repeat":3}
{"protocol":"NEC","address":"0xBF40","command":"0x12"}
{"protocol":"SAMSUNG","address":"0x07","command":"0x02"}
{"protocol":"SONY","address":1,"command":21,"bits":12}
{"protocol":"RC5","address":0,"command":12}
{"raw":[9000,4500,560,560,560,1690,560],"freq":38000}
{"raw":[9000,-4500,560,-560,560]}
{"pronto":"0000 006D 0022 0002 0157 00AC 0015 0016 ..."}
```

- NEC: Adresse > 0xFF = Extended-NEC (16 Bit); `repeat` sendet NEC-Wiederholcodes (Taste gehalten).
- Sony: 12/15/20 Bit, 40 kHz, standardmäßig 3 Sendungen.
- RC5: 36 kHz, Toggle-Bit wechselt automatisch (oder `"toggle":0/1`).
- `raw`: Dauern in µs, abwechselnd Puls/Pause – oder positiv = Puls, negativ = Pause (Format von ESPHome/IRremote). `freq` überschreibt die Trägerfrequenz (20–60 kHz).
- `pronto`: Pronto-Hex aus IR-Datenbanken (z. B. irdb), Trägerfrequenz wird aus dem Code übernommen.

Beispiel mit Mosquitto:
```bash
mosquitto_pub -h 192.168.1.10 -t p4cam/ir/send -m '{"protocol":"NEC","address":"0x04","command":"0x08"}'
mosquitto_sub -h 192.168.1.10 -t 'p4cam/#' -v
```

Ohne MQTT kann man über die Webseite oder per HTTP testen:
```bash
curl -X POST http://<IP>/ir -d '{"protocol":"NEC","address":4,"command":8}'
```

### IR-LED anschließen

Ein GPIO liefert nur wenige mA – für brauchbare Reichweite die LED über einen Transistor treiben:

```
GPIO (Standard 20) ──[ 1 kΩ ]── Basis  NPN (BC337 / 2N2222)
                                Emitter ── GND
3V3 / 5V ──[ 22–47 Ω ]──►|── Kollektor        (IR-LED 940 nm, Anode an Vorwiderstand)
```

Der Standard-GPIO 20 ist nur ein Vorschlag – auf der Webseite lässt sich jeder freie Pin der Stiftleiste einstellen. Nicht verwenden: 7/8 (Kamera), 28–35, 49–52 (Ethernet), 24–27 (USB), 37/38 (UART0).

## Webseite

`http://<IP>/` zeigt Status (IP, RTSP-URL, Clients, MQTT-Verbindung, gesendete IR-Befehle), einen IR-Testsender und die Einstellungen. Zusätzlich gibt es `GET /api/status` (JSON). Die Seite hat keinen Passwortschutz – nur im eigenen Netz betreiben.

## Selbst bauen

Voraussetzung: **ESP-IDF v5.5.4** (von Waveshare empfohlen, mindestens v5.4).

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor      # Windows: -p COM5
```

Die Komponenten `espressif/esp_video` (inkl. OV5640-Treiber) und `espressif/esp_h264` lädt der IDF Component Manager beim ersten Build automatisch.

### Chip-Revision beachten

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
| ffmpeg (Aufnahme) | `ffmpeg -rtsp_transport tcp -i rtsp://<IP>:554/stream -c copy aufnahme.mp4` |
| Frigate / go2rtc / Home Assistant | Kamera-URL `rtsp://<IP>:554/stream`, Transport TCP empfohlen |

## Einstellungen (menuconfig)

| Menü | Option | Standard |
|---|---|---|
| Ethernet | PHY-Typ / Adresse / Reset-GPIO | IP101 / 1 / 51 |
| Ethernet | Statische IP | aus (DHCP) |
| Kamera | SCCB-Pins, Reset/PWDN | 8 / 7, keine |
| H.264 | Bitrate | 2,5 Mbit/s |
| H.264 | GOP | 28 (≈ 2 s) |
| H.264 | QP min/max | 20 / 38 |
| RTSP | Port / Pfad / max. Clients | 554 / `stream` / 4 |
| MQTT / IR | Startwerte für Broker, Topic, IR-GPIO | leer / `p4cam` / 20 |

## Fehlersuche

- **`esp_video_init fehlgeschlagen`** – Kabel/Stecker der Kamera prüfen (Kontakte richtig herum?), SCCB-Pins im menuconfig prüfen.
- **Kein Link** – Kabel/Switch prüfen; bei anderem PHY im menuconfig „Generischer IEEE-802.3-PHY“ und PHY-Adresse `-1` (Autosuche) wählen.
- **Ruckeln über UDP** – Client auf TCP umstellen (`-rtsp_transport tcp`, VLC: „RTP über RTSP (TCP)“).
- **Bootloop direkt nach dem Flashen** – falsche Chip-Revision gewählt (siehe oben).

## Projektstruktur

```
main/
  main.c          Start, Encoder-/Streaming-Task, Statistik
  eth_init.c      EMAC + IP101, DHCP / statische IP
  camera.c        OV5640 via esp_video (V4L2), YUV420-Ausgabe (direkt oder per PPA)
  rtsp_server.c   RTSP-Server, RTP-Paketierung (RFC 6184, FU-A)
  ir_tx.c         IR-Sender (RMT mit Traegermodulation), Protokolle + JSON-Parser
  mqtt_ctrl.c     MQTT-Client, IR-Befehle aus <topic>/ir/send
  web_ui.c        Konfigurations-Webseite, /ir, /api/status
  app_config.c    Einstellungen im NVS
  Kconfig.projbuild
.github/workflows/build.yml   Firmware für beide Chip-Revisionen, Release bei Tag v*
```
