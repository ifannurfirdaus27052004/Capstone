const express = require("express");
const http = require("http");
const path = require("path");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: "*" } });

const PORT = process.env.PORT || 4000;
app.use(express.static(path.join(__dirname)));

// State Default (Offline / Restart)
const defaultData = {
    mq2_raw: 0, mq3_raw: 0, mq5_raw: 0, tgs_raw: 0,
    mq2: 0, mq3: 0, mq5: 0, tgs: 0,
    klasifikasi: "Menunggu Koneksi...", skor: 0.0,
    statusBacaan: "OFFLINE", pompa1: "OFF", pompa2: "OFF",
    connected: false,
    wifi_ssid: "-", ip_address: "-", rssi: 0, uptime_formatted: "0h 00m 00s", free_heap: 0
};

let lastData = { ...defaultData };
let espSocketId = null; // Menyimpan ID spesifik milik ESP32

// State untuk EMA Filter (mencegah fluktuasi nilai)
let emaSensors = { mq2: null, mq3: null, mq5: null, tgs: null };
const EMA_ALPHA = 0.15; // Faktor smoothing (semakin kecil = nilai semakin stabil namun lebih lambat merespon)

// Fungsi mengubah detik menjadi format "0h 00m 00s"
function formatUptime(seconds) {
    if (!seconds) return "0h 00m 00s";
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    return `${h}h ${m}m ${s}s`;
}

// =================================================================
// 1. FUNGSI DERAJAT KEANGGOTAAN (MEMBERSHIP)
// =================================================================
function membershipRendah(x, batasBawah, batasAtas) {
    if (x <= batasBawah) return 1.0;
    if (x >= batasAtas) return 0.0;
    return (batasAtas - x) / (batasAtas - batasBawah);
}

function membershipSedang(x, batasBawah, batasTengah, batasAtas) {
    if (x <= batasBawah || x >= batasAtas) return 0.0;
    if (x > batasBawah && x <= batasTengah) return (x - batasBawah) / (batasTengah - batasBawah);
    if (x > batasTengah && x < batasAtas) return (batasAtas - x) / (batasAtas - batasTengah);
    return 0.0;
}

function membershipTinggi(x, batasBawah, batasAtas) {
    if (x <= batasBawah) return 0.0;
    if (x >= batasAtas) return 1.0;
    return (x - batasBawah) / (batasAtas - batasBawah);
}

// =================================================================
// 2. FUNGSI LOGIKA FUZZY UTAMA (BERDASARKAN TABEL KALIBRASI)
// =================================================================
function computeFuzzyClassification(data) {
    // Memastikan kita mengambil nilai RAW yang tepat
    const mq2 = typeof data.mq2_raw === 'number' ? data.mq2_raw : data.mq2;
    const mq3 = typeof data.mq3_raw === 'number' ? data.mq3_raw : data.mq3;
    const mq5 = typeof data.mq5_raw === 'number' ? data.mq5_raw : data.mq5;
    const tgs = typeof data.tgs_raw === 'number' ? data.tgs_raw : data.tgs;

    // --- TAHAP 1: FUZZIFIKASI (Menerjemahkan angka RAW ke Derajat Fuzzy) ---
    // Batas nilai diekstrak langsung dari Tabel Data Sensor Anda

    // MQ-2 (Mentah < 1900 | Matang ~2280 | Busuk > 2600)
    let mq2_rendah = membershipRendah(mq2, 1900, 2100);
    let mq2_sedang = membershipSedang(mq2, 1900, 2280, 2600);
    let mq2_tinggi = membershipTinggi(mq2, 2400, 2600);

    // MQ-3 (Mentah < 130 | Matang ~310 | Busuk > 550)
    let mq3_rendah = membershipRendah(mq3, 130, 200);
    let mq3_sedang = membershipSedang(mq3, 130, 310, 550);
    let mq3_tinggi = membershipTinggi(mq3, 450, 550);

    // MQ-5 (Mentah < 470 | Matang ~760 | Busuk > 900)
    let mq5_rendah = membershipRendah(mq5, 470, 600);
    let mq5_sedang = membershipSedang(mq5, 470, 760, 900);
    let mq5_tinggi = membershipTinggi(mq5, 800, 900);

    // TGS2602 (Mentah < 70 | Matang ~150 | Busuk > 280)
    let tgs_rendah = membershipRendah(tgs, 70, 100);
    let tgs_sedang = membershipSedang(tgs, 70, 150, 280);
    let tgs_tinggi = membershipTinggi(tgs, 220, 280);

    // --- TAHAP 2: INFERENSI RULE BASE (Menggunakan Logika AND / Minimal) ---
    // JIKA semua sensor Rendah, MAKA Mentah
    let ruleMentah = Math.min(mq2_rendah, mq3_rendah, mq5_rendah, tgs_rendah);
    
    // JIKA semua sensor Sedang, MAKA Matang
    let ruleMatang = Math.min(mq2_sedang, mq3_sedang, mq5_sedang, tgs_sedang);
    
    // JIKA semua sensor Tinggi, MAKA Busuk
    let ruleBusuk  = Math.min(mq2_tinggi, mq3_tinggi, mq5_tinggi, tgs_tinggi);

    // --- TAHAP 3: DEFUZZIFIKASI (Mencari Keputusan Paling Dominan) ---
    let maxRule = Math.max(ruleMentah, ruleMatang, ruleBusuk);

    if (maxRule === 0) {
        data.klasifikasi = "Analisis Berjalan...";
        data.skor = 0.0;
    } else if (maxRule === ruleMentah) {
        data.klasifikasi = "Mentah";
        data.skor = parseFloat((20 + (maxRule * 20)).toFixed(1)); // Range skor 20% - 40%
    } else if (maxRule === ruleMatang) {
        data.klasifikasi = "Matang";
        data.skor = parseFloat((60 + (maxRule * 20)).toFixed(1)); // Range skor 60% - 80%
    } else {
        data.klasifikasi = "Busuk";
        data.skor = parseFloat((80 + (maxRule * 20)).toFixed(1)); // Range skor 80% - 100%
    }
}

