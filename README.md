# P4-ETH-RTSP-Cam-MK1

**Version 1.0.0**

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

## Bauen und Flashen

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
  Kconfig.projbuild
.github/workflows/build.yml   Firmware für beide Chip-Revisionen, Release bei Tag v*
```
