// #include <Arduino.h>
// #include <BluetoothSerial.h>
// #include <esp32-hal-bt.h>
// #include <Preferences.h>
// #include <WiFi.h>
// #include <WiFiClientSecure.h>
// #include "esp_heap_caps.h"

// #include <OneWire.h>
// #include <DallasTemperature.h>
// #include <DHT.h>
// #include <ArduinoJson.h>

// /*
//   MOGI Combined Sketch
//   - Bluetooth (SPP) provisioning: terima SSID+Password (opsional juga API config)
//   - Connect WiFi
//   - Read sensors: DS18B20 (temperature) + DHT22 (humidity + temp fallback)
//   - POST ke backend: /api/arduino/temperature pakai Device API Key

//   Kompatibel dengan flow lama bluetoothtest.ino:
//   - Kirim SSID (line 1)
//   - Kirim Password (line 2)

//   Flow baru yang direkomendasikan:
//   - Kirim JSON satu baris via Bluetooth, contoh:
//     {"ssid":"TelU-IOT","password":"TUKJ#2024.","deviceCode":"MOGI-003","deviceApiKey":"mogi_MOGI-003_xxx","apiBaseUrl":"https://mogi-backend-firebase.vercel.app"}

//   Command tambahan:
//   - RESET_WIFI        -> hapus WiFi tersimpan
//   - SHOW_CONFIG       -> tampilkan config ringkas
// */

// /* =================== DEVICE/BACKEND DEFAULTS =================== */
// static const char* DEFAULT_API_BASE_URL = "https://mogi-backend-firebase.vercel.app";
// static const char* DEFAULT_DEVICE_CODE = "MOGI-006";

// // API Key akan dikirim otomatis dari aplikasi setelah user register device.
// // Biarkan kosong — device akan menerima key via Bluetooth provisioning.
// static const char* DEFAULT_DEVICE_API_KEY = "";

// static const uint32_t DEFAULT_SEND_INTERVAL_MS = 20000; // 30 detik
// static const float HUMIDITY_FALLBACK = 75.0f; // fallback jika DHT gagal

// // If true, Bluetooth will be fully stopped once WiFi is connected and the device key is set.
// // NOTE: Keep this false if you want the device to stay always discoverable for "forget/reset" flows.
// // Memory is still protected by temporarily pausing BT during TLS handshake when no client is connected.
// static const bool AUTO_DISABLE_BT_WHEN_READY = false;

// /* =================== BLUETOOTH =================== */
// BluetoothSerial SerialBT;
// static const char* BT_DEVICE_NAME_FALLBACK = "MOGI_ESP32";
// String btDeviceName;
// static bool bluetoothEnabled = true;

// static String btRxBuffer;

// // Forward declaration (used by TLS/BT helpers)
// void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param);

// /* =================== HTTPS/TLS (MEMORY) =================== */
// static WiFiClientSecure tlsClient;
// static bool tlsClientInit = false;

// static uint32_t largestFreeBlock() {
//   return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
// }

// static void disableBluetoothNow(const char* reason);
// static void maybeAutoDisableBluetooth(const char* reason);

// struct BaseUrlParts {
//   bool https;
//   String host;
//   uint16_t port;
// };

// static bool parseBaseUrl(const String& baseUrl, BaseUrlParts& out);

// static bool getCachedBaseUrlParts(const String& baseUrl, BaseUrlParts& out) {
//   static String cached;
//   static BaseUrlParts cachedParts;
//   static bool cachedOk = false;

//   if (cached != baseUrl) {
//     cached = baseUrl;
//     cachedOk = parseBaseUrl(cached, cachedParts);
//   }
//   if (!cachedOk) return false;
//   out = cachedParts;
//   return true;
// }

// static bool parseBaseUrl(const String& baseUrl, BaseUrlParts& out) {
//   String s = baseUrl;
//   s.trim();
//   if (!s.length()) return false;

//   bool https = true;
//   if (s.startsWith("https://")) {
//     https = true;
//     s = s.substring(8);
//   } else if (s.startsWith("http://")) {
//     https = false;
//     s = s.substring(7);
//   }

//   int slash = s.indexOf('/');
//   if (slash >= 0) s = s.substring(0, slash);

//   uint16_t port = https ? 443 : 80;
//   int colon = s.indexOf(':');
//   if (colon >= 0) {
//     String hostPart = s.substring(0, colon);
//     String portPart = s.substring(colon + 1);
//     portPart.trim();
//     int p = portPart.toInt();
//     if (p > 0 && p <= 65535) port = (uint16_t)p;
//     s = hostPart;
//   }

//   s.trim();
//   if (!s.length()) return false;
//   out.https = https;
//   out.host = s;
//   out.port = port;
//   return true;
// }

// static int readHttpStatusCode(Stream& stream) {
//   // Read first line: HTTP/1.1 200 OK
//   char line[96];
//   size_t idx = 0;
//   unsigned long start = millis();
//   while (millis() - start < 8000) {
//     while (stream.available()) {
//       char c = (char)stream.read();
//       if (c == '\r') continue;
//       if (c == '\n') {
//         line[idx] = 0;
//         // Parse status code
//         // Expected: "HTTP/1.1 200"
//         const char* sp1 = strchr(line, ' ');
//         if (!sp1) return -1;
//         while (*sp1 == ' ') sp1++;
//         return atoi(sp1);
//       }
//       if (idx < sizeof(line) - 1) {
//         line[idx++] = c;
//       }
//     }
//     delay(10);
//   }
//   return -1;
// }

// static void drainHttpHeaders(Stream& stream) {
//   // Consume headers until blank line
//   int newlines = 0;
//   unsigned long start = millis();
//   while (millis() - start < 8000) {
//     while (stream.available()) {
//       char c = (char)stream.read();
//       if (c == '\r') continue;
//       if (c == '\n') {
//         newlines++;
//         if (newlines >= 2) return;
//       } else {
//         newlines = 0;
//       }
//     }
//     delay(5);
//   }
// }

// static bool pauseBluetoothIfIdleForTls() {
//   if (!bluetoothEnabled) return false;

//   // Only pause when no client is connected; we don't want to drop an active provisioning session.
//   if (SerialBT.hasClient()) return false;

//   Serial.println("[BT] Pausing Bluetooth to free heap for TLS...");
//   SerialBT.end();
//   btStop();
//   delay(120);
//   Serial.printf("[MEM] Free heap after BT pause: %u bytes\n", ESP.getFreeHeap());
//   Serial.printf("[MEM] Largest free block after BT pause: %u bytes\n", largestFreeBlock());
//   return true;
// }

// static void resumeBluetoothAfterTlsIfPaused(bool wasPaused) {
//   if (!wasPaused) return;
//   Serial.println("[BT] Resuming Bluetooth...");
//   btStart();
//   delay(80);
//   SerialBT.register_callback(btCallback);
//   SerialBT.begin(btDeviceName.c_str());
//   delay(80);
// }

// /* =================== PREFERENCES (NVS) =================== */
// Preferences prefs;
// static const char* PREF_NAMESPACE = "mogi";

// /* =================== PINS =================== */
// #define ONE_WIRE_BUS 33
// #define DHTPIN 18
// #define DHTTYPE DHT22

// // Relay & Motor (L298N)
// #define RELAY_PIN 32
// #define EN1 25
// #define EN2 26
// #define ENA 27  // PWM pin

// /* =================== PWM CONFIG =================== */
// #define PWM_FREQ     1000
// #define PWM_RES      8
// #define PWM_CHANNEL  0

// /* =================== SENSORS =================== */
// OneWire oneWire(ONE_WIRE_BUS);
// DallasTemperature ds18b20(&oneWire);
// DHT dht(DHTPIN, DHTTYPE);

// /* =================== RUNTIME CONFIG =================== */
// String wifiSsid;
// String wifiPassword;

// String apiBaseUrl;
// String deviceCode;
// String deviceApiKey;

// uint32_t sendIntervalMs = DEFAULT_SEND_INTERVAL_MS;

// bool wifiConnectedOnce = false;
// unsigned long lastSendMs = 0;

// /* =================== RELAY & MOTOR STATE =================== */
// bool relayState = false;
// int pwmValue = 0;

// /* =================== BLUETOOTH LEGACY STATE =================== */
// bool waitingForPassword = false;

// /* =================== BT MEMORY HELPERS =================== */
// static void disableBluetoothNow(const char* reason) {
//   if (!bluetoothEnabled) return;
//   Serial.print("[BT] Disabling Bluetooth (memory save). Reason: ");
//   Serial.println(reason ? reason : "-");
//   SerialBT.end();
//   btStop();
//   bluetoothEnabled = false;
//   btRxBuffer = "";
//   delay(120);
//   Serial.printf("[MEM] Free heap after BT disable: %u bytes\n", ESP.getFreeHeap());
//   Serial.printf("[MEM] Largest free block after BT disable: %u bytes\n", largestFreeBlock());
// }

// static void maybeAutoDisableBluetooth(const char* reason) {
//   if (!AUTO_DISABLE_BT_WHEN_READY) return;
//   if (!bluetoothEnabled) return;
//   if (WiFi.status() != WL_CONNECTED) return;
//   if (!deviceApiKey.length()) return;
//   if (SerialBT.hasClient()) return;
//   disableBluetoothNow(reason);
// }

