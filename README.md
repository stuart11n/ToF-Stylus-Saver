# LOLIN S2 Mini — VL53L4CD proximity + Shelly

Arduino firmware for a **LOLIN (Wemos) S2 Mini** with a **VL53L4CD** time-of-flight sensor.

## Behavior (`arduino/vl53l0x_pololu`)

- **LED (GPIO15):** on when distance &lt; 40 mm (unfiltered / live)
- **Shelly relay:** OFF on filtered obstacle latch, ON when clear (hysteresis + debounce)
- **Web UI:** `http://<board-ip>/` shows Shelly host and lets you change it (saved in NVS; defaults from `SHELLY_HOST`)
- **Pushover:** optional (wired in code; currently unused on the obstacle path)
- **Offset calibration:** stored in NVS (`c` at 40 mm, `z` to clear)

### Wiring

| VL53L4CD | S2 Mini |
|---------|---------|
| VIN / VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |
| XSHUT | GPIO7 |

### Libraries

Install via Arduino Library Manager / `arduino-cli`:

- [VL53L4CD](https://github.com/pololu/vl53l4cd-arduino) (Pololu)

Board: **LOLIN S2 Mini** (`esp32:esp32:lolin_s2_mini`), with USB CDC on boot enabled (default).

### Build / flash

```bash
arduino-cli compile -b esp32:esp32:lolin_s2_mini arduino/vl53l0x_pololu
arduino-cli upload -p COMxx -b esp32:esp32:lolin_s2_mini arduino/vl53l0x_pololu
arduino-cli monitor -p COMxx -c baudrate=115200
```

On a board already running Arduino CDC, upload to the app port (auto 1200-baud reset). Otherwise: hold **0**, tap **RST**, release **0**, then upload to the bootloader port.

### Serial commands

| Key | Action |
|-----|--------|
| `c` | Offset calibrate at 40 mm |
| `z` | Clear offset |
| `1` / `0` | Shelly relay on / off |

## `secrets.h` (required, not in git)

Create local credentials from the example:

```bash
cp arduino/vl53l0x_pololu/secrets.h.example arduino/vl53l0x_pololu/secrets.h
```

Edit `arduino/vl53l0x_pololu/secrets.h`:

| Symbol | Purpose |
|--------|---------|
| `WIFI_SSID` | 2.4 GHz Wi‑Fi network name |
| `WIFI_PASSWORD` | Wi‑Fi password |
| `PUSHOVER_USER` | Pushover user key ([pushover.net](https://pushover.net)) |
| `PUSHOVER_TOKEN` | Pushover application API token |
| `SHELLY_HOST` | Default Shelly IP (override anytime via the web UI; Gen1 `/relay/0?turn=` or Gen2 RPC fallback) |

**Do not commit `secrets.h`.** It is listed in `.gitignore`. Only `secrets.h.example` (placeholders) is tracked.

## Other sketches / MicroPython

- `arduino/vl6180x_pololu` — earlier Pololu VL6180X test (that module’s near-range was unusable)
- `arduino/wifi_diag` — Wi‑Fi scan / join diagnostics
- `src/` — earlier MicroPython experiments (use a local `secrets.py`; also gitignored)

## Notes

- VL53L4CD is specified down to ~1 mm (unlike VL53L0X, which floors around 30 mm).
- After a full flash erase, re-run offset calibration (`c`).
