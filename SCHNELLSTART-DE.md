# Schnellstart (Deutsch)

Kurzanleitung zum Aufbau und zur Inbetriebnahme. Die vollständige Dokumentation
steht in [README.md](README.md), die ausführliche Windows-Anleitung in
[WINDOWS.md](WINDOWS.md) — beide auf Englisch.

---

## 1. Was Sie brauchen

| Teil | Hinweis |
|---|---|
| ESP32 DevKit v1 (30-polig) | jedes ESP32-Board geht, der Pinplan unten gilt für das 30-polige v1 |
| **2 ×** MCP2515 + TJA1050 CAN-Modul | das übliche blaue Modul. **Jeden Quarz einzeln prüfen** — 8 MHz oder 16 MHz, und zwei Module aus derselben Bestellung können sich unterscheiden |
| Micro-SD-Modul (SPI) | 3V3-Logik, aber **Versorgung über 5V (VIN)** |
| Micro-SD-Karte, **FAT32** | Class 10 oder besser. Karten über 32 GB sind meist exFAT und müssen neu formatiert werden |
| 120 Ω Widerstand, 0–2 Stück | einer je Bus, und nur dort, wo der Logger am Busende sitzt. Die beiden Entscheidungen sind unabhängig |

> **Der Quarz ist die häufigste Fehlerquelle.** Steht in `src/config.h` unter
> `CAN1_CRYSTAL_MHZ` bzw. `CAN2_CRYSTAL_MHZ` der falsche Wert, meldet der Logger
> „NO CAN TRAFFIC" für **diesen** Bus — obwohl er einwandfrei läuft. Die beiden
> Werte werden getrennt eingestellt.

---

## 2. Verdrahtung

**Beide CAN-Controller teilen sich einen SPI-Bus. Die SD-Karte bekommt ihren
eigenen.** Diese Trennung trägt das ganze Konzept: ein SD-Schreibvorgang dauert
Millisekunden — auf einer schlechten Karte Hunderte davon — und ein gemeinsamer
Bus würde das Lesen der CAN-Frames genau so lange blockieren. Zwei MCP2515 an
einem SPI-Bus kosten dagegen nichts, weil MISO hochohmig wird, solange CS high
ist: nur CS und INT müssen eindeutig sein.

| MCP2515 #1 → CAN1 | ESP32 | | MCP2515 #2 → CAN2 | ESP32 | | SD-Karte | ESP32 |
|---|---|---|---|---|---|---|---|
| VCC | 3V3 | | VCC | 3V3 | | VCC | **5V (VIN)** |
| GND | GND | | GND | GND | | GND | GND |
| CS  | **D22** | | CS  | **D5**  | | CS   | **D4**  |
| INT | **D21** | | INT | **D17** | | SCK  | **D14** |
| SCK | D18 | | SCK | D18 (geteilt) | | MISO | **D27** |
| MISO| D19 | | MISO| D19 (geteilt) | | MOSI | **D13** |
| MOSI| D23 | | MOSI| D23 (geteilt) | | | |

Die geteilten Leitungen SCK/MISO/MOSI kurz halten — ein Steckbrett-Stern mit
zwei langen Beinen ist die einzige Stelle, an der diese Topologie heikel wird.

> **Auf den meisten DevKit-v1-Boards gibt es keinen Pin „D17".** Dieses Board
> beschriftet UART2 nach Funktion: der Pin mit dem Aufdruck **TX2** ist GPIO17,
> **RX2** ist GPIO16. Der INT von CAN2 kommt an TX2. USB-Upload und serielle
> Ausgabe laufen über UART0 und sind davon nicht betroffen.

> **SD-Modul an 5V (VIN) versorgen, nicht an 3V3.** Fast alle Micro-SD-Platinen
> haben einen eigenen 3V3-Regler samt Pegelwandlern und erwarten 5 V an VCC. An
> 3V3 bricht die Spannung beim Schreiben ein, und `SD.begin()` scheitert genau
> so, als steckte gar keine Karte im Slot. Die SPI-Leitungen bleiben in beiden
> Fällen 3V3. Nur bei den seltenen Platinen ohne Regler ist 3V3 richtig.

Busseite: `CAN_H` und `CAN_L` jedes Moduls an **seinen eigenen** Bus, `GND` an
die jeweilige Busmasse. Voreingestellt sind **250 kBit/s je Bus**
(`CAN1_BITRATE_KBPS`, `CAN2_BITRATE_KBPS`); die beiden Busse müssen nicht
gleich schnell sein.

**Terminierung je Bus getrennt entscheiden.** 120 Ω gehören zwischen `CAN_H` und
`CAN_L` nur dort, wo der Logger am physikalischen Ende **dieses** Busses sitzt.
Am Ende des einen Busses zu sitzen sagt nichts über den anderen aus.

**Wichtig bei laufender Maschine:** Setzen Sie `CAN1_LISTEN_ONLY` bzw.
`CAN2_LISTEN_ONLY` (je Bus getrennt) in
`src/config.h` auf `1`, wenn bereits zwei oder mehr Teilnehmer am Bus hängen.
Der Logger sendet dann nie selbst. Steht er dagegen allein mit einem einzigen
Steuergerät am Bus, muss der Wert `0` bleiben — sonst quittiert niemand die
Frames und das Steuergerät geht auf Störung.

---

## 3. Software aufspielen

**Einfachster Weg — PlatformIO, ganz ohne Kommandozeile:**

