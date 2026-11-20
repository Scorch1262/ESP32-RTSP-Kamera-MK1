# P4-ETH-RTSP-Cam-MK1

**Version 1.9.1**

Firmware für **Waveshare ESP32-P4-ETH** und **Waveshare ESP32-S3-ETH**: Das Bild einer angeschlossenen **OV5640-Kamera** wird als **RTSP-Stream über die Ethernet-Schnittstelle** (RJ45, optional PoE) ins Netzwerk gesendet. Zusätzlich lässt sich eine **IR-Beleuchtung per MQTT ein- und ausschalten**, bei **Bewegung im Bild** werden Clips auf die **MicroSD-Karte** gespeichert (über die Webseite herunterladbar), und beim ESP32-S3 kann das **eingebaute WLAN** genutzt werden.

![Aufbau: Kamera → ESP32-P4-ETH / ESP32-S3-ETH → RTSP / MQTT / Webseite, GPIO → Transistor → IR-Beleuchtung](docs/aufbau.svg)

## Unterstützte Boards

| | ESP32-P4-ETH | ESP32-S3-ETH |
|---|---|---|
| Kamera | OV5640 über MIPI-CSI (22-Pin) | OV5640 oder OV2640 über DVP (24-Pin) |
| Videoformat | **H.264** (Hardware-Encoder) | **MJPEG** (JPEG aus der Kamera, RFC 2435) |
| Auflösung | 1280×720 @ ~14 fps | 640×480 bis 1920×1080, auf der Webseite wählbar (Standard 1280×720) |
| Ethernet | interne EMAC + IP101 (100 Mbit/s) | W5500 über SPI |
| WLAN | – (das Board hat keinen Funkchip) | ja: mit WLAN verbinden oder eigener Access Point |
| MicroSD | SDIO 4-Bit | SDMMC 1-Bit |
| Aufnahmeformat | AVI mit H.264 | AVI mit MJPEG |
| IR-GPIO (Standard) | 20 | 17 |

Der ESP32-S3 hat keinen H.264-Encoder. Deshalb liefert die Kamera dort fertige JPEG-Bilder, die unverändert als MJPEG-Stream verschickt werden. VLC, ffmpeg, go2rtc, Frigate und OpenCV spielen beides ab. MJPEG braucht aber deutlich mehr Bandbreite als H.264: Bei 1280×720 sind es je nach JPEG-Qualität etwa 5–15 Mbit/s. Wer mehr Bilder pro Sekunde braucht, wählt eine kleinere Auflösung oder eine höhere Qualitätszahl.

## Funktionen

- RTSP-Server mit RTP über **TCP (interleaved)** und **UDP (unicast)**, bis zu 4 Clients gleichzeitig
- P4: H.264 Baseline per Hardware, Bitrate/GOP/QP einstellbar; neue Clients bekommen sofort ein IDR-Frame
- S3: MJPEG mit Auflösung und JPEG-Qualität auf der Webseite einstellbar
- DHCP oder statische IP, Hostname einstellbar
- **MQTT mit Zustand und Steuerung im selben Topic**: IR-Licht, Stream, Daueraufnahme, Aufzeichnung, Bewegung, Bewegungsaufnahme, Empfindlichkeit (GPIO der IR-Beleuchtung frei wählbar)
- **Konfigurations-Webseite** unter `http://<IP-Adresse>/` (Netzwerk, MQTT-Broker, Basis-Topic, IR-GPIO, Ein/Aus-Schalter, beim S3 auch Auflösung/Qualität) – gespeichert im NVS, kein Neukompilieren nötig
- **Bewegungserkennung** mit einstellbarer Empfindlichkeit; Lichtwechsel (z. B. IR-Licht an/aus) werden ignoriert
- **Aufnahme auf MicroSD** bei Bewegung, Clip-Länge einstellbar (5–600 s), älteste Aufnahmen werden bei voller Karte automatisch gelöscht (abschaltbar)
- **Daueraufnahme als Ringspeicher** per Knopf auf der Webseite (bleibt nach Neustart aktiv)
- **Aufnahmen-Seite** zum Herunterladen und Löschen der Videos
- **Dunkel-Meldung** und Bildhelligkeit per MQTT
- **Stream ein/aus** per MQTT (`<Basis>/Stream`) oder Webseite – Aufnahmen laufen dabei weiter
- **Kamera-Einstellungen** auf der Webseite (Helligkeit, Kontrast, Weißabgleich, Belichtung, Nachtmodus, Autofokus …) – angeboten wird nur, was der Sensor kann
- **Bild drehen** in 90°-Schritten über die Webseite (Stream, Aufnahmen und Bewegungserkennung)
- **WLAN (nur ESP32-S3)**: mit vorhandenem WLAN verbinden oder eigenen Access Point öffnen, parallel zum Ethernet
- Uhrzeit per NTP (Dateinamen mit Datum/Uhrzeit), ohne Internet wird die Uhrzeit vom Browser übernommen
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
| MicroSD CLK / CMD / D0–D3 | 43 / 44 / 39, 40, 41, 42 (Versorgung über LDO-Kanal 4) |

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
| MicroSD CLK / CMD / D0 | 7 / 6 / 5 |