// /* =================== HELPERS =================== */
// static void btPrintln(const String& s) {
//   if (bluetoothEnabled && SerialBT.hasClient()) {
//     SerialBT.println(s);
//   }
//   Serial.print("[BT] ");
//   Serial.println(s);
// }

// static void disableBluetoothForTlsIfNeeded() {
//   if (bluetoothEnabled && !SerialBT.hasClient()) {
//     SerialBT.end();
//     btStop();
//     delay(200);
//   }
// }


// static void clearAllPrefs() {
//   prefs.begin(PREF_NAMESPACE, false);
//   prefs.remove("ssid");
//   prefs.remove("pass");
//   prefs.remove("api");
//   prefs.remove("code");
//   prefs.remove("key");
//   prefs.remove("interval");
//   prefs.end();
// }

// static void loadConfigFromPrefs() {
//   prefs.begin(PREF_NAMESPACE, true);

//   wifiSsid = prefs.getString("ssid", "");
//   wifiPassword = prefs.getString("pass", "");

//   apiBaseUrl = prefs.getString("api", DEFAULT_API_BASE_URL);
//   deviceCode = prefs.getString("code", DEFAULT_DEVICE_CODE);
//   deviceApiKey = prefs.getString("key", DEFAULT_DEVICE_API_KEY);

//   sendIntervalMs = prefs.getUInt("interval", DEFAULT_SEND_INTERVAL_MS);

//   prefs.end();
// }

// static void saveWifiToPrefs(const String& ssid, const String& pass) {
//   prefs.begin(PREF_NAMESPACE, false);
//   prefs.putString("ssid", ssid);
//   prefs.putString("pass", pass);
//   prefs.end();
// }

// static void saveApiConfigToPrefs(const String& baseUrl, const String& code, const String& key, uint32_t intervalMs) {
//   prefs.begin(PREF_NAMESPACE, false);
//   prefs.putString("api", baseUrl);
//   prefs.putString("code", code);
//   prefs.putString("key", key);
//   prefs.putUInt("interval", intervalMs);
//   prefs.end();
// }

// static void clearWifiPrefs() {
//   prefs.begin(PREF_NAMESPACE, false);
//   prefs.remove("ssid");
//   prefs.remove("pass");
//   prefs.end();
// }

// static bool isProvisioned() {
//   // Provisioned means we have at least WiFi SSID and Device API Key.
//   // WiFi password is optional (open networks supported).
//   return wifiSsid.length() && deviceApiKey.length();
// }

// static void showConfig() {
//   Serial.println("\n===== MOGI CONFIG =====");
//   Serial.println("WiFi SSID   : " + (wifiSsid.length() ? wifiSsid : String("<empty>")));
//   Serial.println("WiFi Pass   : " + String(wifiPassword.length() ? "<set>" : "<empty>"));
//   Serial.println("API BaseURL : " + apiBaseUrl);
//   Serial.println("Device Code : " + deviceCode);
//   Serial.println("Device Key  : " + String(deviceApiKey.length() ? "<set>" : "<empty>"));
//   Serial.println("Interval ms : " + String(sendIntervalMs));
//   Serial.println("=======================\n");

//   btPrintln("WiFi SSID: " + (wifiSsid.length() ? wifiSsid : String("<empty>")));
//   btPrintln("API: " + apiBaseUrl);
//   btPrintln("Device: " + deviceCode);
//   btPrintln("Key: " + String(deviceApiKey.length() ? "<set>" : "<empty>"));
//   btPrintln("Interval(ms): " + String(sendIntervalMs));
// }

// static bool connectWiFiWithCurrentConfig(uint32_t timeoutMs = 15000) {
//   if (!wifiSsid.length()) {
//     Serial.println("[WiFi] SSID kosong. Provision via Bluetooth dulu.");
//     return false;
//   }

//   Serial.println("\n[WiFi] Connecting...");
//   Serial.println("[WiFi] SSID: " + wifiSsid);

//   WiFi.mode(WIFI_STA);
//   if (wifiPassword.length()) {
//     WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
//   } else {
//     // Support open networks / no password provisioning
//     WiFi.begin(wifiSsid.c_str());
//   }

//   const uint32_t start = millis();
//   while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
//     delay(250);
//     Serial.print(".");
//   }

//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("\n[WiFi] Connected");
//     Serial.print("[WiFi] IP: ");
//     Serial.println(WiFi.localIP());
//     wifiConnectedOnce = true;

//     // If provisioning already finished, we can disable Bluetooth to save heap.
//     maybeAutoDisableBluetooth("WiFi connected");
//     return true;
//   }

//   Serial.println("\n[WiFi] Failed");
//   return false;
// }

// static void performResetAll() {
//   Serial.println("\n🔄 RESET ALL triggered! Clearing all config and restarting...");
//   clearAllPrefs();
//   wifiSsid = "";
//   wifiPassword = "";
//   apiBaseUrl = DEFAULT_API_BASE_URL;
//   deviceCode = DEFAULT_DEVICE_CODE;
//   deviceApiKey = DEFAULT_DEVICE_API_KEY;
//   sendIntervalMs = DEFAULT_SEND_INTERVAL_MS;
//   waitingForPassword = false;
//   delay(300);
//   ESP.restart();
// }

// static void sendTemperatureToApi(float temperatureC, float humidity) {
//   if (WiFi.status() != WL_CONNECTED) {
//     Serial.println("❌ WiFi not connected. Skipping API call.");
//     return;
//   }

//   // If device is already configured and BT is idle, keep BT off for TLS stability.
//   maybeAutoDisableBluetooth("before TLS");

//   const char* useDeviceCode = deviceCode.length() ? deviceCode.c_str() : DEFAULT_DEVICE_CODE;

//   // Cek apakah API key sudah di-provision dari HP
//   if (!deviceApiKey.length()) {
//     Serial.println("⏳ Device API Key belum di-set. Register device dari aplikasi dulu.");
//     return;
//   }
//   const char* useDeviceApiKey = deviceApiKey.c_str();

//   if (!useDeviceCode || !*useDeviceCode) {
//     Serial.println("❌ deviceCode kosong.");
//     return;
//   }

//   Serial.println("\n📡 Sending data to API...");
//   Serial.printf("[MEM] Free heap: %u bytes\n", ESP.getFreeHeap());
//   Serial.printf("[MEM] Largest free block: %u bytes\n", largestFreeBlock());

//   BaseUrlParts base;
//   if (!getCachedBaseUrlParts(apiBaseUrl, base)) {
//     Serial.println("❌ apiBaseUrl invalid.");
//     return;
//   }

//   static const char* PATH = "/api/arduino/temperature";
//   Serial.print("URL: ");
//   Serial.print(base.https ? "https://" : "http://");
//   Serial.print(base.host);
//   Serial.println(PATH);

//   // Free memory for TLS handshake (only if BT is idle)
//   const bool btPaused = pauseBluetoothIfIdleForTls();

//   // Create compact JSON payload without ArduinoJson/String to minimize heap usage.
//   // Backend expects: { temperature: number, humidity: number }
//   // Keep keys exactly the same.
//   // Force finite numbers to ensure backend validation passes.
//   float t = temperatureC;
//   float h = humidity;
//   if (!isfinite(t)) t = -999.0f;
//   if (!isfinite(h)) h = -999.0f;

//   // Keep a reasonable precision to reduce payload size.
//   // DS18B20 resolution is 0.0625°C, so 2 decimals is enough.
//   char jsonPayload[96];
//   const int payloadLen = snprintf(
//     jsonPayload,
//     sizeof(jsonPayload),
//     "{\"temperature\":%.2f,\"humidity\":%.2f}",
//     (double)t,
//     (double)h
//   );
//   if (payloadLen <= 0 || payloadLen >= (int)sizeof(jsonPayload)) {
//     Serial.println("❌ Payload build failed (buffer too small).");
//     resumeBluetoothAfterTlsIfPaused(btPaused);
//     return;
//   }

//   Serial.print("Payload: ");
//   Serial.println(jsonPayload);

//   int statusCode = -1;

//   if (base.https) {
//     // TLS client init (minimize RAM usage)
//     if (!tlsClientInit) {
//       tlsClient.setInsecure();
//       tlsClient.setHandshakeTimeout(10);
//       tlsClient.setTimeout(15000);
//       tlsClientInit = true;
//     }

//     tlsClient.stop();
//     delay(60);

//     Serial.printf("[TLS] Connecting to %s:%u ...\n", base.host.c_str(), base.port);
//     if (!tlsClient.connect(base.host.c_str(), base.port)) {
//       Serial.println("❌ TLS connect failed.");
//       char errBuf[96];
//       tlsClient.lastError(errBuf, sizeof(errBuf));
//       Serial.print("[TLS] lastError: ");
//       Serial.println(errBuf);
//       statusCode = -1;
//     } else {
//       // Minimal HTTP/1.0 request to reduce memory pressure
//       tlsClient.print("POST ");
//       tlsClient.print(PATH);
//       tlsClient.print(" HTTP/1.0\r\n");
//       tlsClient.print("Host: ");
//       tlsClient.print(base.host);
//       tlsClient.print("\r\n");
//       tlsClient.print("Content-Type: application/json\r\n");
//       tlsClient.print("X-Device-Code: ");
//       tlsClient.print(useDeviceCode);
//       tlsClient.print("\r\n");
//       tlsClient.print("X-Device-Key: ");
//       tlsClient.print(useDeviceApiKey);
//       tlsClient.print("\r\n");
//       tlsClient.print("Content-Length: ");
//       tlsClient.print(payloadLen);
//       tlsClient.print("\r\n");
//       tlsClient.print("Connection: close\r\n\r\n");
//       tlsClient.write((const uint8_t*)jsonPayload, (size_t)payloadLen);

