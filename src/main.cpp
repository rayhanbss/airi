#include <Arduino.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>

// RTC
#define PIN_CLK 13
#define PIN_DAT 12
#define PIN_RST 14

// SOIL
#define PIN_SOIL 34

ThreeWire myWire(PIN_DAT, PIN_CLK, PIN_RST);
RtcDS1302<ThreeWire> rtc(myWire);

void setup() {
    Serial.begin(115200);

    rtc.Begin();

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

    int soilValue = analogRead(PIN_SOIL);
    int soilPercent = 100 - (soilValue * 100 / 4095);


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

    Serial.print(" Soil: ");
    Serial.println(soilPercent);
    Serial.println();

    delay(1000);
}
