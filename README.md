## What It Does
A Wi-Fi access point controller that reads temperature/humidity (DHT22) and switches a relay (D6) automatically or manually through a web interface.

---

## Hardware Wiring

| Component | ESP8266 Pin |
|-----------|-------------|
| DHT22 Data | D2 (GPIO4) |
| Relay IN | D6 (GPIO12) |
| BOOT button | GPIO0 (built-in) |

- DHT22 needs a 10kΩ pull-up resistor between Data and VCC.
- Relay is **active-LOW** (LOW = ON).
- Power: DHT22 and relay from 3.3V (check relay module voltage rating).

---

## First-Time Setup

1. **Flash** the sketch to your ESP8266 using Arduino IDE (select "NodeMCU 1.0" or your board).
2. **Power on** the device.
3. On your phone/PC, connect to Wi-Fi:
   - **SSID:** `ESP8266_DHT22`
   - **Password:** `12345678`
4. Open a browser and go to **http://192.168.4.1** (captive portal usually opens automatically).

---

## Using the Web Interface

### Dashboard (`/`)
- Live temperature, humidity, relay state, sensor health
- Buttons: **Relay ON**, **Relay OFF**, **Auto Mode**
- Auto-refreshes every 3 seconds

### Sensors (`/sensors`)
- Set **Relay Mode**: Off / Temperature / Humidity / Temp + Hum
- Set **thresholds**:
  - Temp ON / Temp OFF (°C)
  - Hum ON / Hum OFF (%)
- Rules are enforced: ON must be above OFF (auto-swapped with minimum gap)
- **Manual override**: Force ON, Force OFF, or Return to Auto
- Settings auto-save after 15 seconds

### WiFi AP (`/wifi`)
- Change **SSID** (1–32 chars)
- Change **password** (8–63 chars)
- Click **Apply & Restart AP** — you will be disconnected; reconnect with new credentials

### Status (`/status`)
- Uptime, free heap, fragmentation, requests, NVS writes, CRC errors
- **Restart** button
- **Factory Reset** button (restores defaults)

---

## Default Values

| Setting | Default |
|---------|---------|
| SSID | ESP8266_DHT22 |
| Password | 12345678 |
| Temp ON / OFF | 30 / 28 °C |
| Hum ON / OFF | 70 / 65 % |
| Relay mode | Temperature |
| AP IP | 192.168.4.1 |

---

## Button & Recovery

- **Hold BOOT button 5 seconds** → factory reset and reboot.
- If config is corrupt (CRC error), defaults load automatically.
- If you forget the password, use the BOOT-button factory reset.

---

## Auto-Control Logic

| Mode | Turns ON when | Turns OFF when |
|------|---------------|----------------|
| Off | never | always |
| Temperature | temp ≥ tempOn | temp ≤ tempOff |
| Humidity | hum ≥ humOn | hum ≤ humOff |
| Temp + Hum | either threshold high | both thresholds low |

- If the sensor fails, auto mode **holds the last state** (safe fallback).
- Manual mode ignores sensor readings until you return to Auto.

---

## Quick Troubleshooting

| Problem | Fix |
|---------|-----|
| Can't find Wi-Fi | Power-cycle; check antenna/board |
| Sensor shows `--` / ERR | Check D2 wiring and pull-up resistor |
| Relay won't switch | Check relay module power and D6 wiring |
| Settings not saving | Wait 15 s after change, or reboot |
| Forgot password | Hold BOOT 5 s to factory reset |

---

## Tips

- Keep the DHT22 away from the relay to avoid false heat readings.
- The relay is active-LOW — if your module is active-HIGH, change `RELAY_ACTIVE_LOW` to `false` and re-flash.
- Use the **Status** page to check free heap if the web UI becomes slow.
