
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>

// ==========================================
// WIFI CONFIGURATION
// ==========================================

const char* ssid = "ZfamilyA_2.4G@unifi";
const char* password = "25122020";

// ==========================================
// GOOGLE APPS SCRIPT URL
// ==========================================

const char* scriptURL =
"https://script.google.com/macros/s/AKfycbxvLvdSjhvYWmg6MKNnzwaxI75s9OmUPDLJMaRSkoZPD5m_hDY-fElxK4BSzAXjmMZq/exec";

// ==========================================
// SENSOR CONFIGURATION
// ==========================================

#define DHTPIN 16
#define DHTTYPE DHT11

DHT dht(DHTPIN, DHTTYPE);

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

// ==========================================
// TIMING CONFIGURATION
// ==========================================

// Sampling interval: 10 minutes
const unsigned long sendInterval = 600000;

// Retry delay: 5 seconds
const unsigned long retryDelay = 5000;

// WiFi connection timeout: 20 seconds
const unsigned long wifiTimeout = 20000;

unsigned long previousMillis = 0;

bool firstSend = true;

// ==========================================
// ERROR HANDLING CONFIGURATION
// ==========================================

const int maxRetries = 3;
const int maxConsecutiveFailures = 3;

int consecutiveFailures = 0;

unsigned long successfulUploads = 0;
unsigned long failedUploads = 0;

enum SendResult {
  SEND_SUCCESS,
  CONNECTION_FAILED,
  SEND_UNCONFIRMED
};

// ==========================================
// MEMORY MONITORING
// ==========================================

void monitorMemory() {

  Serial.print("[MEMORY] Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" bytes");

  Serial.print("[MEMORY] Minimum Free Heap: ");
  Serial.print(ESP.getMinFreeHeap());
  Serial.println(" bytes");

  Serial.print("[MEMORY] Largest Free Block: ");
  Serial.print(ESP.getMaxAllocHeap());
  Serial.println(" bytes");
}

// ==========================================
// WIFI CONNECTION
// ==========================================

bool connectWiFi(bool forceReconnect = false) {

  if (!forceReconnect &&
      WiFi.status() == WL_CONNECTED) {

    return true;
  }

  Serial.println();
  Serial.println("[WIFI] Connecting to WiFi...");

  digitalWrite(LED_BUILTIN, LOW);

  if (forceReconnect) {

    Serial.println("[WIFI] Performing forced reconnection...");

    WiFi.disconnect(true, false);

    delay(1000);
  }

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  WiFi.setAutoReconnect(true);

  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttempt < wifiTimeout) {

    Serial.print(".");

    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println();
    Serial.println("[WIFI] CONNECTED!");

    Serial.print("[WIFI] IP Address: ");
    Serial.println(WiFi.localIP());

    Serial.print("[WIFI] Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");

    digitalWrite(LED_BUILTIN, HIGH);

    return true;
  }

  Serial.println();
  Serial.println("[WIFI] CONNECTION FAILED!");

  digitalWrite(LED_BUILTIN, LOW);

  return false;
}

// ==========================================
// SENSOR DATA VALIDATION
// ==========================================

bool validateSensorData(float temperature,
                        float humidity) {

  if (isnan(temperature) || isnan(humidity)) {

    Serial.println("[ERROR] Invalid sensor reading!");

    return false;
  }

  if (temperature < 0 || temperature > 50) {

    Serial.println("[ERROR] Temperature out of range!");

    return false;
  }

  if (humidity < 0 || humidity > 100) {

    Serial.println("[ERROR] Humidity out of range!");

    return false;
  }

  return true;
}

// ==========================================
// HTTP CONFIGURATION
// ==========================================

void configureHTTP(HTTPClient &http) {

  http.setReuse(false);

  http.setConnectTimeout(10000);

  http.setTimeout(15000);

  http.setFollowRedirects(
    HTTPC_DISABLE_FOLLOW_REDIRECTS
  );
}

// ==========================================
// VALIDATE SERVER RESPONSE
// ==========================================

