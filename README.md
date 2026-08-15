# esp32poeLarsBatMon

Battery monitor for the boat "Lars": an ESP32-S3 reads battery data from a
Bluetooth (BLE) BMS and makes it available over the network via PoE Ethernet.

## Status

- [x] Board identified: **Waveshare ESP32-S3-ETH** (ESP32-S3R8, 512KB SRAM,
      16MB Flash, 8MB Octal PSRAM, WiFi/BLE 5, native USB-Serial/JTAG).
      MAC: `xx:xx:xx:xx:xx:xx`. Connected via `/dev/cu.usbmodem101`.
      W5500 Ethernet over SPI: CLK=13 MISO=12 MOSI=11 CS=14 IRQ=10 RST=9.
      PoE via optional 802.3af module on the board.
- [x] Phase 1: BLE scanner (`src/main.cpp`) — builds, flashes, and runs.
      Lists nearby BLE devices with name, RSSI, manufacturer data and
      service UUIDs, to identify the BMS.
- [ ] BMS not yet located in a scan — battery ("12100BNNH19-C01278", SN
      `BDRG12100-BNN-C4H0206H-H19R`, FW 2.0.0) needs to be in BLE range
      (few meters) during the scan. Likely uses the common JBD/Xiaoxiang
      BMS-over-BLE protocol (typical for rebranded Chinese LiFePO4 packs
      with a generic monitoring app) — to be confirmed once found.
- [ ] Phase 2: identify the BMS's BLE advertisement/GATT service and decode
      voltage/current/SoC.
- [x] Phase 3 (plumbing): Ethernet (W5500 over SPI) + MQTT (PubSubClient)
      added. Publishes a retained heartbeat to `esp32poeLarsBatMon/status`
      and raw BLE scan hits (MAC/RSSI/name/manufacturer data, capped at 15
      per scan) as JSON to `esp32poeLarsBatMon/ble/scan`, so the BMS can be
      spotted from the MQTT log once it's in range. Broker:
      `192.168.24.213:1883`, no auth, not yet reachable/tested end-to-end
      (board currently bench-powered over USB only, no PoE link up).
      Still pending: replace the raw-scan dump with actual decoded BMS
      values once Phase 2 is done.
- [x] Found and fixed a firmware crash: a nearby device flooding BLE
      advertisements at very high rate (Apple-continuity-style spam)
      overloaded the scan callback and corrupted serial output. Fixed with
      `scan->setDuplicateFilter(true)` in `setup()`.

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

- ESP32-S3, 8MB embedded PSRAM
- BLE-based BMS (model not yet identified — run the Phase 1 scanner near the
  battery to find it)
- PoE for network connectivity (board TBD)

## Verwendung

```sh
pio run -t upload
pio device monitor
```

Scanner läuft alle paar Sekunden neu und listet BLE-Geräte in der Nähe. Das
BMS am Namen, an der MAC oder an den Manufacturer-Data-Bytes identifizieren.

## Deutsch

Batteriemonitor für das Boot "Lars": ein ESP32-S3 liest Batteriedaten von
einem Bluetooth-(BLE)-BMS aus und stellt sie übers Netzwerk per PoE-Ethernet
bereit. Aktuell läuft Phase 1 (BLE-Scanner), um das genaue BMS-Modell und
sein Advertisement-Format zu bestimmen — danach folgen Dekodierung und
Ethernet/PoE-Anbindung.