> GPIO8 schaltet die Spannungsversorgung der Kamera und muss Low sein, sonst wird die Kamera nicht erkannt. Die Firmware erledigt das automatisch. Das OV5640-Modul aus dem CAM-KIT und die OV2640 aus älteren Kits werden beide erkannt.

Alle Werte lassen sich in `idf.py menuconfig` → **P4-ETH-RTSP-Cam-MK1** ändern.

## Fertige Firmware flashen (ohne ESP-IDF)

Im Release liegen Gesamt-Images (Bootloader + Partitionstabelle + App), die **an Adresse `0x0`** geschrieben werden:

| Datei | Board |
|---|---|
| `P4-ETH-RTSP-Cam-MK1-v1.9.1-esp32s3.bin` | ESP32-S3-ETH |
| `P4-ETH-RTSP-Cam-MK1-v1.9.1-esp32p4-chip-rev-v3.bin` | ESP32-P4-ETH mit Chip v3.x |
| `P4-ETH-RTSP-Cam-MK1-v1.9.1-esp32p4-chip-rev-v1.bin` | ESP32-P4-ETH mit Chip v0.x / v1.x |

**Im Browser (Chrome/Edge):** <https://espressif.github.io/esptool-js/> öffnen → *Connect* → Port des Boards wählen → Flash Address `0x0`, Datei wählen → *Program*. Beim P4 steht die Chip-Revision nach *Connect* im Log.

**Per Kommandozeile:**
```bash
esptool.py --chip esp32s3 -b 460800 write_flash 0x0 P4-ETH-RTSP-Cam-MK1-v1.9.1-esp32s3.bin
esptool.py --chip esp32p4 -b 460800 write_flash 0x0 P4-ETH-RTSP-Cam-MK1-v1.9.1-esp32p4-chip-rev-v3.bin
```

> Falls der ESP32-S3 nicht in den Flash-Modus geht: BOOT-Taste gedrückt halten, kurz RESET drücken, BOOT loslassen.

> UF2-Dateien gibt es für beide Boards nicht: Die Chips haben ab Werk keinen UF2-Bootloader (kein „Laufwerk zum Draufkopieren“ wie beim RP2040).

## IP-Adresse finden

1. **Router:** Das Board meldet sich per DHCP mit dem Hostnamen `p4-rtsp-cam` (auf der Webseite änderbar).
2. **Serieller Monitor:** Board per USB-C anschließen, Terminal mit 115200 Baud öffnen (z. B. esptool-js → *Console*, PuTTY, Arduino Serial Monitor, `idf.py monitor`). Alle 15 s erscheint eine Zeile wie
   ```
   I (15234) main: Status: verbunden | IP 192.168.1.57 | Web http://192.168.1.57/ | Stream rtsp://192.168.1.57:554/stream
   ```
   Vorher das Browser-Fenster von esptool-js schließen, sonst ist der Port noch belegt.
3. **Status-LED (ESP32-S3-ETH, RGB-LED an GPIO21):**

| LED | Bedeutung |
|---|---|
| blau | Start |
| rot blinkend | Ethernet-Chip (W5500) nicht gefunden |
| orange | kein Link – Netzwerkkabel/Switch prüfen |
| gelb blinkend | Link OK, warte auf IP-Adresse (DHCP) |
| grün | verbunden, Stream läuft |
| lila | verbunden, aber Kamera nicht erkannt |

## Bild drehen

Auf der Webseite unter **Einstellungen → Videobild** lässt sich das Bild in 90°-Schritten im Uhrzeigersinn drehen (0°, 90°, 180°, 270°). Nach **Speichern & Neustart** gilt die Drehung für den RTSP-Stream, die Aufnahmen auf der SD-Karte und die Bewegungserkennung.

