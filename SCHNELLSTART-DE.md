# Schnellstart (Deutsch)

Kurzanleitung zum Aufbau und zur Inbetriebnahme. Die vollständige Dokumentation
steht in [README.md](README.md), die ausführliche Windows-Anleitung in
[WINDOWS.md](WINDOWS.md) — beide auf Englisch.

---

## 1. Was Sie brauchen

| Teil | Hinweis |
|---|---|
| ESP32 DevKit v1 (30-polig) | jedes ESP32-Board geht, der Pinplan unten gilt für das 30-polige v1 |
| MCP2515 + TJA1050 CAN-Modul | das übliche blaue Modul. **Quarz prüfen** — 8 MHz oder 16 MHz |
| Micro-SD-Modul (SPI) | 3V3-Logik |
| Micro-SD-Karte, **FAT32** | Class 10 oder besser. Karten über 32 GB sind meist exFAT und müssen neu formatiert werden |
| 120 Ω Widerstand | nur wenn der Logger am Busende sitzt |

> **Der Quarz ist die häufigste Fehlerquelle.** Steht in `src/config.h` unter
> `CAN_CRYSTAL_MHZ` der falsche Wert, meldet der Logger „NO CAN TRAFFIC" —
> obwohl der Bus einwandfrei läuft.

---

## 2. Verdrahtung

Bewusst **zwei getrennte SPI-Busse**: ein SD-Schreibvorgang dauert
Millisekunden, und ein gemeinsamer Bus würde das Lesen der CAN-Frames genau so
lange blockieren.

| MCP2515 | ESP32 | | SD-Karte | ESP32 |
|---|---|---|---|---|
| VCC | 3V3 | | VCC | 3V3 |
| GND | GND | | GND | GND |
| CS  | **D5**  | | CS   | **D4**  |
| INT | **D17** | | SCK  | **D14** |
| SCK | D18 | | MISO | **D27** |
| MISO| D19 | | MOSI | **D13** |
| MOSI| D23 | | | |

Busseite: `CAN_H` und `CAN_L` an den Bus, `GND` an die Busmasse. Voreingestellt
sind **250 kBit/s**.

**Wichtig bei laufender Maschine:** Setzen Sie `CAN_LISTEN_ONLY` in
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
| `frames.dbc` | Ihre DBC-Datei. **Damit werden Signale in Echtzeit dekodiert** — mit Namen und physikalischen Einheiten. Vorlage: `examples/example.dbc` |
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
| `SD CARD NOT FOUND` | Karte nicht FAT32, oder Verdrahtung: CS=D4, SCK=D14, MISO=D27, MOSI=D13 |
| `CAN CONTROLLER NOT RESPONDING` | Verdrahtung oder Spannung des MCP2515: CS=D5, 3V3 |
| `NO CAN TRAFFIC`, Bus läuft aber | falscher `CAN_CRYSTAL_MHZ` (8 statt 16), falsche Baudrate, oder CAN_H/CAN_L vertauscht |
| `no /frames.dbc on the card` | keine DBC auf der Karte — es wird alles als Rohdaten aufgezeichnet (kein Fehler) |
| `lost` steigt an | SD-Karte zu langsam. Bessere Karte verwenden |
| kein COM-Port sichtbar | USB-Treiber fehlt, oder das USB-Kabel ist ein reines Ladekabel ohne Datenadern |
| letzte Sekunden fehlen nach Spannungsausfall | ohne Power-Fail-Eingang normal, maximal 1 Sekunde. Siehe README §8 |

Ausführlich: [README.md](README.md), Abschnitt 11.
