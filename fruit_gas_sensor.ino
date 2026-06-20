#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoOTA.h>
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
// WEBSOCKET / SOCKET.IO EVENT HANDLER
// =================================================================
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
    if (type == WStype_DISCONNECTED) {
        socketIoReady = false;
        return;
    }
    if (type != WStype_TEXT || length == 0) return;

    const char* msg = (const char*)payload;
    if (msg[0] == '0') {
        webSocket.sendTXT("40");
        socketIoReady = true;
        return;
    }
    if (length == 1 && msg[0] == '2') {
        webSocket.sendTXT("3");
        return;
    }

    const char prefix[] = "42[\"serverToEsp\",\"";
    const size_t prefixLen = sizeof(prefix) - 1;
    if (length <= prefixLen) return;
    if (strncmp(msg, prefix, prefixLen) != 0) return;

    const char* cmdStart = msg + prefixLen;
    const char* cmdEnd = strstr(cmdStart, "\"]");
    if (!cmdEnd) return;

    bool setP1 = false;
    bool setP2 = false;
    bool doSetP1 = false;
    bool doSetP2 = false;
    bool doSetAllOff = false;
    bool doApplyNetwork = false;
    bool doResetWifi = false;
    char ipBuf[40] = "";
    char gwBuf[40] = "";

    const char* sep = strchr(cmdStart, '|');
    size_t cmdLen = sep ? (size_t)(sep - cmdStart) : (size_t)(cmdEnd - cmdStart);
    if (cmdLen == 0 || cmdLen > 40) return;

    char cmdBuf[41];
    memcpy(cmdBuf, cmdStart, cmdLen);
    cmdBuf[cmdLen] = '\0';

    const char* arg = sep ? sep + 1 : NULL;

    if (strcmp(cmdBuf, "setPompa1") == 0 && arg) {
        doSetP1 = true;
        setP1 = (arg[0] == '1' || (arg[0] == 't' && arg[1] == 'r'));
    } else if (strcmp(cmdBuf, "setPompa2") == 0 && arg) {
        doSetP2 = true;
        setP2 = (arg[0] == '1' || (arg[0] == 't' && arg[1] == 'r'));
    } else if (strcmp(cmdBuf, "setAllOff") == 0) {
        doSetAllOff = true;
    } else if (strcmp(cmdBuf, "applyNetwork") == 0 && arg) {
        doApplyNetwork = true;
        const char* sep2 = strchr(arg, '|');
        if (sep2) {
            size_t ipLen = sep2 - arg;
            if (ipLen < sizeof(ipBuf)) {
                memcpy(ipBuf, arg, ipLen);
                ipBuf[ipLen] = '\0';
            }
            strncpy(gwBuf, sep2 + 1, sizeof(gwBuf) - 1);
            gwBuf[sizeof(gwBuf) - 1] = '\0';
        } else {
            strncpy(ipBuf, arg, sizeof(ipBuf) - 1);
            ipBuf[sizeof(ipBuf) - 1] = '\0';
        }
    } else if (strcmp(cmdBuf, "resetWifi") == 0) {
        doResetWifi = true;
    }

    if (doSetP1) setPompa1(setP1);
    if (doSetP2) setPompa2(setP2);
    if (doSetAllOff) semuaPompaOff();
    if (doApplyNetwork) {
        preferences.putString("statIp", ipBuf);
        preferences.putString("statGw", gwBuf);
        Serial.println("[NETWORK] Konfigurasi Baru Disimpan. Memicu Restart...");
        triggerRestart();
    }
    if (doResetWifi) {
        wm.resetSettings();
        preferences.remove("statIp");
        Serial.println("[NETWORK] Memori Jaringan Dibersihkan. Memicu Restart...");
        triggerRestart();
    }
}

// =================================================================
// KIRIM DATA TELEMETRI KE SERVER
// =================================================================
void kirimDataKeServer() {
    if (!socketIoReady) return;

    const char* status = isReading ? "MEMBACA" : "IDLE";
    const char* p1 = pompa1On ? "ON" : "OFF";
    const char* p2 = pompa2On ? "ON" : "OFF";

    char payload[256];
    int len = snprintf(payload, sizeof(payload),
        "{\"mq2_raw\":%d,\"mq3_raw\":%d,\"mq5_raw\":%d,\"tgs_raw\":%d,"
        "\"statusBacaan\":\"%s\",\"pompa1\":\"%s\",\"pompa2\":\"%s\"}",
        rawMQ2, rawMQ3, rawMQ5, rawTGS, status, p1, p2);
    if (len <= 0) return;

    char message[300];
    snprintf(message, sizeof(message), "42[\"espData\",%s]", payload);
    webSocket.sendTXT(message);
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