| Board | 180° | 90° / 270° |
|---|---|---|
| ESP32-P4-ETH | verlustfrei per Grafikbaustein (PPA) | verlustfrei per PPA, Stream im Hochformat 720×1280 |
| ESP32-S3-ETH | verlustfrei im Kamerasensor (Spiegeln + Kippen) | per Software: Bild wird neu kodiert, max. 640×480 (Hochformat 480×640), ca. 8 Bilder/s |

## Kamera-Einstellungen

Die Webseite zeigt die Karte **Kamera-Einstellungen** mit allen Reglern, die der angeschlossene Sensor unterstützt. Änderungen wirken sofort (ohne Neustart), werden gespeichert und beim Start wieder gesetzt. **Alle auf Standard** stellt den Zustand nach dem Kamerastart wieder her. Diese Einstellungen gibt es nur auf der Webseite, nicht per MQTT.

| Gruppe | Regler |
|---|---|
| Bild | Helligkeit, Kontrast, Farbsättigung, Schärfe, Rauschunterdrückung, Effekt (Graustufen, Sepia, Negativ …) |
| Weißabgleich | automatisch ein/aus, Modus (Sonnig, Bewölkt, Büro, Zuhause) |
| Belichtung | automatische Belichtung/Verstärkung ein/aus, Belichtungskorrektur, manuelle Belichtungszeit und Verstärkung, Nachtmodus |
| Korrekturen | Objektiv-Korrektur, Pixelfehler-Korrektur, Gamma, Testbild |
| Fokus (nur OV5640) | Autofokus aus (Unendlich) / dauerhaft / einmalig, Knopf „Jetzt scharfstellen“, Statusanzeige |

- **ESP32-S3:** über den Kameratreiber esp32-camera; bei einem OV2640 erscheinen nur dessen Regler.
- **ESP32-P4:** direkte Registerzugriffe auf den OV5640 (der P4-Kameratreiber bietet selbst keine Bildregler).
- **Autofokus:** funktioniert nur mit OV5640-Modulen, die einen Fokusmotor haben. Beim ersten Einschalten wird die AF-Firmware (aus espressif/esp32-camera, Apache 2.0) in den Sensor geladen; das dauert einige Sekunden.

## WLAN (nur ESP32-S3-ETH)

Auf der Webseite unter *Einstellungen → WLAN*:

| Betriebsart | Verhalten |
|---|---|
| Aus | nur Ethernet (Standard) |
| Mit vorhandenem WLAN verbinden | WLAN-Name und Passwort eintragen (Knopf *WLAN suchen* listet die Netze in Reichweite). Das Board bekommt per DHCP eine zweite IP-Adresse; sie steht im Status auf der Webseite und im seriellen Monitor. |
| Eigener Access Point | Das Board öffnet ein eigenes WLAN (Name leer = `P4-RTSP-Cam-XXXX`). Passwort mit mind. 8 Zeichen = WPA2, sonst offen. Webseite dann unter `http://192.168.4.1/`. |

Ethernet und WLAN laufen gleichzeitig; Webseite, RTSP-Stream und MQTT sind über beide Wege erreichbar. Startet das Board ohne Netzwerkkabel, reicht das WLAN allein.

> Das **ESP32-P4-ETH** hat keinen Funkchip. WLAN gibt es bei Waveshare erst auf dem ESP32-P4-**WIFI6**-POE-ETH (mit zusätzlichem ESP32-C6) – dieses Board wird von der Firmware nicht unterstützt.

## Bewegungsaufnahmen auf der MicroSD-Karte

