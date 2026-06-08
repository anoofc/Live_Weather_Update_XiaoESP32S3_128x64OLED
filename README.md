# ESP32-S3 Riyadh Weather Station

A compact weather dashboard built using the **Seeed Studio XIAO ESP32-S3** and a **128x64 SSD1306 OLED display**.

The device connects to Wi-Fi, fetches live weather and air quality data from Open-Meteo cloud services, and presents the information through a rotating dashboard optimized for small OLED displays.

---

## Features

* Real-time weather data from Riyadh, Saudi Arabia
* Automatic Wi-Fi reconnect
* SSD1306 OLED display (128x64)
* Weather condition icons
* Dashboard and detail screens
* Air Quality Index (AQI)
* UV Index
* Wind Speed and Direction
* Humidity
* Temperature
* Visibility
* Automatic data refresh every 10 minutes

---

## Hardware

### Controller

* Seeed Studio XIAO ESP32-S3

### Display

* SSD1306 OLED
* Resolution: 128 × 64
* Interface: I2C

### Wiring

| OLED | XIAO ESP32-S3 |
| ---- | ------------- |
| VCC  | 3.3V          |
| GND  | GND           |
| SDA  | GPIO5 (D4)    |
| SCL  | GPIO6 (D5)    |

---

## Libraries

### PlatformIO

```ini
lib_deps =
    bblanchon/ArduinoJson
    olikraus/U8g2
```

---

## Data Sources

### Weather Data

Open-Meteo Forecast API

Provides:

* Temperature
* Relative Humidity
* Visibility
* Wind Speed
* Wind Direction
* Weather Code

### Air Quality Data

Open-Meteo Air Quality API

Provides:

* European AQI
* UV Index

---

## Display Layout

### Dashboard Screen

Displays:

* Weather Icon
* Local Time
* Temperature
* Humidity
* AQI
* UV Index
* Wind Speed

### Detail Screens

The display automatically rotates through:

1. Dashboard
2. Temperature
3. Humidity
4. Air Quality
5. Wind Information

---

## Screen Timing

| Screen      | Duration  |
| ----------- | --------- |
| Dashboard   | 3 seconds |
| Temperature | 2 seconds |
| Humidity    | 2 seconds |
| AQI         | 2 seconds |
| Wind        | 2 seconds |

The cycle then repeats.

---

## Weather Icons

Weather conditions are derived from Open-Meteo Weather Codes.

| Code | Condition |
| ---- | --------- |
| 0    | Clear Sky |
| 1-3  | Cloudy    |
| 51+  | Rain      |

Icons are stored in flash memory using PROGMEM bitmaps.

---

## Configuration

Update Wi-Fi credentials inside:

```cpp
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
```

---

## Location

Current configuration:

Riyadh, Saudi Arabia

```cpp
Latitude  = 24.7136
Longitude = 46.6753
```

Change these values if you want weather for another location.

---

## Refresh Intervals

```cpp
FETCH_INTERVAL_MS = 10 minutes
WIFI_RETRY_MS     = 15 seconds
```

---

## Example Serial Output

```text
Connecting to Wi-Fi: EIS
Wi-Fi connected. IP: 192.168.1.100

Refreshing weather data...
HTTP Code: 200
HTTP Code: 200

Data refresh OK
Temp: 35.3 C
Humidity: 12%
AQI: 553
UV: 0.0
```

---

## Future Improvements

* Sunrise / Sunset display
* Daily weather forecast
* Animated weather icons
* NTP time synchronization
* Touch button screen navigation
* Automatic brightness adjustment
* Deep sleep power-saving mode
* Multiple city support

---

## Author

Anoof Chappangathil

+91 8304 853 899

+966 55 752 2561

Built using:

* ESP32-S3
* U8g2 Graphics Library
* ArduinoJson
* Open-Meteo APIs
* PlatformIO
