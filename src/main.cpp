#define DEBUG 0

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ArduinoJson.h>

// -----------------------------
// User configuration
// -----------------------------
const char* WIFI_SSID = "EIS";
const char* WIFI_PASSWORD = "12341234";

// Riyadh, Saudi Arabia
static constexpr float LATITUDE  = 24.7136f;
static constexpr float LONGITUDE = 46.6753f;

// Open-Meteo endpoints
// Weather Forecast API: current weather
// Air Quality API: current European AQI + UV Index
static const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=24.7136"
  "&longitude=46.6753"
  "&current=temperature_2m,relative_humidity_2m,visibility,wind_speed_10m,wind_direction_10m"
  "&timezone=Asia%2FRiyadh"
  "&forecast_days=1"
  "&wind_speed_unit=kmh"
  "&temperature_unit=celsius"
  "&timeformat=iso8601";

static const char* AIR_URL =
  "https://air-quality-api.open-meteo.com/v1/air-quality"
  "?latitude=24.7136"
  "&longitude=46.6753"
  "&current=european_aqi,uv_index"
  "&timezone=Asia%2FRiyadh"
  "&timeformat=iso8601";

// -----------------------------
// OLED setup
// XIAO ESP32-S3 I2C pins:
// D4 = SDA = GPIO5
// D5 = SCL = GPIO6
// -----------------------------
static constexpr uint8_t SDA_PIN = 5;
static constexpr uint8_t SCL_PIN = 6;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// -----------------------------
// Timing
// -----------------------------
static constexpr unsigned long PAGE_INTERVAL_MS  = 2000UL;     // 2 sec per screen
static constexpr unsigned long FETCH_INTERVAL_MS  = 10UL * 60UL * 1000UL; // refresh every 10 min
static constexpr unsigned long WIFI_RETRY_MS      = 15000UL;    // retry Wi-Fi every 15 sec

// -----------------------------
// Data model
// -----------------------------
struct WeatherData {
  bool valid = false;

  String localDate;   // YYYY-MM-DD
  String localTime;   // HH:MM

  float temperature = NAN;    // C
  float humidity    = NAN;    // %
  float visibility  = NAN;    // meters
  float windSpeed   = NAN;    // km/h
  float windDir     = NAN;    // degrees

  float aqi         = NAN;    // European AQI
  float uvIndex     = NAN;    // UV index
};

WeatherData gData;

enum PageIndex : uint8_t {
  PAGE_TIME = 0,
  PAGE_TEMP,
  PAGE_HUMIDITY,
  PAGE_AIR,
  PAGE_UV,
  PAGE_VISIBILITY,
  PAGE_WIND,
  PAGE_COUNT
};

uint8_t currentPage = 0;
unsigned long lastPageChangeMs = 0;
unsigned long lastFetchMs = 0;
unsigned long lastWiFiRetryMs = 0;

// -----------------------------
// Helpers
// -----------------------------
static String safeFloat(float value, int digits = 1) {
  if (isnan(value)) return "--";
  return String(value, digits);
}

static String formatKm(float meters) {
  if (isnan(meters)) return "--";
  return String(meters / 1000.0f, 1) + " km";
}

static String formatWind(float speed, float directionDeg) {
  if (isnan(speed) && isnan(directionDeg)) return "--";
  String dir = "--";
  if (!isnan(directionDeg)) {
    int d = (int)roundf(directionDeg) % 360;
    if (d < 0) d += 360;

    // 16-point compass
    static const char* compass[] = {
      "N","NNE","NE","ENE","E","ESE","SE","SSE",
      "S","SSW","SW","WSW","W","WNW","NW","NNW"
    };
    dir = String(compass[(d + 11) / 22 % 16]) + " " + String(d) + " deg";
  }

  String spd = isnan(speed) ? String("--") : String(speed, 1) + " km/h";
  return spd + " " + dir;
}

static String aqiCategory(float aqi) {
  if (isnan(aqi)) return "--";
  if (aqi <= 20.0f) return "GOOD";
  if (aqi <= 40.0f) return "FAIR";
  if (aqi <= 60.0f) return "MODERATE";
  if (aqi <= 80.0f) return "POOR";
  if (aqi <= 100.0f) return "VERY POOR";
  return "EXTREME";
}