bool validateServerResponse(int responseCode,
                            String response) {

  Serial.print("[HTTP] Final Response Code: ");
  Serial.println(responseCode);

  Serial.print("[HTTP] Server Response: ");
  Serial.println(response);

  if (responseCode != HTTP_CODE_OK) {

    Serial.println("[ERROR] Unexpected HTTP response.");

    return false;
  }

  StaticJsonDocument<512> responseDoc;

  DeserializationError error =
    deserializeJson(responseDoc, response);

  if (error) {

    Serial.println("[ERROR] Invalid JSON response!");

    return false;
  }

  String status = responseDoc["status"] | "";

  if (status == "success") {

    Serial.println("[SUCCESS] Data saved to Google Sheets!");

    return true;
  }

  Serial.println("[ERROR] Apps Script rejected the data.");

  return false;
}

// ==========================================
// RETRIEVE GOOGLE SCRIPT RESPONSE
// ==========================================

bool retrieveServerResponse(String redirectURL) {

  if (!redirectURL.startsWith("https://")) {

    Serial.println("[ERROR] Invalid redirect URL!");

    return false;
  }

  // Separate HTTPS client for GET request
  WiFiClientSecure redirectClient;

  redirectClient.setInsecure();

  redirectClient.setHandshakeTimeout(15);

  HTTPClient redirectHttp;

  configureHTTP(redirectHttp);

  redirectHttp.setFollowRedirects(
    HTTPC_STRICT_FOLLOW_REDIRECTS
  );

  Serial.println("[HTTP] Retrieving server response...");

  if (!redirectHttp.begin(redirectClient, redirectURL)) {

    Serial.println("[ERROR] Redirect initialization failed!");

    redirectHttp.end();
    redirectClient.stop();

    return false;
  }

  int responseCode = redirectHttp.GET();

  String response = "";

  if (responseCode > 0) {

    response = redirectHttp.getString();

  } else {

    Serial.print("[ERROR] Redirect GET failed: ");

    Serial.println(
      redirectHttp.errorToString(responseCode)
    );
  }

  // Explicit connection cleanup
  redirectHttp.end();

  redirectClient.stop();

  Serial.println("[HTTP] Redirect connection closed.");

  return validateServerResponse(
    responseCode,
    response
  );
}

// ==========================================
// SEND DATA TO GOOGLE SHEETS
// ==========================================

SendResult sendDataToGoogleSheets(float temperature,
                                 float humidity) {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("[ERROR] WiFi disconnected!");

    return CONNECTION_FAILED;
  }

  // Create a fresh HTTPS client for every POST
  WiFiClientSecure client;

  client.setInsecure();

  client.setHandshakeTimeout(15);

  HTTPClient http;

  configureHTTP(http);

  Serial.println("[HTTP] Connecting to Google Sheets...");

  if (!http.begin(client, scriptURL)) {

    Serial.println("[ERROR] HTTP initialization failed!");

    http.end();
    client.stop();

    return CONNECTION_FAILED;
  }

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  StaticJsonDocument<200> doc;

  doc["temperature"] = temperature;
  doc["humidity"] = humidity;

  String jsonString;

  serializeJson(doc, jsonString);

  Serial.print("[HTTP] Sending JSON: ");
  Serial.println(jsonString);

  int responseCode = http.POST(jsonString);

  String response = "";
  String redirectURL = "";

  if (responseCode > 0) {

    Serial.print("[HTTP] Response Code: ");
    Serial.println(responseCode);

    if (responseCode == 302 ||
        responseCode == 303) {

      redirectURL = http.getLocation();

    } else {

      response = http.getString();
    }

  } else {

    Serial.print("[ERROR] HTTP POST failed: ");

    Serial.println(
      http.errorToString(responseCode)
    );
  }

  // Explicit connection cleanup
  http.end();

  client.stop();

  Serial.println("[HTTP] POST connection closed.");

  // Connection refused before confirmed transmission
  if (responseCode == HTTPC_ERROR_CONNECTION_REFUSED) {

    return CONNECTION_FAILED;
  }

  // Other errors may occur after data was transmitted.
  // Avoid blindly repeating the POST.
  if (responseCode <= 0) {

    return SEND_UNCONFIRMED;
  }

  // Google Apps Script response redirect
  if (responseCode == 302 ||
      responseCode == 303) {

    delay(250);

    if (retrieveServerResponse(redirectURL)) {

      return SEND_SUCCESS;
    }

    // POST may already have stored the data.
    // Do not repeat POST automatically.
    return SEND_UNCONFIRMED;
  }

  if (validateServerResponse(responseCode, response)) {

    return SEND_SUCCESS;
  }

  return SEND_UNCONFIRMED;
}