- Karte **FAT32-formatiert** einlegen (Karten über 32 GB werden meist mit exFAT ausgeliefert und müssen umformatiert werden, z. B. mit dem *SD Card Formatter* oder `guiformat`). Die Firmware formatiert nie selbst.
- Einstellungen auf der Webseite: *Bei Bewegung aufnehmen*, *Clip-Länge* (5–600 s), *Empfindlichkeit* (1–100), *Älteste Aufnahmen automatisch löschen*.
- Bei Bewegung startet ein Clip mit der eingestellten Länge. Hält die Bewegung an, folgt direkt der nächste Clip.
- Dateien liegen in `/rec` auf der Karte, Namen nach Datum/Uhrzeit und Anlass: `2026-11-14_18-42-07_bewegung.avi`, `…_dauer.avi`, `…_manuell.avi`. Ohne gültige Uhrzeit heißen sie `ohne-Uhrzeit_…avi`.
- Format: AVI – beim S3 mit MJPEG (läuft in jedem Player), beim P4 mit H.264 (VLC, Windows-Medienwiedergabe, ffmpeg).
- *Clip jetzt aufnehmen* auf der Webseite startet eine Aufnahme sofort (zum Testen).
- **Daueraufnahme** (Knopf *Daueraufnahme starten/beenden*): nimmt ohne Pause in Segmenten auf (Standard 300 s, einstellbar 60–3600 s). Ist die Karte voll, werden immer die ältesten Aufnahmen gelöscht (Ringspeicher) – auch wenn „automatisch löschen“ aus ist. Der Zustand bleibt nach einem Neustart erhalten. Während der Daueraufnahme starten keine zusätzlichen Bewegungsclips.
- Vor einem Neustart über *Speichern & Neustart* wird eine laufende Aufnahme sauber abgeschlossen.
- Die Aufnahmen-Seite `http://<IP>/aufnahmen` listet alle Videos (neueste zuerst) mit *Herunterladen* und *Löschen*.
- Dort gibt es auch die Taste **Alles von der SD-Karte löschen**: löscht nach doppelter Rückfrage (Eingabe `LOESCHEN`) alle Dateien und Ordner der Karte. Eine laufende Aufnahme wird vorher sauber beendet, eine eingeschaltete Daueraufnahme läuft danach weiter.

Bewegung und neue Aufnahmen werden zusätzlich per MQTT gemeldet (siehe unten).

## MQTT – Zustand und Steuerung im selben Topic

1. Board starten, `http://<IP>/` öffnen.
2. MQTT-Broker eintragen (z. B. `mqtt://192.168.1.10:1883`), ggf. Benutzer/Passwort und das **Basis-Topic** (Standard `Tuer/Kamera`) → *Speichern & Neustart*.

Jedes Topic zeigt den aktuellen Zustand und steuert zugleich die Funktion: Wer einen Wert sendet, schaltet; das Board meldet den tatsächlichen Zustand im selben Topic zurück.

| Topic | Zustand (Board meldet) | Steuerung (an das Board senden) |
|---|---|---|
| `Tuer/Kamera/IR-Licht` | `1` ein / `0` aus | `1` / `0` schaltet (auch `toggle`) |
| `Tuer/Kamera/Stream` | `1` RTSP-Stream ein / `0` aus | `1` / `0` schaltet den Stream (auch `toggle`, wird gespeichert) |
| `Tuer/Kamera/Daueraufnahme` | `1` / `0` | `1` / `0` Daueraufnahme (Ringspeicher) ein/aus |
| `Tuer/Kamera/Aufzeichnung` | `1` solange eine Aufzeichnung läuft (Bewegung, manuell oder Daueraufnahme) | `1` startet einen Clip, `0` beendet jede Aufzeichnung und schaltet die Daueraufnahme aus |
| `Tuer/Kamera/Bewegung` | `1` bei erkannter Bewegung, `0` nach 5 s ohne | `1` löst eine Bewegung aus (wie erkannt, startet ggf. einen Clip), `0` setzt zurück |
| `Tuer/Kamera/Bewegungsaufnahme` | `1` / `0` | `1` / `0` Aufnahme bei Bewegung ein/aus (wird gespeichert) |
| `Tuer/Kamera/Empfindlichkeit` | `1`–`100` | `1`–`100` Empfindlichkeit der Bewegungserkennung (wird gespeichert) |

Reine Meldungen (Messwerte, nicht steuerbar):

| Topic | Inhalt |
|---|---|
| `Tuer/Kamera/Dunkel` | `1` = Bild zu dunkel, `0` = hell genug |
| `Tuer/Kamera/Helligkeit` | mittlere Bildhelligkeit in % |
| `Tuer/Kamera/Aufnahme` | Dateiname einer neu gestarteten Aufnahme |
| `Tuer/Kamera/Verfuegbar` | `online` / `offline` (Last Will) |
| `Tuer/Kamera/Info` | Version, IP, RTSP-URL |

Werte wie `on`/`off`, `ein`/`aus`, `true`/`false` werden ebenfalls verstanden. Ist ein Befehl nicht ausführbar (z. B. `Aufzeichnung` = `1` ohne SD-Karte), meldet das Board im selben Topic den tatsächlichen Zustand zurück.

