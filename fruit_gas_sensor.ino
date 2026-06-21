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
const unsigned long readIntervalIdle = 2000;
const unsigned long readIntervalAktif = 500;
bool pendingRestart = false;
unsigned long restartTime = 0;

// =================================================================
// KONTROL POMPA (NON-BLOCKING)
// =================================================================
void setPompa1(bool stateON) {
    if (stateON) {
        digitalWrite(RELAY2, HIGH); 
        pompa2On = false;
        digitalWrite(RELAY1, LOW); 
        pompa1On = true;
        isReading = true;
    } else {
        digitalWrite(RELAY1, HIGH); 
        pompa1On = false;
        isReading = false;
    }
}

void setPompa2(bool stateON) {
    if (stateON) {
        digitalWrite(RELAY1, HIGH); 
        pompa1On = false;
        isReading = false;
        digitalWrite(RELAY2, LOW); 
        pompa2On = true;
    } else {
        digitalWrite(RELAY2, HIGH); 
        pompa2On = false;
    }
}

void semuaPompaOff() {
    setPompa1(false);
    setPompa2(false);
}

// =================================================================
// SENSOR RAW READ
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
    ArduinoOTA.setPassword("admin123"); 
    ArduinoOTA.begin();
}

void setupWebSocketSSL() {
    Serial.println("[SSL] Configuring SSL for WebSocket...");
}

// =================================================================
// KIRIM DATA TELEMETRI KE SERVER
// =================================================================
void kirimDataKeServer() {
    DynamicJsonDocument doc(512);
    doc["mq2_raw"] = rawMQ2;
    doc["mq3_raw"] = rawMQ3;
    doc["mq5_raw"] = rawMQ5;
    doc["tgs_raw"] = rawTGS;
    doc["statusBacaan"] = isReading ? "MEMBACA" : "IDLE";
    doc["pompa1"] = pompa1On ? "ON" : "OFF";
    doc["pompa2"] = pompa2On ? "ON" : "OFF";
    
    // PENAMBAHAN SYSTEM INFO
    doc["wifi_ssid"] = WiFi.SSID();
    doc["ip_address"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["uptime_sec"] = millis() / 1000;
    doc["free_heap"] = ESP.getFreeHeap();

    String jsonData;
    serializeJson(doc, jsonData);
    String frame = "42[\"espData\"," + jsonData + "]";
    webSocket.sendTXT(frame);
}

// =================================================================
// WEBSOCKET EVENT HANDLER (INSTANT FEEDBACK MENGHILANGKAN DELAY)
// =================================================================
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_DISCONNECTED || type == WStype_CONNECTED) return;
    if (type != WStype_TEXT || length == 0) return;

    String msg = String((char*)payload);
    if (msg.startsWith("0")) { webSocket.sendTXT("40"); return; }
    if (msg == "2") { webSocket.sendTXT("3"); return; }
    if (!msg.startsWith("42")) return;

    msg.remove(0, 2); 
    DynamicJsonDocument doc(512);
    if (deserializeJson(doc, msg)) return;
    if (doc[0].as<String>() != "serverToEsp") return;
    
    String cmd = doc[1]["cmd"].as<String>();
    
    // Proses instruksi dan langsung kirim data balik (Bypass interval delay UI)
    if (cmd == "setPompa1") {
        setPompa1(doc[1]["data"]["state"] | false);
        kirimDataKeServer(); 
    }
    else if (cmd == "setPompa2") {
        setPompa2(doc[1]["data"]["state"] | false);
        kirimDataKeServer();
    }
    else if (cmd == "setAllOff") {
        semuaPompaOff();
        kirimDataKeServer();
    }
    else if (cmd == "reboot") {
        triggerRestart();
    }
    else if (cmd == "resetWifi") {
        wm.resetSettings();
        preferences.remove("statIp");
        triggerRestart();
    }
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

    wm.autoConnect("FruitSensor_AP");

    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    const char* socketIoUrl = "/socket.io/?EIO=4&transport=websocket";

    if (ws_port == 443 || String(ws_host).indexOf("ijuloss.my.id") >= 0) {
        setupWebSocketSSL();
        webSocket.beginSSL(ws_host, ws_port, socketIoUrl);
    } else {
        webSocket.begin(ws_host, ws_port, socketIoUrl);
    }

    setupArduinoOTA();
}

void updateWifiLed() {
    if (WiFi.status() == WL_CONNECTED) digitalWrite(LED_WIFI, LOW); 
    else digitalWrite(LED_WIFI, HIGH); 
}

// =================================================================
// LOOP
// =================================================================
void loop() {
    if (pendingRestart && (millis() - restartTime >= 1000)) ESP.restart();

    webSocket.loop();
    ArduinoOTA.handle();
    updateWifiLed();

    // Gunakan interval cepat jika Pompa 1 ATAU Pompa 2 menyala agar pergerakan angka di web sama-sama responsif
    unsigned long intervalSekarang = (pompa1On || pompa2On) ? readIntervalAktif : readIntervalIdle;
    if (millis() - lastReadTime >= intervalSekarang) {
        lastReadTime = millis();
        bacaSensor();
        if (WiFi.status() == WL_CONNECTED) {
            kirimDataKeServer();
        }
    }
}