# Functions
Automatically turn on and off the fan above and below 35°C using esp8266 and dht22 temperature humidity sensor.

# Requirements 
- ESP8266
- DHT22 Temperature Humidity Sensor
- 1 Channel Relay
- F-F Dupont Wires

# Wiring Diagram
```
DHT22 VCC  → 3.3V
DHT22 GND  → GND
DHT22 DATA → D2 (with 10kΩ pull-up to 3.3V)

Relay IN   → D5
Relay VCC  → 5V (or 3.3V per module)
Relay GND  → GND
```
