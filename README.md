# esp32poeLarsBatMon

Battery monitor for the boat "Lars": an ESP32-S3 reads battery data from a
Bluetooth (BLE) BMS and publishes it over the network via PoE Ethernet and MQTT.

---

## Status — working end-to-end

- [x] **Board: Waveshare ESP32-S3-ETH** (ESP32-S3R8, 512 KB SRAM, 16 MB Flash,
      8 MB Octal PSRAM, WiFi/BLE 5, native USB-Serial/JTAG). MAC:
      `xx:xx:xx:xx:xx:xx` (redacted), connected via `/dev/cu.usbmodem101`.
      W5500 Ethernet over SPI: CLK=13 MISO=12 MOSI=11 CS=14 IRQ=10 RST=9.
      PoE via optional 802.3af module on the board.
- [x] **Battery: Redodo Power** 12.8 V / 100 Ah LiFePO4, model RH190, alias
      "R-12100BNNH19-C01278". MAC stored in `include/secrets.h` (gitignored,
      see [Usage](#usage) below). BLE GATT service `0xFFE0`, notify on
      `0xFFE1`, write on `0xFFE2`.
- [x] **Ethernet (W5500) + MQTT (PubSubClient)** working end-to-end. Retained
      heartbeat on `LiFePo01/status` (IP, uptime, free heap, `bms_connected`).
      Broker `192.168.24.213:1883` (the Cerbo/Venus GX's own MQTT broker), no auth.

      **Gotcha fixed**: `#define MQTT_MAX_PACKET_SIZE`/`MQTT_KEEPALIVE` placed
      before `#include <PubSubClient.h>` only takes effect in the one `.cpp`
      file that includes the header — not in PubSubClient's own
      separately-compiled source. The library silently kept its 256-byte
      default buffer, so the larger `LiFePo01/battery` payload (with
      `raw_hex`, ~600 bytes) failed to publish every time while the small
      `.../cells` payload kept working. Easy to miss because `publish()`'s
      return value was not being checked. Fixed by using the runtime calls
      `setBufferSize(2048)` and `setKeepAlive(60)` in `setup()` instead.

- [x] **Victron integration via Node-RED**, running on the Cerbo/Venus GX
      itself. Importable flow: `node-red/lifepo01-victron-flow.json`.
      Two MQTT-in nodes (`LiFePo01/battery`, `LiFePo01/battery/cells`) feed a
      function node that maps values to Victron D-Bus paths (`Dc/0/Voltage`,
      `Dc/0/Current`, `Dc/0/Power`, `Soc`, `Soh`, `Dc/0/Temperature`,
      `Capacity`, `TimeToGo` — estimated seconds until empty at the current
      discharge rate, capped at 10 days — `System/Min-/MaxCellVoltage`).
      These feed a `victron-virtual` node (device type "battery", 100 Ah/12 V)
      that creates a real virtual battery service on the D-Bus, visible in VRM
      and on the GX display.

      Setup: Settings → Node-RED (requires Venus OS Large, already installed)
      → open `https://<venus-ip>:1881/` → Import the flow → Deploy.

      **Confirmed live on the GX Battery detail page**: SoC 90.0 %, 13.3 V,
      4.4 A discharging, 58.0 W, 31.0 °C, min/max cell voltage 3.320 V/3.322 V,
      remaining time 21 h 45 min — matches the ESP32's own numbers
      (96.6 Ah / 4.4 A ≈ 21 h 57 min) closely enough to confirm `TimeToGo`
      is being computed and read correctly.

- [x] **BMS protocol solved.** It is *not* JBD/Xiaoxiang (that command set
      produced zero response even with a fully working GATT connection) —
      Redodo/LiTime/PowerQueen share a BMS OEM and use their own frame format
      on the same `0xFFE0/1/2` layout. Confirmed and implemented against the
      following open-source references:
      - <https://github.com/rubenmuehlhans/litime-ble-hacs> (Home Assistant
        integration; protocol details from `coordinator.py`/`const.py`)
      - <https://github.com/va13ak/esp_redodo_bms> (ESPHome; cross-check)

      Command frame (8 bytes, written to `0xFFE2`):
      `{0x00, 0x00, 0x04, 0x01, CMD, 0x55, 0xAA, CHECKSUM}`,
      `CHECKSUM = (0x04 + CMD) & 0xFF`. `CMD_QUERY_STATUS = 0x13`.
      The status response notifies on `0xFFE1`, little-endian, and may arrive
      fragmented (a new frame starts when `byte[2] == 0x65`; reassemble until
      >= 104 bytes). Byte offsets and all available commands (serial number,
      firmware version, charge/discharge on/off, …) are listed in the header
      comment of `src/main.cpp`.

      Live-verified output, matching the vendor app (SOC 96 %, 13.3 V,
      -4.1 A, 100.8 Ah at 2026-08-15 20:40) closely enough given the time
      elapsed between readings:
      ```json
      {"total_voltage_v":13.291,"current_a":-4.369,"power_w":-58.07,
       "soc_percent":92,"soh_percent":100,"remaining_capacity_ah":97.15,
       "full_charge_capacity_ah":105,"cell_temperature_c":31,
       "discharge_cycles":2,...}
      ```
      Cell voltages: `[3.323, 3.323, 3.323, 3.322]`
      (4S pack, 4 × 3.2 V ≈ 12.8 V nominal — checks out).

- [x] **BLE scanner crash fixed.** A nearby device flooding BLE advertisements
      at very high rate (Apple-continuity-style spam) overloaded the scan
      callback and corrupted serial output. Fixed with
      `scan->setDuplicateFilter(true)` in `setup()`.

### How we got here (for future reference)

The BMS's `0xFFE0/1/2/3` GATT layout is a generic UART-bridge pattern shared
by many unrelated BMS protocol families, so having the right UUIDs is not
enough. The JBD command set produced zero response despite a provably correct
GATT connection (service/characteristics discovered, notify subscribed with
peer ACK, writes ACKed). A full GATT dump also found an unrelated third-party
service (`f000ffc0-0451-4000-b000-000000000000`, TI CC254x/SensorTag-style
base UUID; the chip is a Beken BLE SoC per its Device Information Service)
that only replies with a fixed handshake/error frame regardless of input — a
dead end. What actually solved it: identifying the exact battery brand from a
photo of its label (Redodo Power) and searching for existing open-source
reverse-engineering of that brand's protocol, rather than continuing to guess
bytes or capture raw BLE traffic from the phone app.

### Known environment quirk (this machine)

`pio run` failed initially because PlatformIO's esptool auto-install
(`uv pip install --python=<nix-wrapped-python> -e tool-esptoolpy`) cannot
write into the read-only `/nix/store`. Even after manually creating
`~/.pio_venv`, the resulting `esptool` script picked up an incompatible
`click` version leaked in via the inherited `PYTHONPATH` from the
nix-packaged `platformio` wrapper. Fixed by rebuilding `~/.pio_venv` from
the *unwrapped* nix Python (`python3-3.13.12` without the `-env` suffix,
found via `find /nix/store -maxdepth 1 -iname "*-python3-3.1*"`) and
replacing `~/.pio_venv/bin/esptool` with a small shim that unsets
`PYTHONPATH` before running. `pio device monitor` also does not work from a
non-interactive shell (it requires a real TTY) — use a plain `pyserial` read
loop instead, or run `pio device monitor` from an actual terminal.

---

## TODO / Possible Extensions

All items below are optional — the core functionality is fully working.

- [ ] **Alarms instead of raw flags.** `LiFePo01/battery` already publishes
      `protection_flags` and `failure_flags` as raw bitmasks from the BMS,
      but they are not decoded or forwarded as Victron alarms. Known bits
      (from `litime-ble-hacs`): `0x04` = Overcharge, `0x20` = Over-discharge,
      `0x40` = Charge-Overcurrent, `0x80` = Discharge-Overcurrent,
      `0x100`/`0x200` = High-Temp 1/2, `0x400`/`0x800` = Low-Temp 1/2,
      `0x4000` = Short-Circuit. Map these in the Node-RED function node to the
      corresponding Victron paths: `Alarms/HighVoltage`, `Alarms/LowVoltage`,
      `Alarms/HighChargeCurrent`, `Alarms/HighDischargeCurrent`,
      `Alarms/HighTemperature`, `Alarms/LowTemperature`,
      `Alarms/InternalFailure` (each 0/1). Additionally, compute
      threshold-based alarms locally (not from the BMS): `Alarms/LowSoc`
      (e.g. `soc_percent < 20`) and `Alarms/CellImbalance`
      (e.g. max - min cell voltage > 0.05 V).

- [ ] **Read serial number and firmware version.** Known but not yet
      implemented commands (same 8-byte frame scheme):
      `CMD_SERIAL_NUMBER = 0x10`, `CMD_FIRMWARE_VERSION = 0x16`. Response
      format unknown; would need to be verified against the real device, as
      the status frame was. Nice-to-have for debugging / asset tracking; there
      is no dedicated Victron D-Bus path for this (could go into `CustomName`
      or similar).

- [ ] **DVCC: charger follows BMS limits.** Currently the virtual battery is
      monitoring-only and does not control the charger. To let the charger
      adopt the limits reported by the BMS, also publish
      `Info/MaxChargeVoltage`, `Info/MaxChargeCurrent`,
      `Info/MaxDischargeCurrent`, and `Info/BatteryLowVoltage` (from the
      measured values or the nameplate), then enable DVCC in the Venus OS
      system settings. This is a larger change.

- [ ] **Remote charge/discharge control (FET control).** The BMS supports
      write commands per `litime-ble-hacs`: `CMD_CHARGE_ON/OFF = 0x0A/0x0B`,
      `CMD_DISCHARGE_ON/OFF = 0x0C/0x0D`. These could be exposed via an
      additional MQTT topic (e.g. `LiFePo01/battery/set`). Implement with
      care — safety-critical; consider restricting to Discharge-Off as an
      emergency stop only.

- [ ] **Robustness: Ethernet reconnect.** ETH reconnect after cable/switch
      failure has not yet been tested under real conditions (only MQTT and BLE
      reconnect have been). A watchdog timer (`esp_task_wdt`) would be useful
      in case the firmware ever gets stuck in one of the BLE wait states.

---

## Hardware

- Waveshare ESP32-S3-ETH (W5500 PoE Ethernet)
- Battery: Redodo Power 12.8 V / 100 Ah LiFePO4 (model RH190), MAC in
  `include/secrets.h` (gitignored)
- MQTT broker at `192.168.24.213:1883`

---

## Usage

One-time setup before the first build:

```sh
cp include/secrets.h.example include/secrets.h
# Enter the real BMS_MAC_ADDRESS in include/secrets.h
```

`include/secrets.h` is gitignored and contains the real battery MAC address —
it is never committed.

Build and flash:

```sh
pio run -t upload
```

Reading serial output: `pio device monitor` does not work from a
non-interactive shell (requires a real TTY). Use a plain `pyserial` read loop
instead, or run `pio device monitor` from an actual terminal.

MQTT topics:
- `LiFePo01/status` — heartbeat (retained)
- `LiFePo01/battery` — voltage, current, SoC, capacity, … (retained)
- `LiFePo01/battery/cells` — individual cell voltages as an array (retained)

Victron/Node-RED flow for import: `node-red/lifepo01-victron-flow.json`
(details in the Status section above).

---

## Deutsch (Schweizer Hochdeutsch)

Batteriemonitor fuer das Boot "Lars": ein ESP32-S3 (Waveshare ESP32-S3-ETH)
liest Batteriedaten von der BLE-Batterie (Redodo Power 12.8 V / 100 Ah
LiFePO4) aus und stellt sie ubers Netzwerk per PoE-Ethernet und MQTT bereit.
Von dort werden die Daten per Node-RED-Flow in Victrons D-Bus eingespeist und
erscheinen als echte virtuelle Batterie in VRM und am GX-Display.

Das System laeuft vollstaendig End-to-End und ist live am GX-Display
bestaetigt: Ethernet, MQTT, das BMS-Protokoll und die Victron-Integration
funktionieren alle und liefern plausible, konsistente Werte (SoC, Spannung,
Strom, Leistung, Temperatur, Zellspannungen, geschaetzte Restlaufzeit).

Das urspruenglich vermutete JBD-Standardprotokoll war eine Sackgasse. Die
Loesung war, die Batteriemarke (Redodo) vom Typenschild abzulesen und danach
zu suchen — fuer diese Marke (gemeinsam mit LiTime und PowerQueen) existiert
bereits offen dokumentierter, funktionierender Code. Dabei kamen noch zwei
subtile Fehler hinzu: ein BLE-Scanner-Absturz durch ein flutartig sendendes
Nachbargeraet (behoben mit `setDuplicateFilter(true)`) und ein stiller
MQTT-Publish-Fehlschlag durch einen PubSubClient-Puffer, der trotz `#define`
bei den Standard-256-Bytes blieb (behoben mit `setBufferSize()` und
`setKeepAlive()` zur Laufzeit).

### Einrichtung

Einmalig vor dem ersten Build:

```sh
cp include/secrets.h.example include/secrets.h
# Echte BMS_MAC_ADDRESS in include/secrets.h eintragen
```

`include/secrets.h` ist gitignored und enthaelt die reale Batterie-MAC —
wird nie committed.

```sh
pio run -t upload
```

MQTT-Topics:
- `LiFePo01/status` — Heartbeat (retained)
- `LiFePo01/battery` — Spannung, Strom, SoC, Kapazitaet, … (retained)
- `LiFePo01/battery/cells` — Einzelzellspannungen als Array (retained)

Victron-/Node-RED-Flow zum Import: `node-red/lifepo01-victron-flow.json`

### Moegliche Erweiterungen

Alle Punkte sind optional — die Grundfunktion laeuft bereits vollstaendig.

- [ ] **Alarme statt nur Rohwerte.** `protection_flags` und `failure_flags`
      werden bereits publiziert, aber nicht als Victron-Alarme weitergereicht.
      Bekannte Bits (aus `litime-ble-hacs`): `0x04` = Ueberladung,
      `0x20` = Tiefentladung, `0x40` = Lade-Ueberstrom,
      `0x80` = Entlade-Ueberstrom, `0x100`/`0x200` = Hohe Temperatur 1/2,
      `0x400`/`0x800` = Niedrige Temperatur 1/2, `0x4000` = Kurzschluss.
      Zusaetzlich schwellwertbasiert: `Alarms/LowSoc` und
      `Alarms/CellImbalance`.

- [ ] **Seriennummer und Firmware-Version auslesen.** Bekannte, aber noch
      nicht implementierte Kommandos: `CMD_SERIAL_NUMBER = 0x10`,
      `CMD_FIRMWARE_VERSION = 0x16`. Antwortformat unbekannt, muss am echten
      Geraet verifiziert werden.

- [ ] **DVCC: Ladegeraet folgt den BMS-Limits.** Aktuell ist die virtuelle
      Batterie reines Monitoring. Um DVCC zu aktivieren, muessen zusaetzlich
      `Info/MaxChargeVoltage`, `Info/MaxChargeCurrent`,
      `Info/MaxDischargeCurrent` und `Info/BatteryLowVoltage` gesetzt und
      DVCC in den Venus-OS-Systemeinstellungen aktiviert werden.

- [ ] **Lade-/Entladesteuerung aus der Ferne.** Das BMS unterstuetzt
      Schreibkommandos (`CMD_CHARGE_ON/OFF`, `CMD_DISCHARGE_ON/OFF`). Diese
      koennten ueber ein zusaetzliches MQTT-Topic fernsteuerbar gemacht werden
      — sicherheitsrelevant, daher mit Bedacht implementieren.

- [ ] **Robustheit: Ethernet-Reconnect.** ETH-Reconnect nach Kabel-/
      Switch-Ausfall wurde noch nicht unter Realbedingungen getestet.
      Ein Watchdog-Timer (`esp_task_wdt`) waere sinnvoll.