static void centerText(int y, const String& text, const uint8_t* font) {
  u8g2.setFont(font);
  int16_t w = u8g2.getStrWidth(text.c_str());
  int16_t x = (128 - w) / 2;
  if (x < 0) x = 0;
  u8g2.drawStr(x, y, text.c_str());
}

// static bool httpGetJson(const char* url, DynamicJsonDocument& doc) {
//   WiFiClientSecure client;
//   client.setInsecure(); // simplest HTTPS path for ESP32

//   HTTPClient http;
//   if (!http.begin(client, url)) {
//     Serial.println("HTTP begin failed");
//     return false;
//   }

//   int code = http.GET();
//   if (code != HTTP_CODE_OK) {
//     Serial.printf("HTTP GET failed: %d\n", code);
//     http.end();
//     return false;
//   }


//   DeserializationError err = deserializeJson(doc, http.getStream());
//   http.end();

//   if (err) {
//     Serial.print("JSON parse failed: ");
//     Serial.println(err.c_str());
//     return false;
//   }

//   return true;
// }

static bool httpGetJson(const char* url, JsonDocument& doc)
{
    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;

    if (!http.begin(client, url))
    {
        Serial.println("HTTP begin failed");
        return false;
    }

    int httpCode = http.GET();

    Serial.printf("HTTP Code: %d\n", httpCode);

    if (httpCode <= 0)
    {
        Serial.printf("HTTP Error: %s\n",
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    if (DEBUG){
      Serial.println("============== RESPONSE ==============");
      Serial.println(payload);
      Serial.println("======================================");
    }
    

    DeserializationError err = deserializeJson(doc, payload);

    if (err)
    {
        Serial.print("JSON Error: ");
        Serial.println(err.c_str());
        http.end();
        return false;
    }

    http.end();
    return true;
}

static bool parseDateTime(const String& iso, String& dateOut, String& timeOut) {
  int t = iso.indexOf('T');
  if (t < 0) return false;

  dateOut = iso.substring(0, t);
  timeOut = iso.substring(t + 1, t + 6); // HH:MM
  return true;
}

static bool fetchWeatherData() {
  DynamicJsonDocument doc(8192);
  if (!httpGetJson(WEATHER_URL, doc)) return false;

  JsonObject current = doc["current"].as<JsonObject>();
  if (current.isNull()) return false;

  String isoTime = current["time"] | "";
  String dateStr, timeStr;
  if (!parseDateTime(isoTime, dateStr, timeStr)) {
    dateStr = "--";
    timeStr = "--";
  }

  gData.localDate = dateStr;
  gData.localTime = timeStr;
  gData.temperature = current["temperature_2m"] | NAN;
  gData.humidity    = current["relative_humidity_2m"] | NAN;
  gData.visibility  = current["visibility"] | NAN;
  gData.windSpeed   = current["wind_speed_10m"] | NAN;
  gData.windDir     = current["wind_direction_10m"] | NAN;

  return true;
}

static bool fetchAirData() {
  DynamicJsonDocument doc(8192);
  if (!httpGetJson(AIR_URL, doc)) return false;

  JsonObject current = doc["current"].as<JsonObject>();
  if (current.isNull()) return false;

  gData.aqi     = current["european_aqi"] | NAN;
  gData.uvIndex = current["uv_index"] | NAN;

  return true;
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000UL) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi connect timeout");
  }
}

static void drawStatusLine(const String& text) {
  u8g2.setFont(u8g2_font_6x12_tf);
  centerText(62, text, u8g2_font_6x12_tf);
}