//       statusCode = readHttpStatusCode(tlsClient);
//       drainHttpHeaders(tlsClient);
//       tlsClient.stop();
//     }
//   } else {
//     WiFiClient plain;
//     plain.setTimeout(15000);
//     Serial.printf("[HTTP] Connecting to %s:%u ...\n", base.host.c_str(), base.port);
//     if (!plain.connect(base.host.c_str(), base.port)) {
//       Serial.println("❌ HTTP connect failed.");
//       statusCode = -1;
//     } else {
//       plain.print("POST ");
//       plain.print(PATH);
//       plain.print(" HTTP/1.0\r\n");
//       plain.print("Host: ");
//       plain.print(base.host);
//       plain.print("\r\n");
//       plain.print("Content-Type: application/json\r\n");
//       plain.print("X-Device-Code: ");
//       plain.print(useDeviceCode);
//       plain.print("\r\n");
//       plain.print("X-Device-Key: ");
//       plain.print(useDeviceApiKey);
//       plain.print("\r\n");
//       plain.print("Content-Length: ");
//       plain.print(payloadLen);
//       plain.print("\r\n");
//       plain.print("Connection: close\r\n\r\n");
//       plain.write((const uint8_t*)jsonPayload, (size_t)payloadLen);

//       statusCode = readHttpStatusCode(plain);
//       drainHttpHeaders(plain);
//       plain.stop();
//     }
//   }

//   if (statusCode > 0) {
//     Serial.print("✅ Response Code: ");
//     Serial.println(statusCode);
//     if (statusCode == 200 || statusCode == 201) {
//       Serial.println("✅ Data sent successfully!");
//     } else if (statusCode == 401) {
//       Serial.println("🚫 401 Unauthorized! API key invalid. Performing RESET ALL...");
//       resumeBluetoothAfterTlsIfPaused(btPaused);
//       performResetAll();
//       return; // won't reach here (ESP restarts), but just in case
//     } else {
//       Serial.println("⚠️ Server returned non-success code");
//     }
//   } else {
//     Serial.println("❌ Request failed (no HTTP status).");
//   }

//   resumeBluetoothAfterTlsIfPaused(btPaused);
//   Serial.println("================================\n");
// }

// static int findJsonKeyPos(const String& json, const char* key) {
//   String pat = String('"') + key + '"';
//   return json.indexOf(pat);
// }

// static bool jsonHasKey(const String& json, const char* key) {
//   return findJsonKeyPos(json, key) >= 0;
// }

// static String jsonGetStringValue(const String& json, const char* key) {
//   int keyPos = findJsonKeyPos(json, key);
//   if (keyPos < 0) return "";
//   int colon = json.indexOf(':', keyPos);
//   if (colon < 0) return "";

//   int i = colon + 1;
//   while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
//   if (i >= (int)json.length()) return "";

//   if (json[i] != '"') return ""; // only support quoted string for safety
//   i++; // after opening quote
//   int end = json.indexOf('"', i);
//   if (end < 0) return "";
//   return json.substring(i, end);
// }

// static bool jsonGetUIntValue(const String& json, const char* key, uint32_t& out) {
//   int keyPos = findJsonKeyPos(json, key);
//   if (keyPos < 0) return false;
//   int colon = json.indexOf(':', keyPos);
//   if (colon < 0) return false;

//   int i = colon + 1;
//   while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
//   if (i >= (int)json.length()) return false;

//   int j = i;
//   while (j < (int)json.length() && json[j] >= '0' && json[j] <= '9') j++;
//   if (j == i) return false;

//   out = (uint32_t)json.substring(i, j).toInt();
//   return true;
// }

// static bool parseAndApplyJsonConfig(const String& jsonLine) {
//   // Lightweight JSON parser: supports keys with quoted string values and sendIntervalMs as number.
//   // Example:
//   // {"ssid":"...","password":"...","deviceCode":"MOGI-003","deviceApiKey":"mogi_...","apiBaseUrl":"https://...","sendIntervalMs":30000}
//   bool any = false;
//   const bool keyAlreadySet = deviceApiKey.length() > 0;

//   // Apply fields if key exists in JSON, even if value is empty.
//   // Ini penting supaya value lama di NVS bisa di-clear dari HP.
//   if (jsonHasKey(jsonLine, "ssid")) {
//     wifiSsid = jsonGetStringValue(jsonLine, "ssid");
//     any = true;
//   }
//   if (jsonHasKey(jsonLine, "password")) {
//     wifiPassword = jsonGetStringValue(jsonLine, "password");
//     any = true;
//   }
//   if (jsonHasKey(jsonLine, "apiBaseUrl")) {
//     apiBaseUrl = jsonGetStringValue(jsonLine, "apiBaseUrl");
//     any = true;
//   }
//   if (jsonHasKey(jsonLine, "deviceCode")) {
//     if (keyAlreadySet) {
//       btPrintln("Device sudah provisioned. deviceCode diabaikan (sudah terkunci). Gunakan RESET_ALL untuk ganti.");
//     } else {
//       deviceCode = jsonGetStringValue(jsonLine, "deviceCode");
//       any = true;
//     }
//   }
//   if (jsonHasKey(jsonLine, "deviceApiKey")) {
//     if (keyAlreadySet) {
//       btPrintln("Device API Key sudah tersimpan. Abaikan update key (terkunci). Gunakan RESET_ALL untuk ganti.");
//     } else {
//       deviceApiKey = jsonGetStringValue(jsonLine, "deviceApiKey");
//       any = true;
//     }
//   }
//   // Aliases (support multiple app/backends)
//   if (!jsonHasKey(jsonLine, "deviceApiKey") && jsonHasKey(jsonLine, "deviceKey")) {
//     if (keyAlreadySet) {
//       btPrintln("Device API Key sudah tersimpan. Abaikan update key (terkunci). Gunakan RESET_ALL untuk ganti.");
//     } else {
//       deviceApiKey = jsonGetStringValue(jsonLine, "deviceKey");
//       any = true;
//     }
//   }
//   if (!jsonHasKey(jsonLine, "deviceApiKey") && !jsonHasKey(jsonLine, "deviceKey") && jsonHasKey(jsonLine, "apiKey")) {
//     if (keyAlreadySet) {
//       btPrintln("Device API Key sudah tersimpan. Abaikan update key (terkunci). Gunakan RESET_ALL untuk ganti.");
//     } else {
//       deviceApiKey = jsonGetStringValue(jsonLine, "apiKey");
//       any = true;
//     }
//   }

//   uint32_t interval;
//   if (jsonGetUIntValue(jsonLine, "sendIntervalMs", interval)) {
//     sendIntervalMs = interval;
//     if (sendIntervalMs < 3000) sendIntervalMs = 3000;
//     any = true;
//   }

//   if (!any) return false;

//   btPrintln("JSON config diterima.");

//   // Save API config immediately (boleh kosong sebagian).
//   saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);

//   // If WiFi is already connected and key is set, free BT heap permanently.
//   maybeAutoDisableBluetooth("provisioned");

//   // Save WiFi immediately if SSID present (password optional).
//   if (wifiSsid.length()) {
//     saveWifiToPrefs(wifiSsid, wifiPassword);
//   }

//   showConfig();
//   return true;
// }

// static void maybeFinalizeProvisioningAndRestart(const char* reason) {
//   if (!isProvisioned()) return;

//   // Persist current WiFi (password optional) + API config then restart.
//   saveWifiToPrefs(wifiSsid, wifiPassword);
//   saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);

//   btPrintln(String("Provisioning lengkap (SSID + API Key). Restarting... ") + (reason ? reason : ""));
//   delay(300);
//   ESP.restart();
// }

// static void handleBluetoothLine(String line) {
//   line.trim();
//   if (!line.length()) return;
//   btPrintln("Received via BT: " + line);

//   // Quick set device key without JSON
//   if (line.startsWith("KEY:")) {
//     if (deviceApiKey.length()) {
//       btPrintln("Device API Key sudah tersimpan. Abaikan set key ulang (terkunci). Gunakan RESET_ALL untuk ganti.");
//       showConfig();
//       maybeAutoDisableBluetooth("key already set");
//       return;
//     }
//     deviceApiKey = line.substring(4);
//     deviceApiKey.trim();
//     saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);
//     btPrintln("Device API Key diset." + String(deviceApiKey.length() ? "" : " (kosong)"));
//     showConfig();
//     maybeFinalizeProvisioningAndRestart("(KEY:)");
//     maybeAutoDisableBluetooth("key set");
//     return;
//   }

//   if (line.startsWith("APIKEY:")) {
//     if (deviceApiKey.length()) {
//       btPrintln("Device API Key sudah tersimpan. Abaikan set key ulang (terkunci). Gunakan RESET_ALL untuk ganti.");
//       showConfig();
//       maybeAutoDisableBluetooth("key already set");
//       return;
//     }
//     deviceApiKey = line.substring(7);
//     deviceApiKey.trim();
//     saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);
//     btPrintln("Device API Key diset." + String(deviceApiKey.length() ? "" : " (kosong)"));
//     showConfig();
//     maybeFinalizeProvisioningAndRestart("(APIKEY:)");
//     maybeAutoDisableBluetooth("key set");
//     return;
//   }

