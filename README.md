# esp32poeLarsBatMon

Battery monitor for the boat "Lars": an ESP32-S3 reads battery data from a
Bluetooth (BLE) BMS and makes it available over the network via PoE Ethernet
and MQTT.

## Status — working end-to-end

- [x] Board: **Waveshare ESP32-S3-ETH** (ESP32-S3R8, 512KB SRAM, 16MB Flash,
      8MB Octal PSRAM, WiFi/BLE 5, native USB-Serial/JTAG). MAC:
      `xx:xx:xx:xx:xx:xx` (redacted), connected via `/dev/cu.usbmodem101`. W5500
      Ethernet over SPI: CLK=13 MISO=12 MOSI=11 CS=14 IRQ=10 RST=9. PoE via
      optional 802.3af module on the board.
- [x] Battery: **Redodo Power** 12.8V/100Ah LiFePO4, model RH190, alias
      "R-12100BNNH19-C01278", MAC in `include/secrets.h` (gitignored, see
      Verwendung below). BLE GATT service
      `0xFFE0`, notify on `0xFFE1`, write on `0xFFE2`.
- [x] Ethernet (W5500) + MQTT (PubSubClient) working end-to-end. Retained
      heartbeat on `LiFePo01/status` (IP, uptime, free heap,
      `bms_connected`). Broker `192.168.24.213:1883` (the Cerbo/Venus GX's
      own MQTT broker), no auth.
      **Gotcha fixed**: `#define MQTT_MAX_PACKET_SIZE`/`MQTT_KEEPALIVE`
      before `#include <PubSubClient.h>` only takes effect in this one
      `.cpp` file, not PubSubClient's own separately-compiled source - the
      library silently kept its 256-byte default buffer regardless,
      so the larger `LiFePo01/battery` payload (with `raw_hex`, ~600
      bytes) failed to publish every time while the small `.../cells`
      payload kept working, easy to miss since `publish()`'s return value
      wasn't being checked. Fixed with the runtime
      `setBufferSize(2048)`/`setKeepAlive(60)` calls in `setup()` instead.
- [x] **Victron integration via Node-RED**, running on the Cerbo/Venus GX
      itself. `node-red/lifepo01-victron-flow.json` (importable flow):
      two MQTT-in nodes (`LiFePo01/battery`, `LiFePo01/battery/cells`) →
      function node mapping to Victron D-Bus paths (`Dc/0/Voltage`,
      `Dc/0/Current`, `Dc/0/Power`, `Soc`, `Soh`, `Dc/0/Temperature`,
      `Capacity`, `TimeToGo` — estimated seconds until empty at the
      current discharge rate, capped at 10 days — `System/Min-/
      MaxCellVoltage`) → a `victron-virtual` node
      (device type "battery", 100Ah/12V) that creates a real virtual
      battery service on the D-Bus, visible in VRM and on the GX display.
      Setup: Settings → Node-RED (needs Venus OS Large, already installed)
      → open `https://<venus-ip>:1881/` → Import the flow → Deploy.
      **Confirmed live on the GX device's Battery detail page**: SoC
      90.0%, 13.3V, 4.4A discharging, 58.0W, 31.0°C, min/max cell voltage
      3.320V/3.322V, remaining time 21h45m — matches the ESP32's own
      remaining-capacity/current numbers (96.6Ah / 4.4A ≈ 21h57m) closely
      enough to confirm `TimeToGo` is being computed and read correctly.
- [x] **BMS protocol solved.** It's *not* JBD/Xiaoxiang (that command set
      got zero response, even with a fully working GATT connection) —
      Redodo/LiTime/PowerQueen share a BMS OEM and use their own frame
      format on the same `0xFFE0/1/2` layout. Confirmed and implemented
      against the real, working open-source references:
      - https://github.com/rubenmuehlhans/litime-ble-hacs (Home Assistant
        integration, protocol details taken from `coordinator.py`/`const.py`)
      - https://github.com/va13ak/esp_redodo_bms (ESPHome, cross-check)

      Command frame (8 bytes, written to `0xFFE2`):
      `{0x00, 0x00, 0x04, 0x01, CMD, 0x55, 0xAA, CHECKSUM}`,
      `CHECKSUM = (0x04 + CMD) & 0xFF`. `CMD_QUERY_STATUS = 0x13`.
      Status response notifies on `0xFFE1`, little-endian, may arrive
      fragmented (new frame starts when `byte[2] == 0x65`, reassemble until
      ≥104 bytes). Byte offsets and other available commands (serial
      number, firmware version, charge/discharge on/off, ...) are listed in
      the header comment of `src/main.cpp`.
  - Live-verified output, matching the vendor app (SOC 96%, 13.3V, -4.1A,
        100.8Ah at 2026-08-15 20:40) closely enough to account for the time
        elapsed between readings:
        `{"total_voltage_v":13.291,"current_a":-4.369,"power_w":-58.07,
        "soc_percent":92,"soh_percent":100,"remaining_capacity_ah":97.15,
        "full_charge_capacity_ah":105,"cell_temperature_c":31,
        "discharge_cycles":2,...}`, cell voltages
        `[3.323,3.323,3.323,3.322]` (4S pack, 4×3.2V ≈ 12.8V nominal — checks out).
