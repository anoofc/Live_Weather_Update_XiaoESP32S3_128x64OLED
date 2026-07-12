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

//------------------------------
// Weather Icons 
// -----------------------------

const unsigned char icon_sun[] PROGMEM = {
  0x80,0x01,
  0x44,0x22,
  0x28,0x14,
  0x10,0x08,
  0x1C,0x38,
  0x1C,0x38,
  0x10,0x08,
  0x28,0x14,
  0x44,0x22,
  0x80,0x01,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};


const unsigned char icon_cloud[] PROGMEM = {
  0x00,0x00,
  0x00,0x00,
  0x1C,0x00,
  0x22,0x00,
  0x41,0x00,
  0x80,0x01,
  0x80,0x01,
  0xC0,0x03,
  0x7F,0xFE,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};

const unsigned char icon_rain[] PROGMEM = {
  0x1C,0x00,
  0x22,0x00,
  0x41,0x00,
  0x80,0x01,
  0xFF,0x01,
  0x00,0x00,
  0x20,0x00,
  0x00,0x01,
  0x40,0x00,
  0x00,0x02,
  0x20,0x00,
  0x00,0x01,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00,
  0x00,0x00
};



// Open-Meteo endpoints
// Weather Forecast API: current weather
// Air Quality API: current European AQI + UV Index
const char* WEATHER_URL =
"https://api.open-meteo.com/v1/forecast"
"?latitude=24.7136"
"&longitude=46.6753"
"&current=temperature_2m,relative_humidity_2m,"
"visibility,wind_speed_10m,wind_direction_10m,"
"weather_code"
"&timezone=Asia%2FRiyadh";

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
static constexpr unsigned long FETCH_INTERVAL_MS  = 2UL *60UL * 1000UL; // refresh every 2 min
static constexpr unsigned long WIFI_RETRY_MS      = 5000UL;    // retry Wi-Fi every 15 sec

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

  int weatherCode = 0;        // Open-Meteo weather code (https://open-meteo.com/en/docs#api_form)
};

WeatherData gData;

const uint8_t* getWeatherIcon()
{
    if(gData.weatherCode == 0)
        return icon_sun;

    if(gData.weatherCode >= 1 && gData.weatherCode <= 3)
        return icon_cloud;

    if(gData.weatherCode >= 51)
        return icon_rain;

    return icon_cloud;
}




enum ScreenType
{
    SCREEN_DASHBOARD,
    SCREEN_TEMP,
    SCREEN_HUMIDITY,
    SCREEN_AQI,
    SCREEN_WIND
};

ScreenType currentScreen = SCREEN_DASHBOARD;
unsigned long screenTimer = 0;

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
  JsonDocument doc;
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
  gData.weatherCode = current["weather_code"] | 0;

  return true;
}

static bool fetchAirData() {
  JsonDocument doc;
  if (!httpGetJson(AIR_URL, doc)) return false;

  JsonObject current = doc["current"].as<JsonObject>();
  if (current.isNull()) return false;

  gData.aqi     = current["european_aqi"] | NAN;
  gData.uvIndex = current["uv_index"] | NAN;

  return true;
}

static void drawWiFiConnecting(uint8_t dotCount) {
  u8g2.clearBuffer();

  centerText(18, "WiFi Connecting", u8g2_font_6x12_tf);
  centerText(38, String("SSID: ") + WIFI_SSID, u8g2_font_6x12_tf);

  String dots;
  for (uint8_t i = 0; i < dotCount; i++) {
    dots += ".";
  }
  centerText(58, dots, u8g2_font_6x12_tf);

  u8g2.sendBuffer();
}

static void drawWiFiConnected() {
  u8g2.clearBuffer();

  centerText(20, "WiFi Connected", u8g2_font_6x12_tf);
  centerText(42, "IP Address", u8g2_font_6x12_tf);
  centerText(60, WiFi.localIP().toString(), u8g2_font_6x12_tf);

  u8g2.sendBuffer();
}

