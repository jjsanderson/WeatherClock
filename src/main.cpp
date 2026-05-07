#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <FeedBackServo.h>
#include <ArduinoJson.h>
#include "config.h"  // WiFi credentials, API key, location – see config.h.template

// How often to refresh weather data
static const unsigned long WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 min

// ---------------------------------------------------------------------------
// Pin assignments (Wemos D1 Mini)
//
// SERVO_SIGNAL_PIN  – control PWM output to the servo's white/signal wire.
// SERVO_FEEDBACK_PIN – digital input from the servo's separate feedback wire.
//                      Must be an interrupt-capable pin; all ESP8266 GPIO qualify.
// ---------------------------------------------------------------------------
static const int SERVO_SIGNAL_PIN   = D2;  // GPIO4  – control signal
static const int SERVO_FEEDBACK_PIN = D1;  // GPIO5  – feedback PWM in

// Proportional gain for the FeedBackServo PID controller.
// Increase to move faster/more aggressively; decrease if the servo oscillates.
static const float SERVO_KP = 1.0f;

// Dead-band threshold (degrees) passed to servo.update().
// The controller stops driving the motor once |actual - target| <= this value.
static const int MOVE_TOLERANCE_DEG = 2;

// ---------------------------------------------------------------------------
// Weather condition → servo angle mapping
//
// OWM condition codes: https://openweathermap.org/weather-conditions
// Adjust the angles (0–180°) to suit the layout of your physical indicator.
// ---------------------------------------------------------------------------
struct WeatherZone {
    int         conditionMin;
    int         conditionMax;
    int         angle;
    const char* label;
};

static const WeatherZone WEATHER_ZONES[] = {
    { 200, 299, 165, "Thunderstorm" },
    { 300, 399, 120, "Drizzle"      },
    { 500, 599, 135, "Rain"         },
    { 600, 699,  90, "Snow"         },
    { 700, 799,  60, "Atmosphere"   },  // fog, mist, haze, dust, etc.
    { 800, 800,   0, "Clear"        },
    { 801, 804,  30, "Cloudy"       },
};
static const int NUM_ZONES = sizeof(WEATHER_ZONES) / sizeof(WEATHER_ZONES[0]);

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

// The FeedBackServo constructor takes the feedback pin and sets up an
// interrupt to measure the PWM duty cycle from the servo's Hall effect sensor.
FeedBackServo weatherServo(SERVO_FEEDBACK_PIN);

int currentTargetAngle = -1;  // -1 = not yet set

// ---------------------------------------------------------------------------
// Servo helpers
// ---------------------------------------------------------------------------

