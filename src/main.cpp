#include <Arduino.h>

#if __has_include("plantera_config.h")
#include "plantera_config.h"
#else
#include "plantera_config.example.h"
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Plantera firmware scaffold ready");
}

void loop() {
}