static void drawWiFiReconnecting() {
  u8g2.clearBuffer();

  centerText(22, "WiFi Unavailable", u8g2_font_6x12_tf);
  centerText(42, "Reconnecting...", u8g2_font_6x12_tf);
  centerText(60, String("SSID: ") + WIFI_SSID, u8g2_font_6x12_tf);

  u8g2.sendBuffer();
}

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("Connecting to Wi-Fi: %s", WIFI_SSID);
  drawWiFiConnecting(1);

  unsigned long start = millis();
  uint8_t dotCount = 1;
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 20000UL) {
    delay(250);
    Serial.print(".");

    dotCount = (dotCount % 3) + 1;
    drawWiFiConnecting(dotCount);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    drawWiFiConnected();
    delay(2000);
  } else {
    Serial.println("Wi-Fi connect timeout");
    drawWiFiReconnecting();
  }
}

static void drawStatusLine(const String& text) {
  u8g2.setFont(u8g2_font_6x12_tf);
  centerText(62, text, u8g2_font_6x12_tf);
}

// DASHBOARD page rendering

void drawDashboard()
{
    u8g2.clearBuffer();

    u8g2.drawXBMP(
        0,
        0,
        16,
        16,
        getWeatherIcon()
    );

    u8g2.setFont(u8g2_font_6x12_tf);

    u8g2.drawStr(22, 12, "RIYADH");

    u8g2.drawStr(
        82,
        12,
        gData.localTime.c_str()
    );

    u8g2.setFont(u8g2_font_logisoso18_tf);

    String temp =
        String(gData.temperature,1) + "C";

    u8g2.drawStr(0, 38, temp.c_str());

    u8g2.setFont(u8g2_font_6x12_tf);

    String hum =
        "HUM " +
        String((int)gData.humidity) + "%";

    u8g2.drawStr(80, 28, hum.c_str());

    String aqi =
        "AQI " +
        String((int)gData.aqi);

    u8g2.drawStr(80, 42, aqi.c_str());

    String uv =
        "UV " +
        String(gData.uvIndex,1);

    u8g2.drawStr(80, 56, uv.c_str());

    String wind =
        String(gData.windSpeed,1) + "km";

    u8g2.drawStr(0, 56, wind.c_str());

    u8g2.sendBuffer();
}

// TEMPERATURE page rendering

void drawTemperature()
{
    u8g2.clearBuffer();

    u8g2.drawXBMP(
        56,
        0,
        16,
        16,
        getWeatherIcon()
    );

    u8g2.setFont(u8g2_font_logisoso32_tf);

    String temp =
        String(gData.temperature,1);

    u8g2.drawStr(10, 50, temp.c_str());

    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(100, 48, "C");
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(35, 63, "TEMPERATURE");

    u8g2.sendBuffer();
}


// HUMIDITY page rendering

void drawHumidity()
{
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_logisoso32_tf);

    String hum =
        String((int)gData.humidity);

    u8g2.drawStr(30, 50, hum.c_str());

    
    u8g2.setFont(u8g2_font_7x14_tf);
    u8g2.drawStr(90, 48, "%");
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(40, 63, "HUMIDITY");

    u8g2.sendBuffer();
}


// AIR QUALITY page rendering

void drawAQI()
{
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_logisoso24_tf);

    String aqi =
        String((int)gData.aqi);

    u8g2.drawStr(35, 40, aqi.c_str());

    u8g2.setFont(u8g2_font_6x12_tf);

    if(gData.aqi > 100)
        u8g2.drawStr(25, 60, "EXTREME");
    else if(gData.aqi > 80)
        u8g2.drawStr(20, 60, "VERY POOR");
    else if(gData.aqi > 60)
        u8g2.drawStr(30, 60, "POOR");
    else if(gData.aqi > 40)
        u8g2.drawStr(20, 60, "MODERATE");
    else
        u8g2.drawStr(35, 60, "GOOD");

    u8g2.sendBuffer();
}

