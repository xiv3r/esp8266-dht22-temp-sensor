# Functions
Automatically turn on and off the fan above and below 35°C using esp32 and dht22 temperature humidity sensor.

# Requirements 
- ESP32S3
- DHT22 Temperature Humidity Sensor
- 1 Channel Relay
- F-F Dupont Wires

# Firmware
> flash the firmware to flash offset address `0x0`
- https://github.com/xiv3r/esp8266-dht22-temp-sensor/releases/tag/esp32
  
# Wiring Diagram
```
DHT 22     |  ESP32

DHT22 VCC  → 3.3V
DHT22 GND  → GND
DHT22 DATA → GPIO 4 (with 10kΩ pull-up to 3.3V)
```

```
RELAY      |  ESP32

Relay IN   → GPIO 5
Relay VCC  → 5V
Relay GND  → GND
```