1. Visual Studio Code installieren (<https://code.visualstudio.com>).
2. Erweiterung **PlatformIO IDE** installieren und die Installation abwarten
   (mehrere hundert MB).
3. **Datei → Ordner öffnen…** und genau den Ordner `platformio` auswählen —
   nicht den übergeordneten.
4. Unten in der blauen Leiste **✓** drücken (übersetzen), dann **→** (auf das
   Board schreiben), dann **🔌** (serielle Ausgabe ansehen).

**Vorher:** den USB-Treiber installieren, sonst erscheint gar kein COM-Port.
CP2102 → Treiber von Silicon Labs, CH340 → Treiber von WCH. Danach das Board
einmal ab- und wieder anstecken.

Wenn beim Schreiben die Meldung `Wrong boot mode detected (0x13)` erscheint:
Taste **`BOOT`** gedrückt halten, kurz **`EN`** drücken, **`BOOT`** weiter
halten und erst loslassen, wenn `Writing at 0x...` erscheint. Details und
Ursachen stehen in [WINDOWS.md](WINDOWS.md).

---

## 4. SD-Karte vorbereiten

Karte auf **FAT32** formatieren. Optional zwei Textdateien ins Hauptverzeichnis:

| Datei | Wozu |
|---|---|
| `frames.dbc` | DBC-Datei für **CAN1**. Damit werden dessen Signale in Echtzeit dekodiert — mit Namen und physikalischen Einheiten. Vorlage: `examples/example.dbc` |
| `frames2.dbc` | DBC-Datei für **CAN2**. Getrennt, weil dieselbe ID auf zwei Bussen üblicherweise Verschiedenes bedeutet. Fehlt sie, wird CAN2 als Rohdaten aufgezeichnet — das ist kein Fehler |
| `config.txt` | WLAN-Einstellungen. Fehlt sie, legt der Logger beim ersten Start eine kommentierte Vorlage an |

**Ohne DBC-Datei** zeichnet der Logger trotzdem alles auf — dann als rohe
Datenbytes. Das ist kein Fehler, sondern ein vorgesehener Betriebsmodus: Sie
können die Aufzeichnung später am PC gegen eine DBC dekodieren.

Weder für die DBC noch für das WLAN muss die Firmware neu übersetzt werden:
Datei ändern, Karte zurückstecken, Spannung aus und wieder ein.

---

## 5. Aufzeichnen

Der Logger startet **automatisch**, sobald SD-Karte und CAN-Controller bereit
sind — es geht beim Einschalten nichts verloren. Die Dateien werden fortlaufend
nummeriert: `1.csv`, `2.csv`, …, jeweils mit einer `1.log` daneben.

Am seriellen Monitor (115200 Baud) erscheint einmal pro Sekunde eine Zeile:

```
[   142.003] I REC 1.csv 00:02:21 | 141000 rows 3672 KB | 220 f/s | 7 ids | lost 0
```

**`lost 0` ist die wichtige Zahl.** Sie bedeutet, dass nachweislich kein
einziger Frame verloren ging.

---

## 6. Weboberfläche

Mit dem WLAN **`CAN-Logger`** verbinden (Passwort `canlogger`) und im Browser
**http://192.168.4.1** öffnen. Die Seite zeigt:

- Zustand von SD-Karte, Aufzeichnung, Bus und Datenintegrität,
- **alle dekodierten Signale** mit aktuellem Wert und Einheit (sofern eine DBC
  vorhanden ist),
- alle CAN-Identifier mit den zuletzt empfangenen Datenbytes,
- das laufende Protokoll.

Die Seite ist vollständig datengesteuert: Sie zeigt genau das, was die DBC-Datei
beschreibt. Ohne DBC erscheinen die Rohdaten.

Über die Schaltfläche **START / STOP** lässt sich die Aufzeichnung von Hand
steuern.

---

## 7. Wenn etwas nicht geht

| Meldung | Ursache |
|---|---|
| `NO SD CARD at any clock ...` | Zuerst die **Versorgung** prüfen: die meisten Module brauchen 5V an VIN, nicht 3V3. Dann: Karte FAT32? Verdrahtung CS=D4, SCK=D14, MISO=D27, MOSI=D13 |
| `SD card needed a slower clock` | Kein Fehler — die Karte ist eingebunden, nur unterhalb von `SD_SPI_HZ`. Ursache sind lange Jumper oder ein billiger Adapter. |
| `CAN1 CONTROLLER NOT RESPONDING` (bzw. CAN2) | Verdrahtung oder Spannung **dieses** MCP2515. Die Meldung nennt Bus und Pins. Antwortet nur einer der beiden, ist es fast immer der Chip-Select — das ist die einzige nicht geteilte Leitung |
| `NO CAN TRAFFIC` auf **einem** Bus | falscher `CAN1_CRYSTAL_MHZ` bzw. `CAN2_CRYSTAL_MHZ` für **dieses** Modul (8 statt 16), falsche Baudrate für diesen Bus, oder dessen CAN_H/CAN_L vertauscht |
| `NO CAN TRAFFIC ON EITHER BUS` | beide gleichzeitig falsch deutet auf die geteilte Verdrahtung: SCK/MISO/MOSI oder 3V3 |
| `CAN2 INTERRUPT NOT FIRING` (bzw. CAN1) | die INT-Leitung dieses Busses. Frames kommen weiter über den 20-ms-Notbetrieb an, aber nur noch ~100 Frames/s — es sieht also aus wie „läuft, nur langsam" |
| `CAN2: no /frames2.dbc on the card` | keine DBC für diesen Bus — er wird als Rohdaten aufgezeichnet (kein Fehler) |
| `lost` steigt an | SD-Karte zu langsam. Bessere Karte verwenden |
| kein COM-Port sichtbar | USB-Treiber fehlt, oder das USB-Kabel ist ein reines Ladekabel ohne Datenadern |
| letzte Sekunden fehlen nach Spannungsausfall | ohne Power-Fail-Eingang normal, maximal 1 Sekunde. Siehe README §8 |

Ausführlich: [README.md](README.md), Abschnitt 11.