io.on("connection", (socket) => {
    // Berikan data terakhir untuk Browser yang baru buka halaman
    socket.emit("espData", lastData);

    // Menerima data dari ESP32
    socket.on("espData", (data) => {
        // Tandai bahwa socket ini adalah milik ESP32
        espSocketId = socket.id;

        // Terapkan EMA Filter untuk mencegah fluktuasi di web
        if (data.statusBacaan === "MEMBACA") {
            // Jika belum ada nilai EMA, inisialisasi dengan data raw pertama
            if (emaSensors.mq2 === null) emaSensors.mq2 = data.mq2_raw;
            if (emaSensors.mq3 === null) emaSensors.mq3 = data.mq3_raw;
            if (emaSensors.mq5 === null) emaSensors.mq5 = data.mq5_raw;
            if (emaSensors.tgs === null) emaSensors.tgs = data.tgs_raw;

            // Kalkulasi EMA
            emaSensors.mq2 = (EMA_ALPHA * data.mq2_raw) + ((1 - EMA_ALPHA) * emaSensors.mq2);
            emaSensors.mq3 = (EMA_ALPHA * data.mq3_raw) + ((1 - EMA_ALPHA) * emaSensors.mq3);
            emaSensors.mq5 = (EMA_ALPHA * data.mq5_raw) + ((1 - EMA_ALPHA) * emaSensors.mq5);
            emaSensors.tgs = (EMA_ALPHA * data.tgs_raw) + ((1 - EMA_ALPHA) * emaSensors.tgs);

            // Ganti data raw dengan nilai hasil filter (dibulatkan)
            data.mq2_raw = Math.round(emaSensors.mq2);
            data.mq3_raw = Math.round(emaSensors.mq3);
            data.mq5_raw = Math.round(emaSensors.mq5);
            data.tgs_raw = Math.round(emaSensors.tgs);
        } else {
            // Reset EMA saat sedang IDLE agar sampel buah baru tidak terpengaruh data sebelumnya
            emaSensors = { mq2: null, mq3: null, mq5: null, tgs: null };
        }

        // Duplikasi untuk UI (UI sekarang menampilkan data yang sudah di-smooth)
        data.mq2 = data.mq2_raw;
        data.mq3 = data.mq3_raw;
        data.mq5 = data.mq5_raw;
        data.tgs = data.tgs_raw;
        
        // Format Metrik Sistem
        data.uptime_formatted = formatUptime(data.uptime_sec);

        let skorHitung = 0.0;
        let teksKlasifikasi = "Standby (Terhubung)";

        if (data.statusBacaan === "MEMBACA") {
            computeFuzzyClassification(data);
            skorHitung = data.skor || 0.0;
            teksKlasifikasi = data.klasifikasi || "Analisis Berjalan...";
        }

        lastData = { 
            ...lastData, 
            ...data, 
            skor: Number(skorHitung).toFixed(1),
            klasifikasi: teksKlasifikasi,
            connected: true 
        };

        // Broadcast ke semua browser
        io.emit("espData", lastData);
    });

    // Menerima perintah dari Dashboard
    socket.on("dashboardCmd", (cmdData) => {
        io.emit("serverToEsp", cmdData);
    });

    // Deteksi jika Client / ESP Terputus
    socket.on("disconnect", () => {
        // Jika yang terputus adalah ESP32, set status menjadi Offline
        if (socket.id === espSocketId) {
            console.log("[SERVER] ESP32 Terputus!");
            espSocketId = null;
            lastData = { ...defaultData }; // Reset ke status awal
            emaSensors = { mq2: null, mq3: null, mq5: null, tgs: null }; // Reset EMA filter
            io.emit("espData", lastData); // Paksa UI kembali ke status offline
        }
    });
});

server.listen(PORT, () => {
    console.log(`[SERVER] Berjalan di port ${PORT}`);
});