//   if (line == "RESET_WIFI") {
//     clearWifiPrefs();
//     wifiSsid = "";
//     wifiPassword = "";
//     waitingForPassword = false;
//     btPrintln("WiFi config dihapus. Kirim SSID lagi.");
//     return;
//   }

//   if (line == "RESET_ALL") {
//     btPrintln("Semua config dihapus. Restarting...");
//     performResetAll();
//     return;
//   }

//   if (line == "SHOW_CONFIG") {
//     showConfig();
//     return;
//   }

//   // JSON provisioning (recommended)
//   if (line.startsWith("{") && line.endsWith("}")) {
//     if (parseAndApplyJsonConfig(line)) {
//       // New flow: once SSID + API Key exist, persist and restart.
//       maybeFinalizeProvisioningAndRestart("(JSON)");

//       // If not provisioned yet, optionally try WiFi when SSID present.
//       if (wifiSsid.length()) {
//         btPrintln("Mencoba konek WiFi...");
//         if (connectWiFiWithCurrentConfig()) {
//           btPrintln("WiFi terhubung. IP: " + WiFi.localIP().toString());
//         } else {
//           btPrintln("Gagal konek WiFi. Cek SSID/password.");
//         }
//       } else {
//         btPrintln("JSON diterima, tapi ssid belum ada.");
//       }
//       return;
//     }

//     btPrintln("JSON tidak valid.");
//     return;
//   }

//   // Prefix-based
//   if (line.startsWith("SSID:")) {
//     wifiSsid = line.substring(5);
//     wifiSsid.trim();
//     waitingForPassword = true;
//     btPrintln("SSID diterima: " + wifiSsid);
//     btPrintln("Kirim Password (PASS:<pw> atau plain text):");
//     // Save SSID early (password may follow, optional)
//     saveWifiToPrefs(wifiSsid, wifiPassword);
//     maybeFinalizeProvisioningAndRestart("(SSID:)");
//     return;
//   }

//   if (line.startsWith("PASS:")) {
//     wifiPassword = line.substring(5);
//     wifiPassword.trim();

//     btPrintln("Password diterima.");
//     btPrintln("Mencoba konek WiFi...");

//     if (connectWiFiWithCurrentConfig()) {
//       saveWifiToPrefs(wifiSsid, wifiPassword);
//       btPrintln("WiFi terhubung. IP: " + WiFi.localIP().toString());
//     } else {
//       btPrintln("Gagal konek WiFi. Kirim SSID lagi.");
//       wifiSsid = "";
//       wifiPassword = "";
//       waitingForPassword = false;
//     }

//     maybeFinalizeProvisioningAndRestart("(PASS:)");
//     return;
//   }

//   // Legacy flow (bluetoothtest.ino): first line ssid, second line password
//   if (!wifiSsid.length() && !waitingForPassword) {
//     wifiSsid = line;
//     waitingForPassword = true;
//     btPrintln("SSID diterima: " + wifiSsid);
//     btPrintln("Kirim Password WiFi:");
//     saveWifiToPrefs(wifiSsid, wifiPassword);
//     maybeFinalizeProvisioningAndRestart("(legacy SSID)");
//     return;
//   }

//   if (waitingForPassword && !wifiPassword.length()) {
//     wifiPassword = line;

//     btPrintln("Password diterima.");
//     btPrintln("Mencoba konek WiFi...");

//     if (connectWiFiWithCurrentConfig()) {
//       saveWifiToPrefs(wifiSsid, wifiPassword);
//       btPrintln("WiFi terhubung. IP: " + WiFi.localIP().toString());
//     } else {
//       btPrintln("Gagal konek WiFi. Kirim SSID lagi:");
//       wifiSsid = "";
//       wifiPassword = "";
//       waitingForPassword = false;
//     }

//     maybeFinalizeProvisioningAndRestart("(legacy PASS)");
//     return;
//   }

//   // If WiFi already configured, we can accept runtime commands
//   btPrintln("Command tidak dikenal: " + line);
// }

// void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
//   (void)param;
//   if (event == ESP_SPP_SRV_OPEN_EVT) {
//     Serial.println("[BT] Client Connected");
//     delay(100);
//     SerialBT.println("=== MOGI ESP32 Setup ===");
//     SerialBT.println("Kirim JSON atau SSID lalu Password.");
//     SerialBT.println("Command: SHOW_CONFIG | RESET_WIFI | RESET_ALL | KEY:<key>");
//   }
// }

// void setup() {
//   Serial.begin(115200);
//   delay(200);

//   // Relay
//   pinMode(RELAY_PIN, OUTPUT);
//   digitalWrite(RELAY_PIN, LOW);

//   // L298N direction pins
//   pinMode(EN1, OUTPUT);
//   pinMode(EN2, OUTPUT);
//   digitalWrite(EN1, HIGH);
//   digitalWrite(EN2, LOW);

//   // PWM ESP32 core 2.x style: setup channel, attach pin, then write duty on channel
//   ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
//   ledcAttachPin(ENA, PWM_CHANNEL);
//   ledcWrite(PWM_CHANNEL, 0);

//   // Sensors
//   ds18b20.begin();
//   dht.begin();

//   // Load config
//   loadConfigFromPrefs();
//   if (!apiBaseUrl.length()) apiBaseUrl = DEFAULT_API_BASE_URL;
//   if (!deviceCode.length()) deviceCode = DEFAULT_DEVICE_CODE;

//   const bool provisioned = isProvisioned();

//   // Bluetooth start ONLY when not provisioned.
//   btDeviceName = deviceCode.length() ? deviceCode : String(BT_DEVICE_NAME_FALLBACK);
//   if (!provisioned) {
//     SerialBT.register_callback(btCallback);
//     SerialBT.begin(btDeviceName.c_str());
//     SerialBT.setTimeout(5000);
//     bluetoothEnabled = true;
//     Serial.println("[BT] Started (provisioning): " + btDeviceName);
//   } else {
//     bluetoothEnabled = false;
//     Serial.println("[BT] OFF (already provisioned)");
//   }

//   showConfig();

//   // After boot, BT should stay OFF when provisioned; then connect WiFi.
//   if (wifiSsid.length()) {
//     connectWiFiWithCurrentConfig();
//   } else {
//     Serial.println("[WiFi] Belum ada SSID. Pair Bluetooth untuk set.");
//   }

//   Serial.println("[SYS] Ready");
// }

// void loop() {
//   // Bluetooth input (only when enabled)
//   if (bluetoothEnabled) {
//     // Robust: buffer until '\n' to avoid partial JSON
//     while (SerialBT.available()) {
//       char c = (char)SerialBT.read();
//       if (c == '\r') continue;
//       if (c == '\n') {
//         if (btRxBuffer.length()) {
//           handleBluetoothLine(btRxBuffer);
//           btRxBuffer = "";
//         }
//         continue;
//       }

//       btRxBuffer += c;
//       // Safety: prevent runaway buffer if sender never sends newline
//       if (btRxBuffer.length() > 1024) {
//         btRxBuffer = "";
//         btPrintln("⚠️ BT buffer overflow, input cleared.");
//       }
//     }
//   }

//   // Reconnect WiFi if lost
//   if (wifiConnectedOnce && WiFi.status() != WL_CONNECTED) {
//     static unsigned long lastReconnect = 0;
//     if (millis() - lastReconnect > 5000) {
//       lastReconnect = millis();
//       Serial.println("[WiFi] Disconnected, reconnecting...");
//       connectWiFiWithCurrentConfig(12000);
//     }
//   }

//   // Periodic sensor read (always) + send (only if WiFi connected)
//   const unsigned long now = millis();
//   if (now - lastSendMs >= sendIntervalMs) {
//     lastSendMs = now;

//     // Relay toggle
//     relayState = !relayState;
//     digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

//     // PWM increment (cycle 0-255)
//     pwmValue += 50;
//     if (pwmValue > 255) pwmValue = 0;
//     ledcWrite(PWM_CHANNEL, pwmValue);

//     // DS18B20 temperature
//     ds18b20.requestTemperatures();
//     float tempDs = ds18b20.getTempCByIndex(0);

//     // DHT22
//     float hum = dht.readHumidity();
//     float tempDht = dht.readTemperature();

//     bool tempOk = !(tempDs == DEVICE_DISCONNECTED_C || tempDs < -50 || tempDs > 100);
//     float tempToSend = tempOk ? tempDs : tempDht;

//     if (!tempOk) {
//       Serial.println("[SENSOR] DS18B20 invalid, fallback to DHT temp.");
//     }

//     bool tempValid = !isnan(tempToSend);
//     if (!tempValid) {
//       Serial.println("[SENSOR] Temperature invalid (DS & DHT).");
//     }

//     float humToSend = hum;
//     if (isnan(humToSend)) {
//       Serial.println("[SENSOR] Humidity invalid, using fallback.");
//       humToSend = HUMIDITY_FALLBACK;
//     }

//     // Always print to Serial (even without WiFi) - avoid String allocations
//     Serial.println("\n===== SENSOR & ACTUATOR =====");
//     Serial.printf("Relay       : %s\n", relayState ? "ON" : "OFF");
//     Serial.printf("PWM Value   : %d\n", pwmValue);
//     Serial.printf("Temp DS18B20: %.2f C\n", (double)tempDs);
//     Serial.printf("Temp DHT22  : %.2f C\n", (double)tempDht);
//     Serial.printf("Humidity    : %.2f %%\n", (double)hum);
//     Serial.printf("Send Temp   : %.2f\n", (double)tempToSend);
//     Serial.printf("Send Hum    : %.2f\n", (double)humToSend);
//     Serial.printf(
//       "WiFi Status : %s\n",
//       (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected"
//     );
//     Serial.println("==============================");

