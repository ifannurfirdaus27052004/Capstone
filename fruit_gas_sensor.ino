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
#define TGS2602_PIN 25

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
// Pompa 1 = sedot bau buah ke chamber sensor (mulai pembacaan)
// Pompa 2 = buang/purge bau dari chamber (bersihkan chamber)
// Keduanya tidak boleh menyala bersamaan.

void setPompa1(bool stateON) {
    if (stateON) {
        // Pastikan pompa 2 mati dulu sebelum pompa 1 menyala
        digitalWrite(RELAY2, HIGH); // OFF (active LOW)
        pompa2On = false;

        digitalWrite(RELAY1, LOW); // ON (active LOW)
        pompa1On = true;

        isReading = true;
    } else {
        digitalWrite(RELAY1, HIGH); // OFF
        pompa1On = false;

        // Pembacaan berhenti, hasil klasifikasi terakhir tetap ditampilkan
        isReading = false;
    }
}

void setPompa2(bool stateON) {
    if (stateON) {
        // Pastikan pompa 1 mati dulu sebelum pompa 2 menyala
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
// SENSOR RAW READ ONLY - ESP32 hanya mengirim data ke server
// =================================================================
// Data fuzzy logic kini diproses sepenuhnya oleh server, sehingga ESP32
// bertugas hanya membaca sensor dan mengirim nilai mentah.

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

    ArduinoOTA.onStart([]() {
        Serial.println("OTA: Start");
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nOTA: End");
    });
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

// =================================================================
// WEBSOCKET / SOCKET.IO EVENT HANDLER (menggunakan ArduinoJson)
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
    if (type != WStype_TEXT || length == 0) return;

    String msg = String((char*)payload);
    Serial.printf("[WS] RX (%d bytes): %s\n", length, msg.c_str());

    // Socket.IO Handshake: "0" → respond "40"
    if (msg.startsWith("0")) {
        webSocket.sendTXT("40");
        socketIoReady = true;
        Serial.println("[WS] Socket.IO handshake completed");
        return;
    }

    // Socket.IO Keepalive: "2" → respond "3"
    if (msg == "2") {
        webSocket.sendTXT("3");
        return;
    }

    // Socket.IO Event frame: "42" prefix = event message
    if (!msg.startsWith("42")) return;

    msg.remove(0, 2);  // Remove "42" prefix
    Serial.printf("[WS] After remove prefix: %s\n", msg.c_str());

    JsonDocument doc;
    if (deserializeJson(doc, msg)) {
        Serial.println("[WS] JSON parse failed!");
        return;
    }

    // Check event name: serverToEsp
    if (doc[0].as<String>() != "serverToEsp") {
        Serial.printf("[WS] Event bukan serverToEsp: %s\n", doc[0].as<String>().c_str());
        return;
    }

    // doc[1] contains the command payload
    String cmd = doc[1]["cmd"].as<String>();
    Serial.printf("[WS] CMD: %s\n", cmd.c_str());

    if (cmd == "setPompa1") {
        bool state = doc[1]["data"]["state"] | false;
        setPompa1(state);
        Serial.printf("[WS] setPompa1 → %s\n", state ? "ON" : "OFF");
    }
    else if (cmd == "setPompa2") {
        bool state = doc[1]["data"]["state"] | false;
        setPompa2(state);
        Serial.printf("[WS] setPompa2 → %s\n", state ? "ON" : "OFF");
    }
    else if (cmd == "setAllOff") {
        semuaPompaOff();
        Serial.println("[WS] setAllOff executed");
    }
    else if (cmd == "applyNetwork") {
        String ip = doc[1]["data"]["ip"].as<String>();
        String gw = doc[1]["data"]["gw"].as<String>();
        if (ip.length() > 0) {
            preferences.putString("statIp", ip);
            preferences.putString("statGw", gw);
            Serial.printf("[NETWORK] Konfigurasi Baru Disimpan: IP=%s GW=%s. Memicu Restart...\n", ip.c_str(), gw.c_str());
            triggerRestart();
        }
    }
    else if (cmd == "resetWifi") {
        wm.resetSettings();
        preferences.remove("statIp");
        Serial.println("[NETWORK] Memori Jaringan Dibersihkan. Memicu Restart...");
        triggerRestart();
    }
}

// =================================================================
// KIRIM DATA TELEMETRI KE SERVER (menggunakan ArduinoJson)
// =================================================================
void kirimDataKeServer() {
    if (!socketIoReady) {
        Serial.println("[WS] Socket.IO not ready, skip send");
        return;
    }

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
    
    Serial.printf("[WS] Sent espData: %s\n", jsonData.c_str());
}

// =================================================================
// SETUP
// =================================================================
void setup() {
    Serial.begin(115200);

    pinMode(RELAY1, OUTPUT);
    pinMode(RELAY2, OUTPUT);
    digitalWrite(RELAY1, HIGH); // OFF (active LOW)
    digitalWrite(RELAY2, HIGH); // OFF (active LOW)

    pinMode(LED_WIFI, OUTPUT);
    digitalWrite(LED_WIFI, HIGH); // Matikan LED saat belum terhubung

    preferences.begin("fruit-app", false);

    String savedHost = preferences.getString("ws_host", default_ws_host);
    strncpy(ws_host, savedHost.c_str(), WS_HOST_MAX);
    ws_host[WS_HOST_MAX - 1] = '\0';
    ws_port = preferences.getInt("ws_port", ws_port);

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

    const char* socketIoUrl = "/socket.io/?EIO=4&transport=websocket";
    Serial.printf("[WS] Connecting to %s:%d%s\n", ws_host, ws_port, socketIoUrl);
    webSocket.begin(ws_host, ws_port, socketIoUrl);

    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);
    setupArduinoOTA();

    delay(2000); // Stabilisasi awal sensor gas (analog dgn kode asli)
}

// =================================================================
// LOOP
// =================================================================
void updateWifiLed() {
    if (WiFi.status() == WL_CONNECTED) {
        digitalWrite(LED_WIFI, LOW); // LED onboard biru menyala
    } else {
        digitalWrite(LED_WIFI, HIGH); // LED dimatikan
    }
}

void loop() {
    // Penanganan Restart Tertunda Tanpa Fungsi Blocking Delay
    if (pendingRestart && (millis() - restartTime >= 1000)) {
        ESP.restart();
    }

    webSocket.loop();
    updateWifiLed();

    unsigned long intervalSekarang = isReading ? readIntervalAktif : readIntervalIdle;

    if (millis() - lastReadTime >= intervalSekarang) {
        lastReadTime = millis();
        bacaSensor();

        // Output ke Serial Plotter (tetap dipertahankan untuk debugging lokal)
        const char* status = isReading ? "MEMBACA" : "IDLE";
        Serial.print(rawMQ2); Serial.print(",");
        Serial.print(rawMQ3); Serial.print(",");
        Serial.print(rawMQ5); Serial.print(",");
        Serial.print(rawTGS); Serial.print(",");
        Serial.println(status);

        if (WiFi.status() == WL_CONNECTED) { kirimDataKeServer(); }
    }

    ArduinoOTA.handle();
}