static void renderPage(uint8_t page) {
  u8g2.clearBuffer();

  switch (page) {
    case PAGE_TIME: {
      centerText(14, "RIYADH", u8g2_font_6x12_tf);
      centerText(34, gData.localDate.length() ? gData.localDate : String("--"), u8g2_font_6x12_tf);
      centerText(54, gData.localTime.length() ? gData.localTime : String("--"), u8g2_font_6x12_tf);
      break;
    }

    case PAGE_TEMP: {
      centerText(14, "TEMPERATURE", u8g2_font_6x12_tf);
      String v = safeFloat(gData.temperature, 1) + " C";
      centerText(40, v, u8g2_font_6x12_tf);
      break;
    }

    case PAGE_HUMIDITY: {
      centerText(14, "HUMIDITY", u8g2_font_6x12_tf);
      String v = safeFloat(gData.humidity, 0) + " %";
      centerText(40, v, u8g2_font_6x12_tf);
      break;
    }

    case PAGE_AIR: {
      centerText(14, "AIR QUALITY", u8g2_font_6x12_tf);
      String v1 = "EAQI " + safeFloat(gData.aqi, 0);
      String v2 = aqiCategory(gData.aqi);
      centerText(36, v1, u8g2_font_6x12_tf);
      centerText(52, v2, u8g2_font_6x12_tf);
      break;
    }

    case PAGE_UV: {
      centerText(14, "UV INDEX", u8g2_font_6x12_tf);
      String v = safeFloat(gData.uvIndex, 1);
      centerText(40, v, u8g2_font_6x12_tf);
      break;
    }

    case PAGE_VISIBILITY: {
      centerText(14, "VISIBILITY", u8g2_font_6x12_tf);
      String v = formatKm(gData.visibility);
      centerText(40, v, u8g2_font_6x12_tf);
      break;
    }

    case PAGE_WIND: {
      centerText(14, "WIND", u8g2_font_6x12_tf);
      String v1 = isnan(gData.windSpeed) ? String("--") : String(gData.windSpeed, 1) + " km/h";
      String v2;
      if (isnan(gData.windDir)) {
        v2 = "--";
      } else {
        int d = (int)roundf(gData.windDir) % 360;
        if (d < 0) d += 360;
        static const char* compass[] = {
          "N","NNE","NE","ENE","E","ESE","SE","SSE",
          "S","SSW","SW","WSW","W","WNW","NW","NNW"
        };
        v2 = String(compass[(d + 11) / 22 % 16]) + " " + String(d) + " deg";
      }

      centerText(34, v1, u8g2_font_6x12_tf);
      centerText(50, v2, u8g2_font_6x12_tf);
      break;
    }
  }

  if (!gData.valid) {
    drawStatusLine("Waiting for data...");
  } else if (WiFi.status() != WL_CONNECTED) {
    drawStatusLine("Wi-Fi disconnected");
  }

  u8g2.sendBuffer();
}

static void refreshAllData() {
  Serial.println("Refreshing weather data...");
  bool w = fetchWeatherData();
  bool a = fetchAirData();
  gData.valid = (w && a);

  if (gData.valid) {
    Serial.println("Data refresh OK");
    Serial.printf("Temp: %.1f C, Humidity: %.0f%%, AQI: %.0f, UV: %.1f\n",
                  gData.temperature, gData.humidity, gData.aqi, gData.uvIndex);
  } else {
    Serial.println("Data refresh failed");
  }
}

// -----------------------------
// Arduino
// -----------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  u8g2.begin();
  u8g2.setContrast(255);

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    refreshAllData();
    lastFetchMs = millis();
  }

  currentPage = 0;
  lastPageChangeMs = millis();
  renderPage(currentPage);
}

void loop() {
  const unsigned long now = millis();

  // Keep Wi-Fi alive
  if (WiFi.status() != WL_CONNECTED && (now - lastWiFiRetryMs) >= WIFI_RETRY_MS) {
    lastWiFiRetryMs = now;
    connectWiFi();

    if (WiFi.status() == WL_CONNECTED && !gData.valid) {
      refreshAllData();
      lastFetchMs = now;
      renderPage(currentPage);
    }
  }

  // Refresh cloud data periodically
  if (WiFi.status() == WL_CONNECTED && (now - lastFetchMs) >= FETCH_INTERVAL_MS) {
    lastFetchMs = now;
    refreshAllData();
    renderPage(currentPage);
  }

  // Cycle pages every 2 seconds
  if ((now - lastPageChangeMs) >= PAGE_INTERVAL_MS) {
    lastPageChangeMs = now;
    currentPage = (currentPage + 1) % PAGE_COUNT;
    renderPage(currentPage);
  }

  delay(5);
}