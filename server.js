// =================================================================
// SERVER: Jembatan Socket.IO antara ESP32 dan Dashboard Browser
// =================================================================
// - ESP32 terhubung sebagai satu Socket.IO client biasa, mengirim
//   event "espData" dan menerima command via event "serverToEsp".
// - Browser dashboard juga terhubung sebagai Socket.IO client, hanya
//   menerima broadcast "espData" dan mengirim command lewat event
//   "dashboardCmd" yang oleh server diteruskan sebagai "serverToEsp"
//   ke SEMUA client (termasuk ESP32). ESP32 mengabaikan command yang
//   bukan untuknya secara natural karena hanya memproses cmd dikenal.
// - Data terakhir disimpan in-memory (tidak ada database), supaya
//   dashboard yang baru connect langsung dapat state terkini.
// =================================================================

const express = require("express");
const http = require("http");
const path = require("path");
const { Server } = require("socket.io");

const app = express();
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: "*" } });

function membershipRendah(x, batasBawah, batasAtas) {
    if (x <= batasBawah) return 1.0;
    if (x >= batasAtas) return 0.0;
    return (batasAtas - x) / (batasAtas - batasBawah);
}

function membershipTinggi(x, batasBawah, batasAtas) {
    if (x <= batasBawah) return 0.0;
    if (x >= batasAtas) return 1.0;
    return (x - batasBawah) / (batasAtas - batasBawah);
}

function computeFuzzyClassification(data) {
    const mq2 = typeof data.mq2_raw === 'number' ? data.mq2_raw : data.mq2;
    const mq3 = typeof data.mq3_raw === 'number' ? data.mq3_raw : data.mq3;
    const mq5 = typeof data.mq5_raw === 'number' ? data.mq5_raw : data.mq5;
    const tgs = typeof data.tgs_raw === 'number' ? data.tgs_raw : data.tgs;
    // Logika Fuzzy dapat ditambahkan di dalam blok ini 
}

const PORT = process.env.PORT || 4000;
app.use(express.static(path.join(__dirname)));

// State terakhir in-memory (hilang saat restart, sesuai kebutuhan)
let lastData = {
    mq2_raw: 0, mq3_raw: 0, mq5_raw: 0, tgs_raw: 0,
    mq2: 0, mq3: 0, mq5: 0, tgs: 0,
    klasifikasi: "Menunggu Data...", skor: 0,
    statusBacaan: "IDLE", pompa1: "OFF", pompa2: "OFF",
    connected: false // status koneksi ESP32 ke server
};

io.on("connection", (socket) => {
    console.log(`[CONNECT] Client baru: ${socket.id}`);

    // Mengirim state terakhir ke client dashboard yang baru connect
    socket.emit("espData", lastData);

    // Menerima data telemetri dari ESP32
    socket.on("espData", (data) => {
        lastData = { ...lastData, ...data, connected: true };
        // Broadcast data ke seluruh browser dashboard yang terkoneksi
        io.emit("espData", lastData);
    });

    // Menerima perintah kendali dari Dashboard (seperti kontrol pompa)
    // dan meneruskannya ke ESP32 sebagai instruksi "serverToEsp"
    socket.on("dashboardCmd", (cmdData) => {
        io.emit("serverToEsp", cmdData);
    });

    socket.on("disconnect", () => {
        console.log(`[DISCONNECT] Client terputus: ${socket.id}`);
    });
});

server.listen(PORT, () => {
    console.log(`[SERVER] Berjalan di port ${PORT}`);
});