//     // Only send to API if WiFi connected and temp valid
//     if (WiFi.status() == WL_CONNECTED && tempValid) {
//       sendTemperatureToApi(tempToSend, humToSend);
//     } else if (!tempValid) {
//       Serial.println("[API] Skip send: invalid temperature.");
//     } else {
//       Serial.println("[API] Skip send: WiFi not connected.");
//     }
//   }

//   // Optional: forward USB Serial -> BT
//   if (Serial.available()) {
//     String out = Serial.readString();
//     if (bluetoothEnabled) {
//       SerialBT.print(out);
//     }
//   }
// }

#include <Arduino.h>
#include <BluetoothSerial.h>
#include <esp32-hal-bt.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_heap_caps.h"

#include <OneWire.h>
#include <DallasTemperature.h>
#include <DHT.h>
#include <ArduinoJson.h>

/*
  MOGI Combined Sketch
  - Bluetooth (SPP) provisioning: terima SSID+Password (opsional juga API config)
  - Connect WiFi
  - Read sensors: DS18B20 (temperature) + DHT22 (humidity + temp fallback)
  - POST ke backend: /api/arduino/temperature pakai Device API Key

  Kompatibel dengan flow lama bluetoothtest.ino:
  - Kirim SSID (line 1)
  - Kirim Password (line 2)

  Flow baru yang direkomendasikan:
  - Kirim JSON satu baris via Bluetooth, contoh:
    {"ssid":"TelU-IOT","password":"TUKJ#2024.","deviceCode":"MOGI-003","deviceApiKey":"mogi_MOGI-003_xxx","apiBaseUrl":"https://mogi-backend-firebase.vercel.app"}

  Command tambahan:
  - RESET_WIFI        -> hapus WiFi tersimpan
  - SHOW_CONFIG       -> tampilkan config ringkas
*/

/* =================== DEVICE/BACKEND DEFAULTS =================== */
static const char* DEFAULT_API_BASE_URL = "https://mogi-backend-firebase.vercel.app";
static const char* DEFAULT_DEVICE_CODE = "MOGI-006";

// API Key akan dikirim otomatis dari aplikasi setelah user register device.
// Biarkan kosong — device akan menerima key via Bluetooth provisioning.
static const char* DEFAULT_DEVICE_API_KEY = "";

static const uint32_t DEFAULT_SEND_INTERVAL_MS = 20000; // 30 detik
static const float HUMIDITY_FALLBACK = 75.0f; // fallback jika DHT gagal

// If true, Bluetooth will be fully stopped once WiFi is connected and the device key is set.
// NOTE: Keep this false if you want the device to stay always discoverable for "forget/reset" flows.
// Memory is still protected by temporarily pausing BT during TLS handshake when no client is connected.
static const bool AUTO_DISABLE_BT_WHEN_READY = false;

/* =================== BLUETOOTH =================== */
BluetoothSerial SerialBT;
static const char* BT_DEVICE_NAME_FALLBACK = "MOGI_ESP32";
String btDeviceName;
static bool bluetoothEnabled = true;

static String btRxBuffer;

// Forward declaration (used by TLS/BT helpers)
void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param);

/* =================== HTTPS/TLS (MEMORY) =================== */
static WiFiClientSecure tlsClient;
static bool tlsClientInit = false;

static uint32_t largestFreeBlock() {
  return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

static void disableBluetoothNow(const char* reason);
static void maybeAutoDisableBluetooth(const char* reason);

struct BaseUrlParts {
  bool https;
  String host;
  uint16_t port;
};

static bool parseBaseUrl(const String& baseUrl, BaseUrlParts& out);

static bool getCachedBaseUrlParts(const String& baseUrl, BaseUrlParts& out) {
  static String cached;
  static BaseUrlParts cachedParts;
  static bool cachedOk = false;

  if (cached != baseUrl) {
    cached = baseUrl;
    cachedOk = parseBaseUrl(cached, cachedParts);
  }
  if (!cachedOk) return false;
  out = cachedParts;
  return true;
}

static bool parseBaseUrl(const String& baseUrl, BaseUrlParts& out) {
  String s = baseUrl;
  s.trim();
  if (!s.length()) return false;

  bool https = true;
  if (s.startsWith("https://")) {
    https = true;
    s = s.substring(8);
  } else if (s.startsWith("http://")) {
    https = false;
    s = s.substring(7);
  }

  int slash = s.indexOf('/');
  if (slash >= 0) s = s.substring(0, slash);

  uint16_t port = https ? 443 : 80;
  int colon = s.indexOf(':');
  if (colon >= 0) {
    String hostPart = s.substring(0, colon);
    String portPart = s.substring(colon + 1);
    portPart.trim();
    int p = portPart.toInt();
    if (p > 0 && p <= 65535) port = (uint16_t)p;
    s = hostPart;
  }

  s.trim();
  if (!s.length()) return false;
  out.https = https;
  out.host = s;
  out.port = port;
  return true;
}

static int readHttpStatusCode(Stream& stream) {
  // Read first line: HTTP/1.1 200 OK
  char line[96];
  size_t idx = 0;
  unsigned long start = millis();
  while (millis() - start < 8000) {
    while (stream.available()) {
      char c = (char)stream.read();
      if (c == '\r') continue;
      if (c == '\n') {
        line[idx] = 0;
        // Parse status code
        // Expected: "HTTP/1.1 200"
        const char* sp1 = strchr(line, ' ');
        if (!sp1) return -1;
        while (*sp1 == ' ') sp1++;
        return atoi(sp1);
      }
      if (idx < sizeof(line) - 1) {
        line[idx++] = c;
      }
    }
    delay(10);
  }
  return -1;
}

static void drainHttpHeaders(Stream& stream) {
  // Consume headers until blank line
  int newlines = 0;
  unsigned long start = millis();
  while (millis() - start < 8000) {
    while (stream.available()) {
      char c = (char)stream.read();
      if (c == '\r') continue;
      if (c == '\n') {
        newlines++;
        if (newlines >= 2) return;
      } else {
        newlines = 0;
      }
    }
    delay(5);
  }
}

static bool pauseBluetoothIfIdleForTls() {
  if (!bluetoothEnabled) return false;

  // Only pause when no client is connected; we don't want to drop an active provisioning session.
  if (SerialBT.hasClient()) return false;

  Serial.println("[BT] Pausing Bluetooth to free heap for TLS...");
  SerialBT.end();
  btStop();
  delay(120);
  Serial.printf("[MEM] Free heap after BT pause: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[MEM] Largest free block after BT pause: %u bytes\n", largestFreeBlock());
  return true;
}

static void resumeBluetoothAfterTlsIfPaused(bool wasPaused) {
  if (!wasPaused) return;
  Serial.println("[BT] Resuming Bluetooth...");
  btStart();
  delay(80);
  SerialBT.register_callback(btCallback);
  SerialBT.begin(btDeviceName.c_str());
  delay(80);
}

/* =================== PREFERENCES (NVS) =================== */
Preferences prefs;
static const char* PREF_NAMESPACE = "mogi";

/* =================== PINS =================== */
#define ONE_WIRE_BUS 33
#define DHTPIN 18
#define DHTTYPE DHT22

// Relay & Motor (L298N)
#define RELAY_PIN 32
#define EN1 25
#define EN2 26
#define ENA 27  // PWM pin

/* =================== PWM CONFIG =================== */
#define PWM_FREQ     1000
#define PWM_RES      8
#define PWM_CHANNEL  0

/* =================== SENSORS =================== */
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);
DHT dht(DHTPIN, DHTTYPE);

/* =================== RUNTIME CONFIG =================== */
String wifiSsid;
String wifiPassword;

String apiBaseUrl;
String deviceCode;
String deviceApiKey;

uint32_t sendIntervalMs = DEFAULT_SEND_INTERVAL_MS;

bool wifiConnectedOnce = false;
unsigned long lastSendMs = 0;

/* =================== RELAY & MOTOR STATE =================== */
bool relayState = false;
int pwmValue = 0;

/* =================== BLUETOOTH LEGACY STATE =================== */
bool waitingForPassword = false;

/* =================== BT MEMORY HELPERS =================== */
static void disableBluetoothNow(const char* reason) {
  if (!bluetoothEnabled) return;
  Serial.print("[BT] Disabling Bluetooth (memory save). Reason: ");
  Serial.println(reason ? reason : "-");
  SerialBT.end();
  btStop();
  bluetoothEnabled = false;
  btRxBuffer = "";
  delay(120);
  Serial.printf("[MEM] Free heap after BT disable: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[MEM] Largest free block after BT disable: %u bytes\n", largestFreeBlock());
}

static void maybeAutoDisableBluetooth(const char* reason) {
  if (!AUTO_DISABLE_BT_WHEN_READY) return;
  if (!bluetoothEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!deviceApiKey.length()) return;
  if (SerialBT.hasClient()) return;
  disableBluetoothNow(reason);
}

/* =================== HELPERS =================== */
static void btPrintln(const String& s) {
  if (bluetoothEnabled && SerialBT.hasClient()) {
    SerialBT.println(s);
  }
  Serial.print("[BT] ");
  Serial.println(s);
}

