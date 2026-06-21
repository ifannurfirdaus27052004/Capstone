#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <WiFiClientSecure.h>
#include "ota_config.h"

// =================================================================
// KONFIGURASI SERVER CASAOS
// =================================================================
const int WS_HOST_MAX = 64;
char ws_host[WS_HOST_MAX];
int ws_port = default_ws_port;

// ======================
// PIN SENSOR GAS
// ======================
#define MQ2_PIN     35
#define MQ3_PIN     34
#define MQ5_PIN     32
// BUGFIX: TGS2602 dipindahkan dari Pin 25 (ADC2) ke Pin 33 (ADC1) agar kompatibel dengan Wi-Fi
#define TGS2602_PIN 33 

// ======================
// PIN RELAY
// ======================
#define RELAY1 22
#define RELAY2 23
#define LED_WIFI 2

WebSocketsClient webSocket;
Preferences preferences;
WiFiManager wm;

// =================================================================
// STATE VARIABLES & TELEMETRI
// =================================================================
int rawMQ2 = 0, rawMQ3 = 0, rawMQ5 = 0, rawTGS = 0;
bool pompa1On = false;
bool pompa2On = false;
bool isReading = false;
unsigned long lastReadTime = 0;
const unsigned long readIntervalIdle = 2000;    // saat idle, cukup kirim status tiap 2 detik
const unsigned long readIntervalAktif = 500;    // saat membaca, update live tiap 0.5 detik
bool socketIoReady = false;
bool pendingRestart = false;
unsigned long restartTime = 0;

// =================================================================
// KONTROL POMPA (MANUAL DARI DASHBOARD, SALING EXCLUSIVE)
// =================================================================
void setPompa1(bool stateON) {
    if (stateON) {
        digitalWrite(RELAY2, HIGH); // OFF (active LOW)
        pompa2On = false;
        digitalWrite(RELAY1, LOW); // ON (active LOW)
        pompa1On = true;
        isReading = true;
    } else {
        digitalWrite(RELAY1, HIGH); // OFF
        pompa1On = false;
        isReading = false;
    }
}

void setPompa2(bool stateON) {
    if (stateON) {
        digitalWrite(RELAY1, HIGH); // OFF (active LOW)
        pompa1On = false;
        isReading = false;
        digitalWrite(RELAY2, LOW); // ON (active LOW)
        pompa2On = true;
    } else {
        digitalWrite(RELAY2, HIGH); // OFF
        pompa2On = false;
    }
}

void semuaPompaOff() {
    setPompa1(false);
    setPompa2(false);
}

// =================================================================
// SENSOR RAW READ ONLY
// =================================================================
void bacaSensor() {
    rawMQ2 = analogRead(MQ2_PIN);
    rawMQ3 = analogRead(MQ3_PIN);
    rawMQ5 = analogRead(MQ5_PIN);
    rawTGS = analogRead(TGS2602_PIN);
}

// =================================================================
// OTA & RESTART
// =================================================================
void triggerRestart() {
    pendingRestart = true;
    restartTime = millis();
}

void setupArduinoOTA() {
    ArduinoOTA.setHostname("fruitmaturity-esp32");
    // BUGFIX: Menambahkan kata sandi untuk mencegah akses pembaruan tanpa izin
    ArduinoOTA.setPassword("admin123"); 
    
    ArduinoOTA.onStart([]() { Serial.println("OTA: Start"); });
    ArduinoOTA.onEnd([]() { Serial.println("\nOTA: End"); });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("OTA: Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });
    ArduinoOTA.begin();
    Serial.println("OTA: Ready");
}

void setupWebSocketSSL() {
    Serial.println("[SSL] Configuring SSL for WebSocket (certificate validation: disabled for testing)");
}

// =================================================================
// WEBSOCKET / SOCKET.IO EVENT HANDLER
// =================================================================
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_DISCONNECTED) {
        socketIoReady = false;
        Serial.println("[WS] Disconnected");
        return;
    }
    if (type == WStype_CONNECTED) {
        Serial.printf("[WS] Connected to %s:%d\n", ws_host, ws_port);
        return;
    }
    if (type == WStype_ERROR) {
        Serial.printf("[WS] ERROR: %s\n", payload ? (char*)payload : "unknown");
        return;
    }
    if (type != WStype_TEXT || length == 0) return;

    String msg = String((char*)payload);

    if (msg.startsWith("0")) {
        webSocket.sendTXT("40");
        socketIoReady = true;
        Serial.println("[WS] ✓ Socket.IO handshake completed");
        return;
    }
    if (msg == "2") {
        webSocket.sendTXT("3");
        return;
    }
    if (!msg.startsWith("42")) return;

    msg.remove(0, 2); 
    JsonDocument doc;
    if (deserializeJson(doc, msg)) {
        Serial.printf("[WS] JSON parse failed!\n");
        return;
    }
    
    if (doc[0].as<String>() != "serverToEsp") return;
    
    String cmd = doc[1]["cmd"].as<String>();
    if (cmd == "setPompa1") {
        bool state = doc[1]["data"]["state"] | false;
        setPompa1(state);
    }
    else if (cmd == "setPompa2") {
        bool state = doc[1]["data"]["state"] | false;
        setPompa2(state);
    }
    else if (cmd == "setAllOff") {
        semuaPompaOff();
    }
    else if (cmd == "applyNetwork") {
        String ip = doc[1]["data"]["ip"].as<String>();
        String gw = doc[1]["data"]["gw"].as<String>();
        if (ip.length() > 0) {
            preferences.putString("statIp", ip);
            preferences.putString("statGw", gw);
            triggerRestart();
        }
    }
    else if (cmd == "resetWifi") {
        wm.resetSettings();
        preferences.remove("statIp");
        triggerRestart();
    }
}