```bash
mosquitto_pub -h 192.168.1.10 -t Tuer/Kamera/IR-Licht -m 1          # IR-Licht ein
mosquitto_pub -h 192.168.1.10 -t Tuer/Kamera/Stream -m 0            # Videostream aus
mosquitto_pub -h 192.168.1.10 -t Tuer/Kamera/Daueraufnahme -m 1     # Daueraufnahme starten
mosquitto_sub -h 192.168.1.10 -t 'Tuer/Kamera/#' -v                 # alles mitlesen
```

**Eigene Meldungen:** Das Board abonniert die steuerbaren Topics und empfängt dadurch auch seine eigenen Zustandsmeldungen zurück. Jede eigene Meldung wird als erwartetes Echo vermerkt und beim Eintreffen abgehakt, auch wenn sie verspätet ankommt. So steuert sich das Board nie selbst, und kurz aufeinanderfolgende Befehle (z. B. ein/aus) von außen gehen trotzdem nicht verloren.

**Keine persistenten Meldungen:** Alle Nachrichten werden ohne Retain-Flag gesendet. Nach jeder (Neu-)Verbindung meldet das Board alle Zustände einmal. Nach einem Neustart ist das IR-Licht **aus**. Beim ersten Start ab v1.5.1 löscht das Board einmalig die von älteren Versionen gespeicherten (retained) Meldungen auf dem Broker.

> Ab v1.6.0 schaltet das reine Basis-Topic (`Tuer/Kamera` = `0`/`1`) nicht mehr – das IR-Licht wird über `Tuer/Kamera/IR-Licht` gesteuert.

### Stream ein/aus

`Tuer/Kamera/Stream` = `0` schaltet den RTSP-Stream ab: Verbundene Player (VLC, NVR …) werden getrennt, neue Verbindungen mit `503 Service Unavailable` abgewiesen. Kamera, Aufnahmen auf der SD-Karte, Bewegungserkennung und Dunkel-Meldung laufen weiter. Mit `1` ist der Stream sofort wieder abrufbar. Der Zustand bleibt nach einem Neustart erhalten und lässt sich auch auf der Webseite (Karte „Videostream“) schalten.

### Dunkel-Meldung

`Tuer/Kamera/Dunkel` wird `1`, wenn die mittlere Bildhelligkeit unter die Schwelle fällt (Standard 15 %), und wieder `0`, wenn sie 5 Prozentpunkte darüber liegt. Ein Wechsel wird erst gemeldet, wenn er 10 s lang anhält.

Ist das IR-Licht eingeschaltet, hellt es das Bild selbst auf. Damit eine Automatik wie „Dunkel = 1 → IR an“ nicht hin- und herschaltet, gilt das Bild bei eingeschaltetem IR-Licht erst ab einer höheren Helligkeit wieder als hell (Standard 60 %). Beide Schwellen lassen sich auf der Webseite einstellen.

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

`http://<IP>/` zeigt Status (IP, RTSP-URL, Clients, MQTT, WLAN, Uhrzeit, MicroSD, letzte Bewegung, Aufnahme), Ein/Aus-Schalter für den Videostream und die IR-Beleuchtung, die Kamera-Einstellungen, die Aufnahmen und alle Einstellungen.

| Adresse | Funktion |
|---|---|
| `/` | Status und Einstellungen |
| `/aufnahmen` | Liste der Videos mit Download/Löschen |
| `/sd/<datei>` | Video direkt herunterladen (z. B. per `curl -O`) |
| `POST /rec/start` | Clip sofort aufnehmen |
| `POST /rec/dauer` | Daueraufnahme `1`/`0` |
| `POST /sd-alles-loeschen` | Karte leeren (Inhalt muss `LOESCHEN` sein) |
| `POST /licht` | IR-Beleuchtung `1`/`0` |
| `POST /stream` | RTSP-Stream `1`/`0` (auch `toggle`) |
| `POST /kamera` | Kamera-Einstellung setzen, z. B. `brightness=1`, `af=1`, `af_trigger=1`, `reset=1` |
| `GET /api/kamera` | Sensor, Autofokus-Status und alle Kamera-Werte als JSON |
| `GET /api/status` | Status als JSON | Die Seite hat keinen Passwortschutz – nur im eigenen Netz betreiben.

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
| Ethernet (S3) | W5500-Pins / SPI-Takt | siehe Tabelle oben / 20 MHz |
| Status | RGB-LED-GPIO / Statusausgabe | 21 (S3), keine (P4) / alle 15 s |
| Ethernet | Statische IP | aus (DHCP) |
| Kamera (P4) | SCCB-Pins, Reset/PWDN | 8 / 7, keine |
| Kamera (S3) | Auflösung / JPEG-Qualität / Spiegeln / alle DVP-Pins | 1280×720 / 12 / aus |
| H.264 (P4) | Bitrate | 2,5 Mbit/s |
| H.264 (P4) | GOP | 28 (≈ 2 s) |
| H.264 (P4) | QP min/max | 20 / 38 |
| RTSP | Port / Pfad / max. Clients | 554 / `stream` / 4 |
| MQTT / IR-Beleuchtung | Startwerte für Broker, Basis-Topic, GPIO, Low-aktiv | leer / `Tuer/Kamera` / 20 (P4), 17 (S3) / nein |