static void disableBluetoothForTlsIfNeeded() {
  if (bluetoothEnabled && !SerialBT.hasClient()) {
    SerialBT.end();
    btStop();
    delay(200);
  }
}


static void clearAllPrefs() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.remove("api");
  prefs.remove("code");
  prefs.remove("key");
  prefs.remove("interval");
  prefs.end();
}

static void loadConfigFromPrefs() {
  prefs.begin(PREF_NAMESPACE, true);

  wifiSsid = prefs.getString("ssid", "");
  wifiPassword = prefs.getString("pass", "");

  apiBaseUrl = prefs.getString("api", DEFAULT_API_BASE_URL);
  deviceCode = prefs.getString("code", DEFAULT_DEVICE_CODE);
  deviceApiKey = prefs.getString("key", DEFAULT_DEVICE_API_KEY);

  sendIntervalMs = prefs.getUInt("interval", DEFAULT_SEND_INTERVAL_MS);

  prefs.end();
}

static void saveWifiToPrefs(const String& ssid, const String& pass) {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

static void saveApiConfigToPrefs(const String& baseUrl, const String& code, const String& key, uint32_t intervalMs) {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.putString("api", baseUrl);
  prefs.putString("code", code);
  prefs.putString("key", key);
  prefs.putUInt("interval", intervalMs);
  prefs.end();
}

static void clearWifiPrefs() {
  prefs.begin(PREF_NAMESPACE, false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

static bool isProvisioned() {
  // Provisioned means we have at least WiFi SSID and Device API Key.
  // WiFi password is optional (open networks supported).
  return wifiSsid.length() && deviceApiKey.length();
}

static void showConfig() {
  Serial.println("\n===== MOGI CONFIG =====");
  Serial.println("WiFi SSID   : " + (wifiSsid.length() ? wifiSsid : String("<empty>")));
  Serial.println("WiFi Pass   : " + String(wifiPassword.length() ? "<set>" : "<empty>"));
  Serial.println("API BaseURL : " + apiBaseUrl);
  Serial.println("Device Code : " + deviceCode);
  Serial.println("Device Key  : " + String(deviceApiKey.length() ? "<set>" : "<empty>"));
  Serial.println("Interval ms : " + String(sendIntervalMs));
  Serial.println("=======================\n");

  btPrintln("WiFi SSID: " + (wifiSsid.length() ? wifiSsid : String("<empty>")));
  btPrintln("API: " + apiBaseUrl);
  btPrintln("Device: " + deviceCode);
  btPrintln("Key: " + String(deviceApiKey.length() ? "<set>" : "<empty>"));
  btPrintln("Interval(ms): " + String(sendIntervalMs));
}

static bool connectWiFiWithCurrentConfig(uint32_t timeoutMs = 15000) {
  if (!wifiSsid.length()) {
    Serial.println("[WiFi] SSID kosong. Provision via Bluetooth dulu.");
    return false;
  }

  Serial.println("\n[WiFi] Connecting...");
  Serial.println("[WiFi] SSID: " + wifiSsid);

  WiFi.mode(WIFI_STA);
  if (wifiPassword.length()) {
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  } else {
    // Support open networks / no password provisioning
    WiFi.begin(wifiSsid.c_str());
  }

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    wifiConnectedOnce = true;

    // If provisioning already finished, we can disable Bluetooth to save heap.
    maybeAutoDisableBluetooth("WiFi connected");
    return true;
  }

  Serial.println("\n[WiFi] Failed");
  return false;
}

static void performResetAll() {
  Serial.println("\n🔄 RESET ALL triggered! Clearing all config and restarting...");
  clearAllPrefs();
  wifiSsid = "";
  wifiPassword = "";
  apiBaseUrl = DEFAULT_API_BASE_URL;
  deviceCode = DEFAULT_DEVICE_CODE;
  deviceApiKey = DEFAULT_DEVICE_API_KEY;
  sendIntervalMs = DEFAULT_SEND_INTERVAL_MS;
  waitingForPassword = false;
  delay(300);
  ESP.restart();
}

static void sendTemperatureToApi(float temperatureC, float humidity) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected. Skipping API call.");
    return;
  }

  // If device is already configured and BT is idle, keep BT off for TLS stability.
  maybeAutoDisableBluetooth("before TLS");

  const char* useDeviceCode = deviceCode.length() ? deviceCode.c_str() : DEFAULT_DEVICE_CODE;

  // Cek apakah API key sudah di-provision dari HP
  if (!deviceApiKey.length()) {
    Serial.println("⏳ Device API Key belum di-set. Register device dari aplikasi dulu.");
    return;
  }
  const char* useDeviceApiKey = deviceApiKey.c_str();

  if (!useDeviceCode || !*useDeviceCode) {
    Serial.println("❌ deviceCode kosong.");
    return;
  }

  Serial.println("\n📡 Sending data to API...");
  Serial.printf("[MEM] Free heap: %u bytes\n", ESP.getFreeHeap());
  Serial.printf("[MEM] Largest free block: %u bytes\n", largestFreeBlock());

  BaseUrlParts base;
  if (!getCachedBaseUrlParts(apiBaseUrl, base)) {
    Serial.println("❌ apiBaseUrl invalid.");
    return;
  }

  static const char* PATH = "/api/arduino/temperature";
  Serial.print("URL: ");
  Serial.print(base.https ? "https://" : "http://");
  Serial.print(base.host);
  Serial.println(PATH);

  // Free memory for TLS handshake (only if BT is idle)
  const bool btPaused = pauseBluetoothIfIdleForTls();

  // Create compact JSON payload without ArduinoJson/String to minimize heap usage.
  // Backend expects: { temperature: number, humidity: number }
  // Keep keys exactly the same.
  // Force finite numbers to ensure backend validation passes.
  float t = temperatureC;
  float h = humidity;
  if (!isfinite(t)) t = -999.0f;
  if (!isfinite(h)) h = -999.0f;

  // Keep a reasonable precision to reduce payload size.
  // DS18B20 resolution is 0.0625°C, so 2 decimals is enough.
  char jsonPayload[96];
  const int payloadLen = snprintf(
    jsonPayload,
    sizeof(jsonPayload),
    "{\"temperature\":%.2f,\"humidity\":%.2f}",
    (double)t,
    (double)h
  );
  if (payloadLen <= 0 || payloadLen >= (int)sizeof(jsonPayload)) {
    Serial.println("❌ Payload build failed (buffer too small).");
    resumeBluetoothAfterTlsIfPaused(btPaused);
    return;
  }

  Serial.print("Payload: ");
  Serial.println(jsonPayload);

  int statusCode = -1;

  if (base.https) {
    // TLS client init (minimize RAM usage)
    if (!tlsClientInit) {
      tlsClient.setInsecure();
      tlsClient.setHandshakeTimeout(10);
      tlsClient.setTimeout(15000);
      tlsClientInit = true;
    }

    tlsClient.stop();
    delay(60);

    Serial.printf("[TLS] Connecting to %s:%u ...\n", base.host.c_str(), base.port);
    if (!tlsClient.connect(base.host.c_str(), base.port)) {
      Serial.println("❌ TLS connect failed.");
      char errBuf[96];
      tlsClient.lastError(errBuf, sizeof(errBuf));
      Serial.print("[TLS] lastError: ");
      Serial.println(errBuf);
      statusCode = -1;
    } else {
      // Minimal HTTP/1.0 request to reduce memory pressure
      tlsClient.print("POST ");
      tlsClient.print(PATH);
      tlsClient.print(" HTTP/1.0\r\n");
      tlsClient.print("Host: ");
      tlsClient.print(base.host);
      tlsClient.print("\r\n");
      tlsClient.print("Content-Type: application/json\r\n");
      tlsClient.print("X-Device-Code: ");
      tlsClient.print(useDeviceCode);
      tlsClient.print("\r\n");
      tlsClient.print("X-Device-Key: ");
      tlsClient.print(useDeviceApiKey);
      tlsClient.print("\r\n");
      tlsClient.print("Content-Length: ");
      tlsClient.print(payloadLen);
      tlsClient.print("\r\n");
      tlsClient.print("Connection: close\r\n\r\n");
      tlsClient.write((const uint8_t*)jsonPayload, (size_t)payloadLen);

      statusCode = readHttpStatusCode(tlsClient);
      drainHttpHeaders(tlsClient);
      tlsClient.stop();
    }
  } else {
    WiFiClient plain;
    plain.setTimeout(15000);
    Serial.printf("[HTTP] Connecting to %s:%u ...\n", base.host.c_str(), base.port);
    if (!plain.connect(base.host.c_str(), base.port)) {
      Serial.println("❌ HTTP connect failed.");
      statusCode = -1;
    } else {
      plain.print("POST ");
      plain.print(PATH);
      plain.print(" HTTP/1.0\r\n");
      plain.print("Host: ");
      plain.print(base.host);
      plain.print("\r\n");
      plain.print("Content-Type: application/json\r\n");
      plain.print("X-Device-Code: ");
      plain.print(useDeviceCode);
      plain.print("\r\n");
      plain.print("X-Device-Key: ");
      plain.print(useDeviceApiKey);
      plain.print("\r\n");
      plain.print("Content-Length: ");
      plain.print(payloadLen);
      plain.print("\r\n");
      plain.print("Connection: close\r\n\r\n");
      plain.write((const uint8_t*)jsonPayload, (size_t)payloadLen);

      statusCode = readHttpStatusCode(plain);
      drainHttpHeaders(plain);
      plain.stop();
    }
  }

  if (statusCode > 0) {
    Serial.print("✅ Response Code: ");
    Serial.println(statusCode);
    if (statusCode == 200 || statusCode == 201) {
      Serial.println("✅ Data sent successfully!");
    } else if (statusCode == 401) {
      Serial.println("🚫 401 Unauthorized! API key invalid. Performing RESET ALL...");
      resumeBluetoothAfterTlsIfPaused(btPaused);
      performResetAll();
      return; // won't reach here (ESP restarts), but just in case
    } else {
      Serial.println("⚠️ Server returned non-success code");
    }
  } else {
    Serial.println("❌ Request failed (no HTTP status).");
  }

  resumeBluetoothAfterTlsIfPaused(btPaused);
  Serial.println("================================\n");
}

