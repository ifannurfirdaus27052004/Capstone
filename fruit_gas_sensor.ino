#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "ota_config.h"

// =================================================================
// KONFIGURASI SERVER CASAOS
// =================================================================
String ws_host = String(default_ws_host);
int ws_port = default_ws_port;
bool use_ssl = default_use_ssl;

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

String pompa1Status = "OFF", pompa2Status = "OFF";

// Status proses pembacaan: "IDLE" (belum/sudah selesai), "MEMBACA" (pompa 1 aktif)
String statusBacaan = "IDLE";

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
        pompa2Status = "OFF";

        digitalWrite(RELAY1, LOW); // ON (active LOW)
        pompa1Status = "ON";

        statusBacaan = "MEMBACA";
    } else {
        digitalWrite(RELAY1, HIGH); // OFF
        pompa1Status = "OFF";

        // Pembacaan berhenti, hasil klasifikasi terakhir tetap ditampilkan
        statusBacaan = "IDLE";
    }
}

void setPompa2(bool stateON) {
    if (stateON) {
        // Pastikan pompa 1 mati dulu sebelum pompa 2 menyala
        digitalWrite(RELAY1, HIGH); // OFF (active LOW)
        pompa1Status = "OFF";
        statusBacaan = "IDLE";

        digitalWrite(RELAY2, LOW); // ON (active LOW)
        pompa2Status = "ON";
    } else {
        digitalWrite(RELAY2, HIGH); // OFF
        pompa2Status = "OFF";
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
void jalankanOTA() {
    Serial.println("\n[OTA] Mengunduh Firmware...");
    String otaURL = String(use_ssl ? "https://" : "http://") + ws_host + ":" + ws_port + "/firmware.bin";
    t_httpUpdate_return ret;

    if (use_ssl) {
        WiFiClientSecure clientS; clientS.setInsecure();
        ret = httpUpdate.update(clientS, otaURL);
    } else {
        WiFiClient client; ret = httpUpdate.update(client, otaURL);
    }
    if (ret == HTTP_UPDATE_OK) {
        pendingRestart = true;
        restartTime = millis();
    }
}

void triggerRestart() {
    pendingRestart = true;
    restartTime = millis();
}

// =================================================================
// WEBSOCKET / SOCKET.IO EVENT HANDLER
// =================================================================
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_DISCONNECTED) { socketIoReady = false; }
    else if (type == WStype_TEXT) {
        String msg = (char*)payload;
        if (msg.startsWith("0")) { webSocket.sendTXT("40"); socketIoReady = true; return; }
        if (msg == "2") { webSocket.sendTXT("3"); return; }

        if (msg.startsWith("42")) {
            msg.remove(0, 2); JsonDocument doc;
            if (!deserializeJson(doc, msg)) {
                if (doc[0].as<String>() == "serverToEsp") {
                    String cmd = doc[1]["cmd"].as<String>();

                    if (cmd == "setPompa1") {
                        bool state = doc[1]["data"]["state"].as<bool>();
                        setPompa1(state);
                    }
                    else if (cmd == "setPompa2") {
                        bool state = doc[1]["data"]["state"].as<bool>();
                        setPompa2(state);
                    }
                    else if (cmd == "setAllOff") {
                        semuaPompaOff();
                    }
                    else if (cmd == "applyNetwork") {
                        preferences.putString("statIp", doc[1]["data"]["ip"].as<String>());
                        preferences.putString("statGw", doc[1]["data"]["gw"].as<String>());
                        Serial.println("[NETWORK] Konfigurasi Baru Disimpan. Memicu Restart...");
                        triggerRestart();
                    }
                    else if (cmd == "startOta") { jalankanOTA(); }
                    else if (cmd == "resetWifi") {
                        wm.resetSettings();
                        preferences.remove("statIp");
                        Serial.println("[NETWORK] Memori Jaringan Dibersihkan. Memicu Restart...");
                        triggerRestart();
                    }
                }
            }
        }
    }
}

