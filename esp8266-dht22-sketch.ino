#include <DHT.h>

#define DHTPIN D2
#define DHTTYPE DHT22
#define RELAY_PIN D5
#define TEMP_ON 35.0 // ON = 35 Degree Celsius
#define TEMP_OFF 34.0 // OFF = 34 Degree Celsius 

DHT dht(DHTPIN, DHTTYPE);

bool relayState = false;

void setup() {
  dht.begin();
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
}

void loop() {
  float temperature = dht.readTemperature();

  if (isnan(temperature)) {
    digitalWrite(RELAY_PIN, LOW);
    relayState = false;
    delay(2000);
    return;
  }

  if (!relayState && temperature > TEMP_ON) {
    relayState = true;
    digitalWrite(RELAY_PIN, HIGH);
  } else if (relayState && temperature < TEMP_OFF) {
    relayState = false;
    digitalWrite(RELAY_PIN, LOW);
  }

  delay(2000);
}