// ==========================================
// AUTOMATIC RETRY AND RECOVERY
// ==========================================

SendResult sendWithRetry(float temperature,
                         float humidity) {

  for (int attempt = 1;
       attempt <= maxRetries;
       attempt++) {

    Serial.println();

    Serial.print("[RETRY] Attempt ");
    Serial.print(attempt);
    Serial.print("/");
    Serial.println(maxRetries);

    bool wifiReady = connectWiFi(attempt > 1);

    if (wifiReady) {

      monitorMemory();

      SendResult result =
        sendDataToGoogleSheets(
          temperature,
          humidity
        );

      if (result == SEND_SUCCESS) {

        return SEND_SUCCESS;
      }

      if (result == SEND_UNCONFIRMED) {

        Serial.println(
          "[WARNING] Upload result uncertain."
        );

        Serial.println(
          "[WARNING] POST will not be repeated."
        );

        return SEND_UNCONFIRMED;
      }
    }

    if (attempt < maxRetries) {

      Serial.println(
        "[RECOVERY] Retrying in 5 seconds..."
      );

      delay(retryDelay);
    }
  }

  return CONNECTION_FAILED;
}

// ==========================================
// SYSTEM STATUS
// ==========================================

void printSystemStatus() {

  Serial.println();
  Serial.println("---------- SYSTEM STATUS ----------");

  Serial.print("[STATUS] Successful Uploads: ");
  Serial.println(successfulUploads);

  Serial.print("[STATUS] Failed/Unconfirmed Uploads: ");
  Serial.println(failedUploads);

  Serial.print("[STATUS] Consecutive Connection Failures: ");
  Serial.println(consecutiveFailures);

  if (WiFi.status() == WL_CONNECTED) {

    Serial.print("[WIFI] Signal Strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  }

  monitorMemory();

  Serial.println("-----------------------------------");
}

// ==========================================
// SYSTEM SETUP
// ==========================================

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("================================");
  Serial.println(" ESP32 DHT11 FORECASTING SYSTEM");
  Serial.println(" Enhanced Automatic Recovery");
  Serial.println("================================");

  pinMode(LED_BUILTIN, OUTPUT);

  digitalWrite(LED_BUILTIN, LOW);

  dht.begin();

  Serial.println("[SYSTEM] DHT11 initialized.");

  WiFi.persistent(false);

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  WiFi.setAutoReconnect(true);

  connectWiFi();

  delay(2000);

  monitorMemory();

  Serial.println("[SYSTEM] Starting data logging...");
}

// ==========================================
// MAIN LOOP
// ==========================================

void loop() {

  digitalWrite(
    LED_BUILTIN,
    WiFi.status() == WL_CONNECTED ? HIGH : LOW
  );

  unsigned long currentMillis = millis();

  if (firstSend ||
      currentMillis - previousMillis >= sendInterval) {

    firstSend = false;

    previousMillis = currentMillis;

    Serial.println();
    Serial.println("---------- NEW READING ----------");

    float humidity = dht.readHumidity();

    float temperature = dht.readTemperature();

    if (validateSensorData(temperature, humidity)) {

      Serial.print("[SENSOR] Temperature: ");
      Serial.print(temperature);
      Serial.println(" C");

      Serial.print("[SENSOR] Humidity: ");
      Serial.print(humidity);
      Serial.println(" %");

      SendResult result =
        sendWithRetry(
          temperature,
          humidity
        );

      if (result == SEND_SUCCESS) {

        successfulUploads++;

        consecutiveFailures = 0;

        Serial.println(
          "[SYSTEM] Data logging completed."
        );

      } else {

        failedUploads++;

        if (result == CONNECTION_FAILED) {

          consecutiveFailures++;

          Serial.println(
            "[SYSTEM] All connection attempts failed."
          );

        } else {

          consecutiveFailures = 0;

          Serial.println(
            "[SYSTEM] Data logging not confirmed."
          );
        }
      }

      printSystemStatus();

      // Automatic restart as last recovery option
      if (consecutiveFailures >=
          maxConsecutiveFailures) {

        Serial.println(
          "[RECOVERY] Maximum consecutive failures reached."
        );

        Serial.println(
          "[RECOVERY] Restarting ESP32 in 5 seconds..."
        );

        delay(5000);

        ESP.restart();
      }
    }

    Serial.println("---------------------------------");
  }

  delay(100);
}
