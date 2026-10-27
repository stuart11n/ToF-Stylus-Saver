# ToF Stylus Saver

A **LOLIN (Wemos) S2 Mini** watches a **VL53L4CD** time-of-flight sensor and cuts a **Shelly** relay when something is close (stylus / obstacle in the beam). Firmware lives in `arduino/vl53l0x_pololu`.

Repo: [stuart11n/ToF-Stylus-Saver](https://github.com/stuart11n/ToF-Stylus-Saver)

## Behavior

- **LED (GPIO15):** on when live range &lt; 40 mm (unfiltered)
- **Shelly:** OFF on filtered obstacle latch, ON when clear
- **Web UI:** `http://<board-ip>/` shows and changes the Shelly host (saved in NVS; default from `SHELLY_HOST`)
- **Serial:** prints `mm=` each sample; `c` / `z` / `1` / `0` as below
- **Pushover:** wired in code, unused on the obstacle path

VL53L4CD is specified down to ~1 mm (VL53L0X typically floors around 30 mm).

### Wiring

| VL53L4CD | S2 Mini |
|----------|---------|
| VIN / VCC | 3V3 |
| GND | GND |
| SDA | GPIO8 |
| SCL | GPIO9 |
| XSHUT | GPIO7 |

### Libraries

- [VL53L4CD](https://github.com/pololu/vl53l4cd-arduino) (Pololu)

Board: **LOLIN S2 Mini** (`esp32:esp32:lolin_s2_mini`), USB CDC on boot enabled (default).

### Build / flash

```bash
arduino-cli compile -b esp32:esp32:lolin_s2_mini arduino/vl53l0x_pololu
arduino-cli upload -p COMxx -b esp32:esp32:lolin_s2_mini arduino/vl53l0x_pololu
arduino-cli monitor -p COMxx -c baudrate=115200
```

If Arduino CDC is already running, upload to the app port (auto 1200-baud reset). Otherwise: hold **0**, tap **RST**, release **0**, then upload to the bootloader port.

### Serial commands

| Key | Action |
|-----|--------|
| `c` | Offset calibrate at 40 mm (hold a white target at 40 mm) |
| `z` | Clear offset |
| `1` / `0` | Shelly relay on / off |

After a full flash erase, run `c` again.

### Robustness

- I2C timeout 50 ms; after a streak of bad ranges the bus is clocked out, XSHUT is pulsed, and the sensor is re-inited
- Shelly HTTP connect/read timeout 800 ms
- WiFi: new STA MAC each boot and per-BSSID mesh join; ranging loop only does a short reconnect, not a full mesh rescan

## `secrets.h` (required, not in git)

```bash
cp arduino/vl53l0x_pololu/secrets.h.example arduino/vl53l0x_pololu/secrets.h
```

| Symbol | Purpose |
|--------|---------|
| `WIFI_SSID` | 2.4 GHz Wi‑Fi SSID |
| `WIFI_PASSWORD` | Wi‑Fi password |
| `PUSHOVER_USER` | Pushover user key ([pushover.net](https://pushover.net)) |
| `PUSHOVER_TOKEN` | Pushover application API token |
| `SHELLY_HOST` | Default Shelly IP (change later in the web UI) |

**Do not commit `secrets.h`.** Only `secrets.h.example` is tracked.

## Other sketches / MicroPython

- `arduino/vl6180x_pololu` — earlier VL6180X test (near-range was unusable)
- `arduino/wifi_diag` — Wi‑Fi scan / join diagnostics
- `src/` — earlier MicroPython experiments (local `secrets.py`; gitignored)