- [x] Found and fixed a firmware crash: a nearby device flooding BLE
      advertisements at very high rate (Apple-continuity-style spam)
      overloaded the scan callback and corrupted serial output. Fixed with
      `scan->setDuplicateFilter(true)` in `setup()`.

### How we got there (for future reference)

The BMS's `0xFFE0/1/2/3` GATT layout is a generic UART-bridge pattern
shared by many unrelated BMS protocol families, so having the right
UUIDs is not enough — the JBD command set produced zero response despite
a provably correct GATT connection (service/characteristics discovered,
notify subscribed with peer ACK, writes ACKed). A full GATT dump also
found an unrelated third-party service (`f000ffc0-0451-4000-b000-
000000000000`, TI CC254x/SensorTag-style base UUID, chip is a Beken BLE
SoC per its Device Information Service) that replies but only with a
fixed handshake/error frame regardless of input — a dead end. What
actually solved it: identifying the exact battery brand from a photo of
its label (Redodo Power) and searching for existing open-source
reverse-engineering of that brand's protocol, rather than continuing to
guess bytes or capturing raw BLE traffic from the phone app.

### Known environment quirk (this machine)

`pio run` failed initially because PlatformIO's esptool auto-install
(`uv pip install --python=<nix-wrapped-python> -e tool-esptoolpy`) can't
write into the read-only `/nix/store`, and even after manually creating
`~/.pio_venv` the resulting `esptool` script picked up an incompatible
`click` version leaked in via inherited `PYTHONPATH` from the nix-packaged
`platformio` wrapper. Fixed by rebuilding `~/.pio_venv` from the *unwrapped*
nix Python (`python3-3.13.12` without `-env` suffix, found via
`find /nix/store -maxdepth 1 -iname "*-python3-3.1*"`) and replacing
`~/.pio_venv/bin/esptool` with a small shim that unsets `PYTHONPATH` before
running. `pio device monitor` also doesn't work from a non-interactive
shell (needs a real TTY) — use a plain `pyserial` read loop instead, or run
`pio device monitor` from an actual terminal.

## TODO / mögliche Erweiterungen

Alles hier ist optional — die Grundfunktion läuft bereits vollständig.

- [ ] **Alarme statt nur Rohwerte.** `LiFePo01/battery` liefert bereits
      `protection_flags`/`failure_flags` als rohe Bitmasken vom BMS, werden
      aber weder decodiert noch als Victron-Alarme weitergereicht. Bekannte
      Bits (aus `litime-ble-hacs`):
      `0x04`=Overcharge, `0x20`=Over-discharge, `0x40`=Charge-Overcurrent,
      `0x80`=Discharge-Overcurrent, `0x100`/`0x200`=High-Temp 1/2,
      `0x400`/`0x800`=Low-Temp 1/2, `0x4000`=Short-Circuit. Im
      Node-RED-Function-Node auf die passenden Victron-Pfade mappen:
      `Alarms/HighVoltage`, `Alarms/LowVoltage`, `Alarms/HighChargeCurrent`,
      `Alarms/HighDischargeCurrent`, `Alarms/HighTemperature`,
      `Alarms/LowTemperature`, `Alarms/InternalFailure` (jeweils 0/1).
      Zusätzlich rein schwellwertbasiert (nicht vom BMS, selbst
      berechnen): `Alarms/LowSoc` (z.B. `soc_percent < 20`),
      `Alarms/CellImbalance` (z.B. `max-min Zellspannung > 0.05V`).
- [ ] **Seriennummer & Firmware-Version auslesen.** Bekannte, aber noch
      nicht implementierte Kommandos (gleiches 8-Byte-Frame-Schema,
      `CMD_SERIAL_NUMBER = 0x10`, `CMD_FIRMWARE_VERSION = 0x16`) —
      Antwortformat unbekannt, müsste am echten Gerät verifiziert werden
      wie beim Status-Frame. Nice-to-have für Debug-Zwecke/Asset-Tracking,
      kein Victron-D-Bus-Pfad dafür vorgesehen (könnte höchstens in
      `CustomName` o.ä. landen).
