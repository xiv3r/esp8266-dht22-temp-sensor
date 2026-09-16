# Functions
Automatically turn on and off the fan above and below 35°C using esp8266 and dht22 temperature humidity sensor.

# Requirements 
- ESP8266
- DHT22 Temperature Humidity Sensor
- 1 Channel Relay
- F-F Dupont Wires

# Firmware
- https://github.com/xiv3r/esp8266-dht22-temp-sensor/releases/tag/esp8266

# Wiring Diagram
```
DHT 22     |  ESP8266

DHT22 VCC  → 3.3V
DHT22 GND  → GND
DHT22 DATA → D2 (with 10kΩ pull-up to 3.3V)
```
```
RELAY      |  ESP8266

Relay IN   → D5
Relay VCC  → 5V
Relay GND  → GND
```