// =================================================================
// KIRIM DATA TELEMETRI KE SERVER
// =================================================================
void kirimDataKeServer() {
    if (!socketIoReady) return;
    JsonDocument doc;

    // Data mentah dikirim ke server untuk diproses fuzzy logic di sisi server.
    doc["mq2_raw"] = rawMQ2;
    doc["mq3_raw"] = rawMQ3;
    doc["mq5_raw"] = rawMQ5;
    doc["tgs_raw"] = rawTGS;

    // Status pembacaan & kontrol pompa
    doc["statusBacaan"] = statusBacaan; // "IDLE" atau "MEMBACA"
    doc["pompa1"] = pompa1Status;
    doc["pompa2"] = pompa2Status;

    String jsonData; serializeJson(doc, jsonData);
    webSocket.sendTXT("42[\"espData\"," + jsonData + "]");
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

    ws_host = preferences.getString("ws_host", ws_host);
    ws_port = preferences.getInt("ws_port", ws_port);
    use_ssl = preferences.getBool("use_ssl", use_ssl);

    String statIp = preferences.getString("statIp", "");
    String statGw = preferences.getString("statGw", "");
    if (statIp != "") {
        IPAddress ip, gw, sn(255, 255, 255, 0);
        ip.fromString(statIp);
        if (statGw != "") { gw.fromString(statGw); }
        else { gw = ip; gw[3] = 1; }
        wm.setSTAStaticIPConfig(ip, gw, sn);
    }

    char hostBuf[64];
    char portBuf[6];
    char sslBuf[2];
    ws_host.toCharArray(hostBuf, sizeof(hostBuf));
    snprintf(portBuf, sizeof(portBuf), "%d", ws_port);
    snprintf(sslBuf, sizeof(sslBuf), "%d", use_ssl ? 1 : 0);

    WiFiManagerParameter otaHostParam("host", "OTA Host", hostBuf, sizeof(hostBuf));
    WiFiManagerParameter otaPortParam("port", "OTA Port", portBuf, sizeof(portBuf));
    WiFiManagerParameter otaSslParam("ssl", "Use SSL (0=off,1=on)", sslBuf, sizeof(sslBuf));
    wm.addParameter(&otaHostParam);
    wm.addParameter(&otaPortParam);
    wm.addParameter(&otaSslParam);

    wm.autoConnect("FruitSensor_AP");
    digitalWrite(LED_WIFI, HIGH);

    String newHost = otaHostParam.getValue();
    int newPort = atoi(otaPortParam.getValue());
    bool newSsl = otaSslParam.getValue()[0] == '1';

    if (newHost.length() > 0 && newHost != ws_host) {
        ws_host = newHost;
        preferences.putString("ws_host", ws_host);
    }
    if (newPort > 0 && newPort != ws_port) {
        ws_port = newPort;
        preferences.putInt("ws_port", ws_port);
    }
    if (newSsl != use_ssl) {
        use_ssl = newSsl;
        preferences.putBool("use_ssl", use_ssl);
    }

    String socketIoUrl = "/socket.io/?EIO=4&transport=websocket";
    if (use_ssl) webSocket.beginSSL(ws_host.c_str(), ws_port, socketIoUrl.c_str());
    else webSocket.begin(ws_host.c_str(), ws_port, socketIoUrl.c_str());

    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);

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

    unsigned long intervalSekarang = (statusBacaan == "MEMBACA") ? readIntervalAktif : readIntervalIdle;

    if (millis() - lastReadTime >= intervalSekarang) {
        lastReadTime = millis();
        bacaSensor();

        // Output ke Serial Plotter (tetap dipertahankan untuk debugging lokal)
        Serial.print(rawMQ2); Serial.print(",");
        Serial.print(rawMQ3); Serial.print(",");
        Serial.print(rawMQ5); Serial.print(",");
        Serial.print(rawTGS); Serial.print(",");
        Serial.println(statusBacaan);

        if (WiFi.status() == WL_CONNECTED) { kirimDataKeServer(); }
    }
}
