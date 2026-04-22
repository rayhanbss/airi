#include <Arduino.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

// Define pins
#define PIN_CLK 13
#define PIN_DAT 14
#define PIN_CE  12

ThreeWire myWire(PIN_DAT, PIN_CLK, PIN_CE);
RtcDS1302<ThreeWire> rtc(myWire);

void setup() {
    Serial.begin(115200);

    rtc.Begin(); // ← capital B, no Wire.begin() needed for DS1302

    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    RtcDateTime adjusted = RtcDateTime(
        compiled.Year(), compiled.Month(), compiled.Day(),
        compiled.Hour(), compiled.Minute(), compiled.Second() + 20
    );


    if (!rtc.IsDateTimeValid()) {
        Serial.println("RTC lost confidence in the DateTime — setting time");
        rtc.SetDateTime(adjusted);
    }

    if (rtc.GetIsWriteProtected()) {
        Serial.println("RTC was write protected, enabling writing now");
        rtc.SetIsWriteProtected(false);
    }

    if (!rtc.GetIsRunning()) {
        Serial.println("RTC was not actively running, starting now");
        rtc.SetIsRunning(true);
    }

    RtcDateTime now = rtc.GetDateTime();
    if (now < adjusted) {
        Serial.println("RTC is older than compile time — updating");
        rtc.SetDateTime(adjusted);
    }
}

void loop() {
    RtcDateTime now = rtc.GetDateTime();

    Serial.print(now.Year(), DEC);
    Serial.print('/');
    Serial.print(now.Month(), DEC);
    Serial.print('/');
    Serial.print(now.Day(), DEC);
    Serial.print(" ");
    Serial.print(now.Hour(), DEC);
    Serial.print(':');
    Serial.print(now.Minute(), DEC);
    Serial.print(':');
    Serial.print(now.Second(), DEC);
    Serial.println();

    delay(1000);
}
