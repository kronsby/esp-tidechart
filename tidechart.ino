#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>       // Hardware-specific library for the TFT display on Lilygo T-Display-S3
#include <time.h>

// Replace with your WiFi network credentials
const char* ssid = "<your_ssid>";
const char* password = "<your_psk>";

// You can find station IDs at: https://tidesandcurrents.noaa.gov/stations.html
const char* stationId = "<your_station_id>";

// Time zone settings for Eastern Standard Time (EST) / Eastern Daylight Time (EDT)
// GMT offset in seconds: -5 hours * 3600 seconds/hour = -18000
const long gmtOffset_sec = -18000;
// Daylight Saving Time offset in seconds: 1 hour * 3600 seconds/hour = 3600
const int daylightOffset_sec = 3600;
const char* ntpServer = "pool.ntp.org"; // NTP server for time synchronization

TFT_eSPI tft = TFT_eSPI(); // Create an instance of the TFT_eSPI display object

DynamicJsonDocument doc(8192);
JsonArray predictions;
float minTide, maxTide;
String highTideEvents[2];
String lowTideEvents[2];
int dataDayOfYear = -1;

// DigiCert Global Root G2 certificate
const char* root_ca = \
"-----BEGIN CERTIFICATE-----\n" \
"Add updated cert here!"
"-----END CERTIFICATE-----\n";

void fetchAndDisplayTides();
bool getTidePredictions();
void processTidePredictions();
void drawTideChart(const struct tm& timeinfo);

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  tft.drawString("Connecting to WiFi...", 10, 10, 2);
  connectToWiFi();
  tft.drawString("WiFi Connected!", 10, 30, 2);

  initTime();
  delay(2000);

  fetchAndDisplayTides();
}

void loop() {
  delay(15 * 60 * 1000);  // every 15 min
  fetchAndDisplayTides();
}

void connectToWiFi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    tft.drawString(".", 10 + (attempts * 10), 10, 2);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
    tft.fillScreen(TFT_RED);
    tft.drawString("WiFi Failed!", 10, 10, 2);
    while (true) delay(1000);
  }
}

void initTime() {
  Serial.print("Synchronizing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("\nFailed to obtain time from NTP server!");
    tft.fillRect(0, 50, tft.width(), 30, TFT_RED);
    tft.drawString("Time Sync Failed!", 10, 50, 2);
    return;
  }
  Serial.println("\nTime synchronized.");
  tft.fillRect(0, 50, tft.width(), 30, TFT_BLACK);
  tft.drawString("Time Synced!", 10, 50, 2);
}

void fetchAndDisplayTides() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Failed to get time!", 10, 10, 2);
    return;
  }

  if (timeinfo.tm_yday != dataDayOfYear) {
    Serial.println("New day detected. Fetching fresh tide data...");
    tft.fillScreen(TFT_BLACK); // Clear screen to show status
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString("Fetching new tide data...", tft.width()/2 - 100, tft.height()/2, 2);

    if (getTidePredictions()) {
      processTidePredictions();
      dataDayOfYear = timeinfo.tm_yday;
      Serial.println("Successfully fetched and processed new data.");
    } else {
      Serial.println("Failed to fetch new tide data. Will retry later.");
      return;
    }
  } else {
    Serial.println("Data for today already cached. Redrawing chart.");
  }

  drawTideChart(timeinfo);
}

/**
 * Fetches and parses tide prediction data from the NOAA API.
 * @return True on success, false on failure.
 */
bool getTidePredictions() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);
  char dateBuffer[9];
  strftime(dateBuffer, sizeof(dateBuffer), "%Y%m%d", &timeinfo);

  String url = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
               "product=predictions&application=NOAA.TidesAndCurrents&station=" +
               String(stationId) + "&begin_date=" + String(dateBuffer) +
               "&end_date=" + String(dateBuffer) +
               "&datum=MLLW&units=english&time_zone=lst&format=json";

  HTTPClient http;
  http.begin(url, root_ca);
  http.setTimeout(10000);
  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    tft.fillScreen(TFT_RED);
    tft.drawString("HTTP Error: " + String(httpCode), 10, 10, 2);
    http.end();
    return false;
  }

  doc.clear(); // Clear previous JSON data
  DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();

  if (error) {
    tft.fillScreen(TFT_RED);
    tft.drawString("JSON Error!", 10, 10, 2);
    tft.drawString(error.c_str(), 10, 30, 2);
    return false;
  }

  predictions = doc["predictions"].as<JsonArray>();
  if (predictions.size() < 3) {
    tft.fillScreen(TFT_RED);
    tft.drawString("Not enough data.", 10, 10, 2);
    return false;
  }
  return true;
}

