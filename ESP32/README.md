# Functions
Automatically turn on and off the fan above and below 35°C using esp8266 and dht22 temperature humidity sensor.

# Requirements 
- ESP32
- DHT22 Temperature Humidity Sensor
- 1 Channel Relay
- F-F Dupont Wires

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
