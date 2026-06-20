#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>

// =================================================================
// KONFIGURASI SERVER CASAOS
// =================================================================
const char* ws_host = "smart-relay.ijuloss.my.id";
const int ws_port = 443;
const bool use_ssl = true;

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

// KOEFISIEN FILTER DSP EMA (Exponential Moving Average)
// Meredam noise pembacaan sensor gas MQ-series yang fluktuatif
const float ALPHA = 0.20;
float filteredMQ2 = 0.0, filteredMQ3 = 0.0, filteredMQ5 = 0.0, filteredTGS = 0.0;
bool filterInit = false;

String pompa1Status = "OFF", pompa2Status = "OFF";

// Status proses pembacaan: "IDLE" (belum/sudah selesai), "MEMBACA" (pompa 1 aktif)
String statusBacaan = "IDLE";

// Hasil klasifikasi fuzzy logic kematangan buah
String klasifikasiBuah = "Menunggu Pembacaan...";
float skorKematangan = 0.0; // 0 (mentah) - 100 (matang penuh)

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
        klasifikasiBuah = "Membaca...";
        filterInit = false; // Mulai sesi baru: filter EMA diinisialisasi ulang
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
// FUZZY LOGIC MAMDANI - KLASIFIKASI KEMATANGAN BUAH
// =================================================================
// Catatan kalibrasi (contoh awal, sesuaikan dengan data eksperimen Anda):
//   - MQ3 (alkohol/etanol): naik signifikan seiring fermentasi gula saat
//     buah matang -> kontributor utama deteksi "matang".
//   - MQ2 (gas mudah terbakar umum) & MQ5 (LPG/gas alam/metana): naik
//     seiring produksi etilen & gas organik volatil saat pembusukan ->
//     indikator "matang/terlalu matang".
//   - TGS2602 (VOC/amonia/H2S): biasanya tinggi saat buah masih segar/
//     mentah karena profil VOC berbeda, lalu menurun relatif saat matang.
// Rule ini adalah TITIK AWAL yang wajar untuk prototipe; sebaiknya
// dikalibrasi ulang dengan data lapangan per jenis buah.

float membershipRendah(float x, float batasBawah, float batasAtas) {
    if (x <= batasBawah) return 1.0;
    if (x >= batasAtas) return 0.0;
    return (batasAtas - x) / (batasAtas - batasBawah);
}

float membershipTinggi(float x, float batasBawah, float batasAtas) {
    if (x <= batasBawah) return 0.0;
    if (x >= batasAtas) return 1.0;
    return (x - batasBawah) / (batasAtas - batasBawah);
}