/**
 * Analyzes the raw prediction data to find min/max and high/low tide events.
 */
void processTidePredictions() {
  minTide = 10.0;
  maxTide = -10.0;
  highTideEvents[0] = ""; highTideEvents[1] = "";
  lowTideEvents[0] = ""; lowTideEvents[1] = "";
  int highTideCount = 0, lowTideCount = 0;

  for (JsonObject p : predictions) {
    float tideHeight = p["v"].as<float>();
    if (tideHeight < minTide) minTide = tideHeight;
    if (tideHeight > maxTide) maxTide = tideHeight;
  }

  int lastTrend = 0;
  for (int i = 1; i < predictions.size(); i++) {
    float prevHeight = predictions[i - 1]["v"].as<float>();
    float currentHeight = predictions[i]["v"].as<float>();
    int currentTrend = (currentHeight > prevHeight) ? 1 : ((currentHeight < prevHeight) ? -1 : lastTrend);

    if (currentTrend != lastTrend && lastTrend != 0) {
      JsonObject peak = predictions[i - 1];
      int hour24 = peak["t"].as<String>().substring(11, 13).toInt();
      String minutes = peak["t"].as<String>().substring(14, 16);
      String suffix = (hour24 < 12) ? "am" : "pm";
      int hour12 = (hour24 % 12 == 0) ? 12 : hour24 % 12;
      String displayTime = String(hour12) + ":" + minutes + suffix;
      String eventStr = displayTime + " (" + String(peak["v"].as<float>(), 1) + "ft)";

      if (lastTrend == 1 && highTideCount < 2) {
        highTideEvents[highTideCount++] = eventStr;
      } else if (lastTrend == -1 && lowTideCount < 2) {
        lowTideEvents[lowTideCount++] = eventStr;
      }
    }
    lastTrend = currentTrend;
  }
}

/**
 * Renders the complete tide chart UI on the TFT display.
 * @param timeinfo A struct containing the current time for the display.
 */
void drawTideChart(const struct tm& timeinfo) {
  tft.fillScreen(TFT_BLACK);
  
  char displayDate[20];
  strftime(displayDate, sizeof(displayDate), "%A, %b %d", &timeinfo);
  tft.setTextColor(TFT_YELLOW);
  tft.drawCentreString(displayDate, tft.width() / 2, 10, 4);

  tft.setTextColor(TFT_CYAN);
  tft.setTextFont(4);
  tft.drawString("High", 15, 45);
  tft.setTextFont(2);
  tft.drawString(highTideEvents[0], 95, 50);
  if (highTideEvents[1] != "") tft.drawString(highTideEvents[1], 205, 50);

  tft.setTextColor(TFT_MAGENTA);
  tft.setTextFont(4);
  tft.drawString("Low", 15, 80);
  tft.setTextFont(2);
  tft.drawString(lowTideEvents[0], 95, 85);
  if (lowTideEvents[1] != "") tft.drawString(lowTideEvents[1], 205, 85);

  const int graphX = 10, graphY = 115;
  const int graphW = tft.width() - 20, graphH = tft.height() - 125;
  tft.drawRect(graphX, graphY, graphW, graphH, TFT_DARKGREY);

  int lastX = -1, lastY = -1;
  for (JsonObject p : predictions) {
    String timeStr = p["t"].as<String>();
    int totalMinutes = timeStr.substring(11, 13).toInt() * 60 + timeStr.substring(14, 16).toInt();
    int x = map(totalMinutes, 0, 1440, graphX, graphX + graphW);
    int y = map(p["v"].as<float>(), minTide, maxTide, graphY + graphH, graphY);

    if (lastX != -1) tft.drawLine(lastX, lastY, x, y, TFT_SKYBLUE);
    lastX = x;
    lastY = y;
  }

  int currentMinute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int timeX = map(currentMinute, 0, 1440, graphX, graphX + graphW);
  tft.drawFastVLine(timeX, graphY, graphH, TFT_RED);
}