// =================================================================
// KIRIM DATA TELEMETRI KE SERVER
// =================================================================
void kirimDataKeServer() {
    if (!socketIoReady) return;

    JsonDocument doc;
    doc["mq2_raw"] = rawMQ2;
    doc["mq3_raw"] = rawMQ3;
    doc["mq5_raw"] = rawMQ5;
    doc["tgs_raw"] = rawTGS;
    doc["statusBacaan"] = isReading ? "MEMBACA" : "IDLE";
    doc["pompa1"] = pompa1On ? "ON" : "OFF";
    doc["pompa2"] = pompa2On ? "ON" : "OFF";

    String jsonData;
    serializeJson(doc, jsonData);
    String frame = "42[\"espData\"," + jsonData + "]";
    webSocket.sendTXT(frame);
}

// =================================================================
// SETUP
// =================================================================
void setup() {
    Serial.begin(115200);
    pinMode(RELAY1, OUTPUT);
    pinMode(RELAY2, OUTPUT);
    digitalWrite(RELAY1, HIGH);
    digitalWrite(RELAY2, HIGH);
    pinMode(LED_WIFI, OUTPUT);
    digitalWrite(LED_WIFI, HIGH); 

    preferences.begin("fruit-app", false);
    String savedHost = preferences.getString("ws_host", default_ws_host);
    strncpy(ws_host, savedHost.c_str(), WS_HOST_MAX);
    ws_host[WS_HOST_MAX - 1] = '\0';
    ws_port = preferences.getInt("ws_port", default_ws_port);

    String statIp = preferences.getString("statIp", "");
    String statGw = preferences.getString("statGw", "");

    if (statIp != "") {
        IPAddress ip, gw, sn(255, 255, 255, 0);
        ip.fromString(statIp);
        if (statGw != "") { gw.fromString(statGw); }
        else { gw = ip; gw[3] = 1; }
        wm.setSTAStaticIPConfig(ip, gw, sn);
    }

    char portBuf[6];
    snprintf(portBuf, sizeof(portBuf), "%d", ws_port);
    WiFiManagerParameter otaHostParam("host", "OTA Host", ws_host, sizeof(ws_host));
    WiFiManagerParameter otaPortParam("port", "OTA Port", portBuf, sizeof(portBuf));
    wm.addParameter(&otaHostParam);
    wm.addParameter(&otaPortParam);
    wm.autoConnect("FruitSensor_AP");

    digitalWrite(LED_WIFI, HIGH);
    String newHost = otaHostParam.getValue();
    int newPort = atoi(otaPortParam.getValue());

    if (newHost.length() > 0) {
        strncpy(ws_host, newHost.c_str(), WS_HOST_MAX);
        ws_host[WS_HOST_MAX - 1] = '\0';
        preferences.putString("ws_host", ws_host);
    }
    if (newPort > 0 && newPort != ws_port) {
        ws_port = newPort;
        preferences.putInt("ws_port", ws_port);
    }

    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    const char* socketIoUrl = "/socket.io/?EIO=4&transport=websocket";

    Serial.printf("[WS] Connecting to %s:%d%s\n", ws_host, ws_port, socketIoUrl);

    // BUGFIX: Menangani koneksi SSL dan Fallback secara mulus
    if (ws_port == 443 || String(ws_host).indexOf("ijuloss.my.id") >= 0) {
        Serial.println("[WS] Menggunakan koneksi SSL/HTTPS");
        setupWebSocketSSL();
        webSocket.beginSSL(ws_host, ws_port, socketIoUrl);
    } else {
        Serial.println("[WS] Menggunakan koneksi HTTP biasa");
        webSocket.begin(ws_host, ws_port, socketIoUrl);
    }

    setupArduinoOTA();
    delay(2000); 
}

void updateWifiLed() {
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_WIFI, LOW); 
    } else {
        digitalWrite(LED_WIFI, HIGH); 
    }
}

// =================================================================
// LOOP
// =================================================================
void loop() {
    if (pendingRestart && (millis() - restartTime >= 1000)) {
        ESP.restart();
    }

    webSocket.loop();
    ArduinoOTA.handle();
    updateWifiLed();

    unsigned long intervalSekarang = isReading ? readIntervalAktif : readIntervalIdle;
    if (millis() - lastReadTime >= intervalSekarang) {
        lastReadTime = millis();
        bacaSensor();

        const char* status = isReading ? "MEMBACA" : "IDLE";
        Serial.print(rawMQ2); Serial.print(",");
        Serial.print(rawMQ3); Serial.print(",");
        Serial.print(rawMQ5); Serial.print(",");
        Serial.print(rawTGS); Serial.print(",");
        Serial.println(status);

        if (WiFi.status() == WL_CONNECTED) {
            kirimDataKeServer();
        }
    }
}