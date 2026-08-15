# esp32poeLarsBatMon

Battery monitor for the boat "Lars": an ESP32-S3 reads battery data from a
Bluetooth (BLE) BMS and makes it available over the network via PoE Ethernet
and MQTT.

## Status — working end-to-end

- [x] Board: **Waveshare ESP32-S3-ETH** (ESP32-S3R8, 512KB SRAM, 16MB Flash,
      8MB Octal PSRAM, WiFi/BLE 5, native USB-Serial/JTAG). MAC:
      `xx:xx:xx:xx:xx:xx`, connected via `/dev/cu.usbmodem101`. W5500
      Ethernet over SPI: CLK=13 MISO=12 MOSI=11 CS=14 IRQ=10 RST=9. PoE via
      optional 802.3af module on the board.
- [x] Battery: **Redodo Power** 12.8V/100Ah LiFePO4, model RH190, alias
      "R-12100BNNH19-C01278", MAC `xx:xx:xx:xx:xx:xx`. BLE GATT service
      `0xFFE0`, notify on `0xFFE1`, write on `0xFFE2`.
- [x] Ethernet (W5500) + MQTT (PubSubClient) working end-to-end. Retained
      heartbeat on `LiFePo01/status` (IP, uptime, free heap,
      `bms_connected`). Broker `192.168.24.213:1883` (the Cerbo/Venus GX's
      own MQTT broker), no auth.
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

## Hardware

- Waveshare ESP32-S3-ETH (W5500 PoE Ethernet)
- Battery: Redodo Power 12.8V/100Ah LiFePO4 (model RH190), MAC
  `xx:xx:xx:xx:xx:xx`
- MQTT broker at `192.168.24.213:1883`

## Verwendung

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
aus und stellt sie übers Netzwerk per PoE-Ethernet via MQTT bereit. Läuft
End-to-End: Ethernet, MQTT und das BMS-Protokoll sind alle funktionsfähig
und liefern live plausible Werte. Das ursprünglich vermutete
JBD-Standardprotokoll war eine Sackgasse; des Rätsels Lösung war, die
Batteriemarke (Redodo) vom Typenschild abzulesen und danach zu suchen —
für diese Marke (gemeinsam mit LiTime/PowerQueen) existiert bereits offen
dokumentierter, funktionierender Code.