void jalankanFuzzyLogic() {
    // Hanya baca & klasifikasikan saat Pompa 1 (penyedot bau) sedang aktif.
    // Saat idle, sensor tidak dibaca agar nilai terakhir (hasil final) tetap
    // ditampilkan apa adanya di dashboard, bukan ter-overwrite oleh udara
    // ambien sekitar chamber.
    if (statusBacaan != "MEMBACA") return;

    rawMQ2 = analogRead(MQ2_PIN);
    rawMQ3 = analogRead(MQ3_PIN);
    rawMQ5 = analogRead(MQ5_PIN);
    rawTGS = analogRead(TGS2602_PIN);

    // PROSES FILTERISASI DIGITAL (meredam noise pembacaan ADC sensor gas)
    if (!filterInit) {
        filteredMQ2 = rawMQ2; filteredMQ3 = rawMQ3;
        filteredMQ5 = rawMQ5; filteredTGS = rawTGS;
        filterInit = true;
    } else {
        filteredMQ2 = (ALPHA * rawMQ2) + ((1.0 - ALPHA) * filteredMQ2);
        filteredMQ3 = (ALPHA * rawMQ3) + ((1.0 - ALPHA) * filteredMQ3);
        filteredMQ5 = (ALPHA * rawMQ5) + ((1.0 - ALPHA) * filteredMQ5);
        filteredTGS = (ALPHA * rawTGS) + ((1.0 - ALPHA) * filteredTGS);
    }

    // ---- FUZZIFIKASI (rentang ADC 0-4095, sesuaikan via kalibrasi) ----
    float mq3Tinggi   = membershipTinggi(filteredMQ3, 800.0, 2200.0);
    float mq2Tinggi    = membershipTinggi(filteredMQ2, 1000.0, 2500.0);
    float mq5Tinggi    = membershipTinggi(filteredMQ5, 1000.0, 2500.0);
    float tgsRendah    = membershipRendah(filteredTGS, 1200.0, 2800.0);

    // ---- INFERENSI (rule wajar, bobot dapat disesuaikan) ----
    // Skor matang dipengaruhi oleh: MQ3 tinggi (40%), MQ2 tinggi (20%),
    // MQ5 tinggi (20%), dan TGS2602 rendah (20%) relatif terhadap profil mentah.
    float skor = (mq3Tinggi * 40.0) + (mq2Tinggi * 20.0) +
                 (mq5Tinggi * 20.0) + (tgsRendah * 20.0);

    skorKematangan = skor;

    // ---- DEFUZZIFIKASI (klasifikasi akhir) ----
    if (skor < 35.0) {
        klasifikasiBuah = "Mentah";
    } else if (skor < 70.0) {
        klasifikasiBuah = "Setengah Matang";
    } else {
        klasifikasiBuah = "Matang";
    }
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

    // Data mentah (untuk model Random Forest di sisi server)
    doc["mq2_raw"] = rawMQ2;
    doc["mq3_raw"] = rawMQ3;
    doc["mq5_raw"] = rawMQ5;
    doc["tgs_raw"] = rawTGS;

    // Data terfilter (untuk tampilan & fuzzy logic)
    doc["mq2"] = filteredMQ2;
    doc["mq3"] = filteredMQ3;
    doc["mq5"] = filteredMQ5;
    doc["tgs"] = filteredTGS;

    // Hasil klasifikasi fuzzy lokal
    doc["klasifikasi"] = klasifikasiBuah;
    doc["skor"] = skorKematangan;
    doc["statusBacaan"] = statusBacaan; // "IDLE" atau "MEMBACA"

    // Status pompa manual
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

    preferences.begin("fruit-app", false);

    String statIp = preferences.getString("statIp", "");
    String statGw = preferences.getString("statGw", "");
    if (statIp != "") {
        IPAddress ip, gw, sn(255, 255, 255, 0);
        ip.fromString(statIp);
        if (statGw != "") { gw.fromString(statGw); }
        else { gw = ip; gw[3] = 1; }
        wm.setSTAStaticIPConfig(ip, gw, sn);
    }

    wm.autoConnect("FruitSensor_AP");
    digitalWrite(LED_WIFI, HIGH);

    String socketIoUrl = "/socket.io/?EIO=4&transport=websocket";
    if (use_ssl) webSocket.beginSSL(ws_host, ws_port, socketIoUrl);
    else webSocket.begin(ws_host, ws_port, socketIoUrl);

    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(3000);

    delay(2000); // Stabilisasi awal sensor gas (analog dgn kode asli)
}

// =================================================================
// LOOP
// =================================================================
void loop() {
    // Penanganan Restart Tertunda Tanpa Fungsi Blocking Delay
    if (pendingRestart && (millis() - restartTime >= 1000)) {
        ESP.restart();
    }

    webSocket.loop();

    unsigned long intervalSekarang = (statusBacaan == "MEMBACA") ? readIntervalAktif : readIntervalIdle;

    if (millis() - lastReadTime >= intervalSekarang) {
        lastReadTime = millis();
        jalankanFuzzyLogic(); // tidak melakukan apa-apa jika statusBacaan != "MEMBACA"

        // Output ke Serial Plotter (tetap dipertahankan untuk debugging lokal)
        Serial.print(rawMQ2); Serial.print(",");
        Serial.print(rawMQ3); Serial.print(",");
        Serial.print(rawMQ5); Serial.print(",");
        Serial.print(rawTGS); Serial.print(",");
        Serial.print(klasifikasiBuah); Serial.print(",");
        Serial.println(skorKematangan);

        if (WiFi.status() == WL_CONNECTED) { kirimDataKeServer(); }
    }
}