//

void drawWind()
{
    u8g2.clearBuffer();

    u8g2.setFont(u8g2_font_logisoso24_tf);

    String speed =
        String(gData.windSpeed,1);

    u8g2.drawStr(35, 36, speed.c_str());

    
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(85, 32, "km/h");
    u8g2.setFont(u8g2_font_6x12_tf);
    String dir = "--";

    int d = (int)gData.windDir;

    if(d >= 337 || d < 22)
        dir = "N";
    else if(d < 67)
        dir = "NE";
    else if(d < 112)
        dir = "E";
    else if(d < 157)
        dir = "SE";
    else if(d < 202)
        dir = "S";
    else if(d < 247)
        dir = "SW";
    else if(d < 292)
        dir = "W";
    else
        dir = "NW";

    u8g2.setFont(u8g2_font_logisoso18_tf);
    u8g2.drawStr(40, 62, dir.c_str());

    u8g2.sendBuffer();
}

void drawCurrentScreen()
{
    switch(currentScreen)
    {
        case SCREEN_DASHBOARD:
            drawDashboard();
            break;

        case SCREEN_TEMP:
            drawTemperature();
            break;

        case SCREEN_HUMIDITY:
            drawHumidity();
            break;

        case SCREEN_AQI:
            drawAQI();
            break;

        case SCREEN_WIND:
            drawWind();
            break;
    }
}


// Screen rotation logic: automatically switch between different screens every few seconds

void updateScreenRotation()
{
    unsigned long now = millis();

    static unsigned long lastSwitch = 0;

    unsigned long duration =
        (currentScreen == SCREEN_DASHBOARD)
        ? 3000
        : 2000;

    if(now - lastSwitch < duration)
        return;

    lastSwitch = now;

    switch(currentScreen)
    {
        case SCREEN_DASHBOARD:
            currentScreen = SCREEN_TEMP;
            break;

        case SCREEN_TEMP:
            currentScreen = SCREEN_HUMIDITY;
            break;

        case SCREEN_HUMIDITY:
            currentScreen = SCREEN_AQI;
            break;

        case SCREEN_AQI:
            currentScreen = SCREEN_WIND;
            break;

        default:
            currentScreen = SCREEN_DASHBOARD;
            break;
    }

    drawCurrentScreen();
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

  currentScreen = SCREEN_DASHBOARD;
  screenTimer = millis();

  if (WiFi.status() == WL_CONNECTED) {
    drawCurrentScreen();
  } else {
    lastWiFiRetryMs = millis();
    drawWiFiReconnecting();
  }
}


void loop()
{
    const unsigned long now = millis();

    // Reconnect WiFi if needed
    if (WiFi.status() != WL_CONNECTED &&
        (now - lastWiFiRetryMs) >= WIFI_RETRY_MS)
    {
        connectWiFi();

        // Start the retry delay after the blocking connection attempt ends so
        // the reconnecting message remains readable between attempts.
        lastWiFiRetryMs = millis();

        if (WiFi.status() == WL_CONNECTED)
        {
            refreshAllData();
            lastFetchMs = now;

            drawCurrentScreen();
        }
        else
        {
            drawWiFiReconnecting();
        }
    }

    // Keep the Wi-Fi status UI visible while offline. This prevents the
    // dashboard and rotating weather pages from replacing it between retries.
    if (WiFi.status() != WL_CONNECTED)
    {
        delay(5);
        return;
    }

    // Periodic weather refresh
    if (WiFi.status() == WL_CONNECTED &&
        (now - lastFetchMs) >= FETCH_INTERVAL_MS)
    {
        lastFetchMs = now;

        refreshAllData();

        drawCurrentScreen();
    }

    // Rotate between dashboard/detail screens
    updateScreenRotation();

    delay(5);
}