// Set a new target angle. The proportional controller in weatherServo.update()
// will drive the motor towards it on every subsequent loop() iteration.
// This is intentionally non-blocking.
void moveServoTo(int targetAngle) {
    Serial.printf("Servo: target → %d° (currently at %d°)\n",
                  targetAngle, weatherServo.getAngle());
    currentTargetAngle = targetAngle;
    weatherServo.setTarget(targetAngle);
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
void connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;
    Serial.printf("Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 20000UL) {
            Serial.println("\nWiFi connect timeout");
            return;
        }
        delay(500);
        Serial.print('.');
    }
    Serial.printf("\nConnected – IP %s\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
// OpenWeatherMap fetch
//
// CURRENT weather is active by default.
// To switch to the near-future FORECAST instead:
//   1. Comment out the "CURRENT" url assignment below.
//   2. Uncomment the "FORECAST" url assignment.
//   3. Swap the two filter / doc extraction blocks as indicated.
// ---------------------------------------------------------------------------
int fetchWeatherConditionCode() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("OWM: WiFi not connected – skipping fetch");
        return -1;
    }

    // -- CURRENT weather endpoint --
    String url = String("https://api.openweathermap.org/data/2.5/weather"
                        "?q=") + OWM_CITY + "," + OWM_COUNTRY_CODE +
                 "&appid=" + OWM_API_KEY +
                 "&units=" + OWM_UNITS;

    /* -- FORECAST endpoint (nearest 3-hour slot) – uncomment to use instead --
    String url = String("https://api.openweathermap.org/data/2.5/forecast"
                        "?q=") + OWM_CITY + "," + OWM_COUNTRY_CODE +
                 "&cnt=2"    // 2 slots covers ~now and +3 h
                 "&appid=" + OWM_API_KEY +
                 "&units=" + OWM_UNITS;
    */

    Serial.printf("OWM: fetching %s,%s ...\n", OWM_CITY, OWM_COUNTRY_CODE);

    // NOTE: setInsecure() skips TLS certificate validation, which is
    // acceptable for a home device on a trusted network.  For a production
    // device, provide a trusted CA bundle or pin the server certificate.
    BearSSL::WiFiClientSecure client;
    client.setInsecure();

    HTTPClient https;
    https.begin(client, url);
    https.setTimeout(8000);

    Serial.println("OWM: sending GET...");
    int httpCode = https.GET();
    Serial.printf("OWM: HTTP response %d\n", httpCode);

    if (httpCode != HTTP_CODE_OK) {
        // Print the response body on failure – OWM returns a JSON error message
        // (e.g. "city not found", "invalid API key") that is very helpful here.
        String body = https.getString();
        Serial.printf("OWM: error body: %s\n", body.c_str());
        https.end();
        return -1;
    }

    // Filter document: only deserialise the fields we actually need.
    // This dramatically reduces RAM usage on the ESP8266.
    JsonDocument filter;
    filter["weather"][0]["id"] = true;           // CURRENT path
    /* filter["list"][0]["weather"][0]["id"] = true;  // FORECAST path */

    Serial.println("OWM: parsing JSON...");
    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, https.getStream(), DeserializationOption::Filter(filter));
    https.end();

    if (err) {
        Serial.printf("OWM: JSON parse error: %s\n", err.c_str());
        return -1;
    }

    int conditionCode = doc["weather"][0]["id"] | -1;           // CURRENT path
    /* int conditionCode = doc["list"][0]["weather"][0]["id"] | -1;  // FORECAST path */

    if (conditionCode < 0) {
        Serial.println("OWM: condition code missing in response (check city/key?)");
    } else {
        Serial.printf("OWM: condition code %d\n", conditionCode);
    }
    return conditionCode;
}

// ---------------------------------------------------------------------------
// Map OWM condition code to servo angle
// ---------------------------------------------------------------------------
int conditionCodeToAngle(int code) {
    if (code < 0) return -1;
    for (int i = 0; i < NUM_ZONES; i++) {
        if (code >= WEATHER_ZONES[i].conditionMin &&
            code <= WEATHER_ZONES[i].conditionMax) {
            Serial.printf("Condition: %s → %d°\n",
                          WEATHER_ZONES[i].label, WEATHER_ZONES[i].angle);
            return WEATHER_ZONES[i].angle;
        }
    }
    Serial.printf("Unknown code %d – defaulting to Clear\n", code);
    return 0;
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n\n--- WeatherClock starting ---");

    // Attach control signal pin and configure the proportional gain.
    weatherServo.setServoControl(SERVO_SIGNAL_PIN);
    weatherServo.setKp(SERVO_KP);

    // The Hall effect sensor is always powered and always tracking position.
    // A single update() call is enough to populate getAngle() before we read it.
    weatherServo.update(MOVE_TOLERANCE_DEG);
    Serial.printf("Startup position: %d°\n", weatherServo.getAngle());

    connectWiFi();

    // Fetch immediately on boot so the indicator is correct straight away.
    int code  = fetchWeatherConditionCode();
    int angle = conditionCodeToAngle(code);
    if (angle >= 0) {
        moveServoTo(angle);
    }
}

void loop() {
    static unsigned long lastFetch = 0;

    // Must be called every iteration – this is the proportional controller step
    // that drives the servo motor towards currentTargetAngle.
    weatherServo.update(MOVE_TOLERANCE_DEG);

    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
    }

    unsigned long now = millis();
    if (now - lastFetch >= WEATHER_INTERVAL_MS) {
        lastFetch = now;
        int code  = fetchWeatherConditionCode();
        int angle = conditionCodeToAngle(code);
        // Only move if we got a valid result and the position has actually changed.
        if (angle >= 0 && angle != currentTargetAngle) {
            moveServoTo(angle);
        }
    }
}