static int findJsonKeyPos(const String& json, const char* key) {
  String pat = String('"') + key + '"';
  return json.indexOf(pat);
}

static bool jsonHasKey(const String& json, const char* key) {
  return findJsonKeyPos(json, key) >= 0;
}

static String jsonGetStringValue(const String& json, const char* key) {
  int keyPos = findJsonKeyPos(json, key);
  if (keyPos < 0) return "";
  int colon = json.indexOf(':', keyPos);
  if (colon < 0) return "";

  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
  if (i >= (int)json.length()) return "";

  if (json[i] != '"') return ""; // only support quoted string for safety
  i++; // after opening quote
  int end = json.indexOf('"', i);
  if (end < 0) return "";
  return json.substring(i, end);
}

static bool jsonGetUIntValue(const String& json, const char* key, uint32_t& out) {
  int keyPos = findJsonKeyPos(json, key);
  if (keyPos < 0) return false;
  int colon = json.indexOf(':', keyPos);
  if (colon < 0) return false;

  int i = colon + 1;
  while (i < (int)json.length() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) i++;
  if (i >= (int)json.length()) return false;

  int j = i;
  while (j < (int)json.length() && json[j] >= '0' && json[j] <= '9') j++;
  if (j == i) return false;

  out = (uint32_t)json.substring(i, j).toInt();
  return true;
}

static bool parseAndApplyJsonConfig(const String& jsonLine) {
  // Lightweight JSON parser: supports keys with quoted string values and sendIntervalMs as number.
  // Example:
  // {"ssid":"...","password":"...","deviceCode":"MOGI-003","deviceApiKey":"mogi_...","apiBaseUrl":"https://...","sendIntervalMs":30000}
  bool any = false;
  const bool keyAlreadySet = deviceApiKey.length() > 0;

  // Apply fields if key exists in JSON, even if value is empty.
  // Ini penting supaya value lama di NVS bisa di-clear dari HP.
  if (jsonHasKey(jsonLine, "ssid")) {
    wifiSsid = jsonGetStringValue(jsonLine, "ssid");
    any = true;
  }
  if (jsonHasKey(jsonLine, "password")) {
    wifiPassword = jsonGetStringValue(jsonLine, "password");
    any = true;
  }
  if (jsonHasKey(jsonLine, "apiBaseUrl")) {
    apiBaseUrl = jsonGetStringValue(jsonLine, "apiBaseUrl");
    any = true;
  }
  if (jsonHasKey(jsonLine, "deviceCode")) {
    if (keyAlreadySet) {
      btPrintln("Device sudah provisioned. deviceCode diabaikan (sudah terkunci). Gunakan RESET_ALL untuk ganti.");
    } else {
      deviceCode = jsonGetStringValue(jsonLine, "deviceCode");
      any = true;
    }
  }
  if (jsonHasKey(jsonLine, "deviceApiKey")) {
    if (keyAlreadySet) {
      btPrintln("Device API Key sudah tersimpan. Abaikan update key (terkunci). Gunakan RESET_ALL untuk ganti.");
    } else {
      deviceApiKey = jsonGetStringValue(jsonLine, "deviceApiKey");
      any = true;
    }
  }
  // Aliases (support multiple app/backends)
  if (!jsonHasKey(jsonLine, "deviceApiKey") && jsonHasKey(jsonLine, "deviceKey")) {
    if (keyAlreadySet) {
      btPrintln("Device API Key sudah tersimpan. Abaikan update key (terkunci). Gunakan RESET_ALL untuk ganti.");
    } else {
      deviceApiKey = jsonGetStringValue(jsonLine, "deviceKey");
      any = true;
    }
  }
  if (!jsonHasKey(jsonLine, "deviceApiKey") && !jsonHasKey(jsonLine, "deviceKey") && jsonHasKey(jsonLine, "apiKey")) {
    if (keyAlreadySet) {
      btPrintln("Device API Key sudah tersimpan. Abaikan update key (terkunci). Gunakan RESET_ALL untuk ganti.");
    } else {
      deviceApiKey = jsonGetStringValue(jsonLine, "apiKey");
      any = true;
    }
  }

  uint32_t interval;
  if (jsonGetUIntValue(jsonLine, "sendIntervalMs", interval)) {
    sendIntervalMs = interval;
    if (sendIntervalMs < 3000) sendIntervalMs = 3000;
    any = true;
  }

  if (!any) return false;

  btPrintln("JSON config diterima.");

  // Save API config immediately (boleh kosong sebagian).
  saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);

  // If WiFi is already connected and key is set, free BT heap permanently.
  maybeAutoDisableBluetooth("provisioned");

  // Save WiFi immediately if SSID present (password optional).
  if (wifiSsid.length()) {
    saveWifiToPrefs(wifiSsid, wifiPassword);
  }

  showConfig();
  return true;
}

static void maybeFinalizeProvisioningAndRestart(const char* reason) {
  if (!isProvisioned()) return;

  // Persist current WiFi (password optional) + API config then restart.
  saveWifiToPrefs(wifiSsid, wifiPassword);
  saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);

  btPrintln(String("Provisioning lengkap (SSID + API Key). Restarting... ") + (reason ? reason : ""));
  delay(300);
  ESP.restart();
}

static void handleBluetoothLine(String line) {
  line.trim();
  if (!line.length()) return;
  btPrintln("Received via BT: " + line);

  // Quick set device key without JSON
  if (line.startsWith("KEY:")) {
    if (deviceApiKey.length()) {
      btPrintln("Device API Key sudah tersimpan. Abaikan set key ulang (terkunci). Gunakan RESET_ALL untuk ganti.");
      showConfig();
      maybeAutoDisableBluetooth("key already set");
      return;
    }
    deviceApiKey = line.substring(4);
    deviceApiKey.trim();
    saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);
    btPrintln("Device API Key diset." + String(deviceApiKey.length() ? "" : " (kosong)"));
    showConfig();
    maybeFinalizeProvisioningAndRestart("(KEY:)");
    maybeAutoDisableBluetooth("key set");
    return;
  }

  if (line.startsWith("APIKEY:")) {
    if (deviceApiKey.length()) {
      btPrintln("Device API Key sudah tersimpan. Abaikan set key ulang (terkunci). Gunakan RESET_ALL untuk ganti.");
      showConfig();
      maybeAutoDisableBluetooth("key already set");
      return;
    }
    deviceApiKey = line.substring(7);
    deviceApiKey.trim();
    saveApiConfigToPrefs(apiBaseUrl, deviceCode, deviceApiKey, sendIntervalMs);
    btPrintln("Device API Key diset." + String(deviceApiKey.length() ? "" : " (kosong)"));
    showConfig();
    maybeFinalizeProvisioningAndRestart("(APIKEY:)");
    maybeAutoDisableBluetooth("key set");
    return;
  }

  if (line == "RESET_WIFI") {
    clearWifiPrefs();
    wifiSsid = "";
    wifiPassword = "";
    waitingForPassword = false;
    btPrintln("WiFi config dihapus. Kirim SSID lagi.");
    return;
  }

  if (line == "RESET_ALL") {
    btPrintln("Semua config dihapus. Restarting...");
    performResetAll();
    return;
  }

  if (line == "SHOW_CONFIG") {
    showConfig();
    return;
  }

  // JSON provisioning (recommended)
  if (line.startsWith("{") && line.endsWith("}")) {
    if (parseAndApplyJsonConfig(line)) {
      // New flow: once SSID + API Key exist, persist and restart.
      maybeFinalizeProvisioningAndRestart("(JSON)");

      // If not provisioned yet, optionally try WiFi when SSID present.
      if (wifiSsid.length()) {
        btPrintln("Mencoba konek WiFi...");
        if (connectWiFiWithCurrentConfig()) {
          btPrintln("WiFi terhubung. IP: " + WiFi.localIP().toString());
        } else {
          btPrintln("Gagal konek WiFi. Cek SSID/password.");
        }
      } else {
        btPrintln("JSON diterima, tapi ssid belum ada.");
      }
      return;
    }

    btPrintln("JSON tidak valid.");
    return;
  }

  // Prefix-based
  if (line.startsWith("SSID:")) {
    wifiSsid = line.substring(5);
    wifiSsid.trim();
    waitingForPassword = true;
    btPrintln("SSID diterima: " + wifiSsid);
    btPrintln("Kirim Password (PASS:<pw> atau plain text):");
    // Save SSID early (password may follow, optional)
    saveWifiToPrefs(wifiSsid, wifiPassword);
    maybeFinalizeProvisioningAndRestart("(SSID:)");
    return;
  }

  if (line.startsWith("PASS:")) {
    wifiPassword = line.substring(5);
    wifiPassword.trim();

    btPrintln("Password diterima.");
    btPrintln("Mencoba konek WiFi...");

    if (connectWiFiWithCurrentConfig()) {
      saveWifiToPrefs(wifiSsid, wifiPassword);
      btPrintln("WiFi terhubung. IP: " + WiFi.localIP().toString());
    } else {
      btPrintln("Gagal konek WiFi. Kirim SSID lagi.");
      wifiSsid = "";
      wifiPassword = "";
      waitingForPassword = false;
    }

    maybeFinalizeProvisioningAndRestart("(PASS:)");
    return;
  }

  // Legacy flow (bluetoothtest.ino): first line ssid, second line password
  if (!wifiSsid.length() && !waitingForPassword) {
    wifiSsid = line;
    waitingForPassword = true;
    btPrintln("SSID diterima: " + wifiSsid);
    btPrintln("Kirim Password WiFi:");
    saveWifiToPrefs(wifiSsid, wifiPassword);
    maybeFinalizeProvisioningAndRestart("(legacy SSID)");
    return;
  }

  if (waitingForPassword && !wifiPassword.length()) {
    wifiPassword = line;

    btPrintln("Password diterima.");
    btPrintln("Mencoba konek WiFi...");

    if (connectWiFiWithCurrentConfig()) {
      saveWifiToPrefs(wifiSsid, wifiPassword);
      btPrintln("WiFi terhubung. IP: " + WiFi.localIP().toString());
    } else {
      btPrintln("Gagal konek WiFi. Kirim SSID lagi:");
      wifiSsid = "";
      wifiPassword = "";
      waitingForPassword = false;
    }

    maybeFinalizeProvisioningAndRestart("(legacy PASS)");
    return;
  }

  // If WiFi already configured, we can accept runtime commands
  btPrintln("Command tidak dikenal: " + line);
}

void btCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t* param) {
  (void)param;
  if (event == ESP_SPP_SRV_OPEN_EVT) {
    Serial.println("[BT] Client Connected");
    delay(100);
    SerialBT.println("=== MOGI ESP32 Setup ===");
    SerialBT.println("Kirim JSON atau SSID lalu Password.");
    SerialBT.println("Command: SHOW_CONFIG | RESET_WIFI | RESET_ALL | KEY:<key>");
  }
}

/* ===== PUMP INTERVAL MODE (NO NTP) ===== */
unsigned long lastPumpMs = 0;
const unsigned long pumpInterval = 8UL * 60UL * 60UL * 1000UL; // 8 jam
const unsigned long pumpDuration = 10000; // 10 detik

bool pumpRunning = false;
unsigned long pumpStartMs = 0;

/* =================== SETUP & LOOP =================== */
#define FAN_NORMAL 120   // kecepatan normal (0-255)
#define FAN_MAX    255   // maksimum

void setup() {
  Serial.begin(115200);
  delay(200);

  // Relay
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // ===== PUMP SELF-TEST (2 detik saat boot) =====
  Serial.println("\n[PUMP TEST] Menjalankan pompa selama 2 detik...");
  digitalWrite(RELAY_PIN, HIGH);
  delay(2000);
  digitalWrite(RELAY_PIN, LOW);
  Serial.println("[PUMP TEST] Pompa test selesai. Jika pompa menyala sebentar, berarti OK.");

  // L298N direction pins
  pinMode(EN1, OUTPUT);
  pinMode(EN2, OUTPUT);
  digitalWrite(EN1, HIGH);
  digitalWrite(EN2, LOW);

  // PWM ESP32 core 2.x style: setup channel, attach pin, then write duty on channel
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);

  // Sensors
  ds18b20.begin();
  dht.begin();

  // Load config
  loadConfigFromPrefs();
  if (!apiBaseUrl.length()) apiBaseUrl = DEFAULT_API_BASE_URL;
  if (!deviceCode.length()) deviceCode = DEFAULT_DEVICE_CODE;

  const bool provisioned = isProvisioned();

  // Bluetooth start ONLY when not provisioned.
  btDeviceName = deviceCode.length() ? deviceCode : String(BT_DEVICE_NAME_FALLBACK);
  if (!provisioned) {
    SerialBT.register_callback(btCallback);
    SerialBT.begin(btDeviceName.c_str());
    SerialBT.setTimeout(5000);
    bluetoothEnabled = true;
    Serial.println("[BT] Started (provisioning): " + btDeviceName);
  } else {
    bluetoothEnabled = false;
    Serial.println("[BT] OFF (already provisioned)");
  }

  showConfig();

  // After boot, BT should stay OFF when provisioned; then connect WiFi.
  if (wifiSsid.length()) {
    connectWiFiWithCurrentConfig();
  } else {
    Serial.println("[WiFi] Belum ada SSID. Pair Bluetooth untuk set.");
  }

  Serial.println("[SYS] Ready");
}

void loop() {

  // ===== PUMP INTERVAL SCHEDULER =====
unsigned long now = millis();

// start pump tiap 8 jam
if (!pumpRunning && (now - lastPumpMs >= pumpInterval)) {
  digitalWrite(RELAY_PIN, HIGH);
  pumpRunning = true;
  pumpStartMs = now;
  lastPumpMs = now;

  Serial.println("[PUMP] ON");
}

// stop pump setelah durasi
if (pumpRunning && (now - pumpStartMs >= pumpDuration)) {
  digitalWrite(RELAY_PIN, LOW);
  pumpRunning = false;

  Serial.println("[PUMP] OFF");
}

  // Bluetooth input (only when enabled)
  if (bluetoothEnabled) {
    // Robust: buffer until '\n' to avoid partial JSON
    while (SerialBT.available()) {
      char c = (char)SerialBT.read();
      if (c == '\r') continue;
      if (c == '\n') {
        if (btRxBuffer.length()) {
          handleBluetoothLine(btRxBuffer);
          btRxBuffer = "";
        }
        continue;
      }

      btRxBuffer += c;
      // Safety: prevent runaway buffer if sender never sends newline
      if (btRxBuffer.length() > 1024) {
        btRxBuffer = "";
        btPrintln("⚠️ BT buffer overflow, input cleared.");
      }
    }
  }

  // Reconnect WiFi if lost
  if (wifiConnectedOnce && WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 5000) {
      lastReconnect = millis();
      Serial.println("[WiFi] Disconnected, reconnecting...");
      connectWiFiWithCurrentConfig(12000);
    }
  }

    // Periodic sensor read (always) + send (only if WiFi connected)
    now = millis();
    if (now - lastSendMs >= sendIntervalMs) {
    lastSendMs = now;

        // DS18B20 temperature
    ds18b20.requestTemperatures();
    float tempDs = ds18b20.getTempCByIndex(0);
    float tempC = tempDs;

    // Relay toggle
    relayState = !relayState;
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

    // ===== AUTO FAN CONTROL =====
    if (tempC >= 29) {
    pwmValue = FAN_MAX;
    }
    else if (tempC >= 24 && tempC <= 28) {
    pwmValue = FAN_NORMAL;
    }
    ledcWrite(ENA, pwmValue);



    // DHT22
    float hum = dht.readHumidity();
    float tempDht = dht.readTemperature();

    bool tempOk = !(tempDs == DEVICE_DISCONNECTED_C || tempDs < -50 || tempDs > 100);
    float tempToSend = tempOk ? tempDs : tempDht;

    if (!tempOk) {
      Serial.println("[SENSOR] DS18B20 invalid, fallback to DHT temp.");
    }

    bool tempValid = !isnan(tempToSend);
    if (!tempValid) {
      Serial.println("[SENSOR] Temperature invalid (DS & DHT).");
    }

    float humToSend = hum;
    if (isnan(humToSend)) {
      Serial.println("[SENSOR] Humidity invalid, using fallback.");
      humToSend = HUMIDITY_FALLBACK;
    }

    // Always print to Serial (even without WiFi) - avoid String allocations
    Serial.println("\n===== SENSOR & ACTUATOR =====");
    Serial.printf("Relay       : %s\n", relayState ? "ON" : "OFF");
    Serial.printf("PWM Value   : %d\n", pwmValue);
    Serial.printf("Temp DS18B20: %.2f C\n", (double)tempDs);
    Serial.printf("Temp DHT22  : %.2f C\n", (double)tempDht);
    Serial.printf("Humidity    : %.2f %%\n", (double)hum);
    Serial.printf("Send Temp   : %.2f\n", (double)tempToSend);
    Serial.printf("Send Hum    : %.2f\n", (double)humToSend);
    Serial.printf(
      "WiFi Status : %s\n",
      (WiFi.status() == WL_CONNECTED) ? "Connected" : "Disconnected"
    );
    Serial.println("==============================");

    // Only send to API if WiFi connected and temp valid
    if (WiFi.status() == WL_CONNECTED && tempValid) {
      sendTemperatureToApi(tempToSend, humToSend);
    } else if (!tempValid) {
      Serial.println("[API] Skip send: invalid temperature.");
    } else {
      Serial.println("[API] Skip send: WiFi not connected.");
    }
  }

  // Optional: forward USB Serial -> BT
  if (Serial.available()) {
    String out = Serial.readString();
    if (bluetoothEnabled) {
      SerialBT.print(out);
    }
  }
}