- [ ] **DVCC: Ladegerät folgt den BMS-Limits.** Aktuell ist unsere virtuelle
      Batterie reines Monitoring, sie steuert das Ladegerät nicht. Um das
      Ladegerät die vom BMS gemeldeten Grenzwerte übernehmen zu lassen,
      zusätzlich `Info/MaxChargeVoltage`, `Info/MaxChargeCurrent`,
      `Info/MaxDischargeCurrent`, `Info/BatteryLowVoltage` aus den
      Batteriewerten (bzw. Typenschild-Werten) setzen, plus DVCC in den
      Venus-OS-Systemeinstellungen aktivieren. Grösserer Umbau, siehe
      frühere Diskussion im Chat-Verlauf.
- [ ] **Lade-/Entladesteuerung (FET-Control) aus der Ferne.** BMS
      unterstützt laut `litime-ble-hacs` auch Schreibkommandos
      `CMD_CHARGE_ON/OFF = 0x0A/0x0B`, `CMD_DISCHARGE_ON/OFF = 0x0C/0x0D`.
      Könnte man z.B. über ein zusätzliches MQTT-Topic
      (`LiFePo01/battery/set`) fernsteuerbar machen — sicherheitsrelevant,
      also mit Bedacht implementieren (z.B. nur Discharge-Off als
      Notausschalter, nicht beides).
- [ ] **Robustheit:** ETH-Reconnect nach Kabel-/Switch-Ausfall noch nicht
      unter Realbedingungen getestet (nur MQTT- und BLE-Reconnect sind es).
      Watchdog-Timer (`esp_task_wdt`) wäre sinnvoll, falls die Firmware mal
      in einem der BLE-Wartezustände hängen bleibt.

## Hardware

- Waveshare ESP32-S3-ETH (W5500 PoE Ethernet)
- Battery: Redodo Power 12.8V/100Ah LiFePO4 (model RH190), MAC in
  `include/secrets.h` (gitignored)
- MQTT broker at `192.168.24.213:1883`

## Verwendung

Einmalig vor dem ersten Build:

```sh
cp include/secrets.h.example include/secrets.h
# dann in include/secrets.h die echte BMS_MAC_ADDRESS eintragen
```

`include/secrets.h` ist gitignored und enthält die reale Batterie-MAC —
wird nie committed.

```sh
pio run -t upload
```

Serial-Ausgabe lesen (im nicht-interaktiven Terminal funktioniert
`pio device monitor` nicht, siehe unten — stattdessen `pyserial` direkt
benutzen oder `pio device monitor` in einem echten Terminal starten).

MQTT-Topics:
- `LiFePo01/status` — Heartbeat (retained)
- `LiFePo01/battery` — Spannung/Strom/SoC/Kapazität/... (retained)
- `LiFePo01/battery/cells` — Einzelzellspannungen als Array (retained)

Victron/Node-RED-Flow zum Import: `node-red/lifepo01-victron-flow.json`
(Details siehe Status oben).

## Deutsch

Batteriemonitor für das Boot "Lars": ein ESP32-S3 (Waveshare ESP32-S3-ETH)
liest Batteriedaten von der BLE-Batterie (Redodo Power 12.8V/100Ah LiFePO4)
aus und stellt sie übers Netzwerk per PoE-Ethernet via MQTT bereit, von wo
sie per Node-RED-Flow in Victrons D-Bus eingespeist werden und als echte
virtuelle Batterie in VRM und am GX-Display erscheinen.

Läuft vollständig End-to-End und ist live am GX-Display bestätigt: Ethernet,
MQTT, das BMS-Protokoll und die Victron-Integration funktionieren alle und
liefern plausible, konsistente Werte (SoC, Spannung, Strom, Leistung,
Temperatur, Zellspannungen, geschätzte Restlaufzeit). Das ursprünglich
vermutete JBD-Standardprotokoll war eine Sackgasse; des Rätsels Lösung war,
die Batteriemarke (Redodo) vom Typenschild abzulesen und danach zu suchen —
für diese Marke (gemeinsam mit LiTime/PowerQueen) existiert bereits offen
dokumentierter, funktionierender Code. Unterwegs kamen noch zwei subtile
Bugs dazu: ein BLE-Scanner-Crash durch ein flutendes Nachbargerät (behoben
mit `setDuplicateFilter(true)`) und ein stiller MQTT-Publish-Fehlschlag
durch einen PubSubClient-Puffer, der trotz `#define` bei den
Default-256-Bytes blieb (behoben mit `setBufferSize()`/`setKeepAlive()`
zur Laufzeit).
