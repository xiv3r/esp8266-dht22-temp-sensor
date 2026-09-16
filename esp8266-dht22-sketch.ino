#include <DHT.h>

#define DHTPIN D2
#define DHTTYPE DHT22
#define RELAY_PIN D5
#define TEMP_ON 35.0
#define TEMP_OFF 34.0
#define READ_INTERVAL 2000

DHT dht(DHTPIN, DHTTYPE);

bool relayState = false;
unsigned long lastRead = 0;

void setup() {
  pinMode(DHTPIN, INPUT_PULLUP);
  dht.begin();
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
}

void loop() {
  unsigned long now = millis();

  if (now - lastRead < READ_INTERVAL) {
    return;
  }
  lastRead = now;

  float temperature = dht.readTemperature();

  if (isnan(temperature)) {
    digitalWrite(RELAY_PIN, LOW);
    relayState = false;
    return;
  }

  if (!relayState && temperature > TEMP_ON) {
    relayState = true;
    digitalWrite(RELAY_PIN, HIGH);
  } else if (relayState && temperature < TEMP_OFF) {
    relayState = false;
    digitalWrite(RELAY_PIN, LOW);
  }
}