## Fehlersuche

- **`esp_video_init fehlgeschlagen`** (P4) – Kabel/Stecker der Kamera prüfen (Kontakte richtig herum?), SCCB-Pins im menuconfig prüfen.
- **Terminal trennt sofort / Board taucht im Router nicht auf** – Das Board startet vermutlich ständig neu. Ab v1.6.0 läuft es auch bei Netzwerk- oder Kamerafehlern weiter, wartet nach einem Absturz 10 s und zeigt die Ursache im seriellen Monitor. Status-LED beachten (siehe „IP-Adresse finden“).
- **`W5500 antwortet nicht`** (S3) – Die Firmware versucht 20, 10 und 5 MHz SPI-Takt. Antwortet der Chip bei keinem davon, ist er nicht erreichbar (Stromversorgung bzw. PoE-Modul prüfen).
- **`esp_camera_init: ESP_ERR_NOT_FOUND`** (S3) – Kamera nicht erkannt: Flachbandkabel richtig herum eingesteckt? Board einmal stromlos machen.
- **„Keine MicroSD-Karte“ / „kein FAT-Dateisystem“** – Karte als FAT32 formatieren, einlegen, Board neu starten.
- **„SD-Karte zu langsam, Bild verworfen“** – schnellere Karte (Class 10 / U1 oder besser) verwenden oder beim S3 die Auflösung verringern.
- **„Bewegungserkennung: JPEG nicht dekodierbar“** (S3) – erscheint höchstens einmal pro Minute mit Angaben zum Bildformat. Ab v1.6.0 werden auch JPEGs ohne Huffman-Tabellen (typisch für OV5640/OV2640) dekodiert.
- **WLAN verbindet nicht** – Status auf der Webseite zeigt den Grund (z. B. Grund 15/202 = falsches Passwort, 201 = WLAN nicht gefunden). Der ESP32-S3 kann nur 2,4 GHz.
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
  cam_ctrl.c      Kamera-Einstellungen: Reglerliste, Speichern, Autofokus
  cam_ctrl_p4.c   P4: OV5640-Register per SCCB
  cam_ctrl_s3.c   S3: Sensorfunktionen von esp32-camera
  ov5640_af_fw.h  OV5640-Autofokus-Firmware (esp32-camera, Apache 2.0)
  rtsp_server.c   RTSP-Server, RTP-Paketierung H.264 (RFC 6184) und MJPEG (RFC 2435)
  ir_light.c      Schaltausgang der IR-Beleuchtung
  status_led.c    WS2812-Status-LED (S3: GPIO21)
  wifi_ctrl.c     WLAN-Client / Access Point (S3), Netzsuche
  sdcard.c        MicroSD-Karte (S3: SDMMC 1-Bit, P4: SDIO 4-Bit)
  motion.c        Bewegungserkennung auf verkleinertem Graustufenbild
  recorder.c      AVI-Aufnahmen mit eigenem Schreib-Task, Speicherverwaltung
  mqtt_ctrl.c     MQTT: Zustand + Steuerung je Topic, Echo-Erkennung, nicht persistent
  web_ui.c        Konfigurations-Webseite, /licht, /api/status
  app_config.c    Einstellungen im NVS
  Kconfig.projbuild
sdkconfig.defaults(.esp32p4/.esp32s3)  gemeinsame und zielabhängige Einstellungen
partitions.csv  Partitionstabelle (4 MB für die Firmware)
.github/workflows/build.yml   Firmware für ESP32-S3 und beide P4-Chip-Revisionen, Release bei Tag v*
```
