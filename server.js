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
const io = new Server(server, {
    cors: { origin: "*" }
});

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

    if ([mq2, mq3, mq5, tgs].some((v) => typeof v !== 'number')) {
        return null;
    }

    const mq3Tinggi = membershipTinggi(mq3, 800.0, 2200.0);
    const mq2Tinggi = membershipTinggi(mq2, 1000.0, 2500.0);
    const mq5Tinggi = membershipTinggi(mq5, 1000.0, 2500.0);
    const tgsRendah = membershipRendah(tgs, 1200.0, 2800.0);

    const skor = (mq3Tinggi * 40.0) + (mq2Tinggi * 20.0) +
                 (mq5Tinggi * 20.0) + (tgsRendah * 20.0);

    let klasifikasi;
    if (skor < 35.0) klasifikasi = 'Mentah';
    else if (skor < 70.0) klasifikasi = 'Setengah Matang';
    else klasifikasi = 'Matang';

    return {
        klasifikasi,
        skor: Number(skor.toFixed(1))
    };
}

const PORT = process.env.PORT || 4000;

app.use(express.static(path.join(__dirname)));

// State terakhir in-memory (hilang saat restart, sesuai kebutuhan)
let lastData = {
    mq2_raw: 0, mq3_raw: 0, mq5_raw: 0, tgs_raw: 0,
    mq2: 0, mq3: 0, mq5: 0, tgs: 0,
    klasifikasi: "Menunggu Data...",
    skor: 0,
    statusBacaan: "IDLE",
    pompa1: "OFF",
    pompa2: "OFF",
    connected: false // status koneksi ESP32 ke server
};

io.on("connection", (socket) => {
    console.log(`[CONNECT] Client baru: ${socket.id}`);

    // Kirim state terakhir ke client yang baru connect (browser ataupun ESP32)
    socket.emit("espData", lastData);

    // ---- DARI ESP32: data telemetri sensor + status pompa ----
    socket.on("espData", (data) => {
        console.log("[ESP] espData received:", data);

        const classification = computeFuzzyClassification(data);
        if (classification) {
            data.klasifikasi = classification.klasifikasi;
            data.skor = classification.skor;
        }

        data.mq2 = data.mq2_raw ?? data.mq2;
        data.mq3 = data.mq3_raw ?? data.mq3;
        data.mq5 = data.mq5_raw ?? data.mq5;
        data.tgs = data.tgs_raw ?? data.tgs;

        lastData = { ...lastData, ...data, connected: true };
        // Broadcast ke SEMUA client lain (dashboard browser)
        socket.broadcast.emit("espData", lastData);
    });

    // ---- DARI DASHBOARD BROWSER: command tombol pompa ----
    // payload: { cmd: "setPompa1", data: { state: true } }
    socket.on("dashboardCmd", (payload) => {
        console.log("[CMD] Dari dashboard:", payload);
        if (!payload || typeof payload.cmd !== 'string') return;

        // Build command object matching ArduinoJson ESP32 expectations
        const cmdObj = {
            cmd: payload.cmd,
            data: payload.data || {}
        };

        // Emit as Socket.IO frame with proper JSON format for ESP32
        // Format: 42[["serverToEsp", {cmd, data}]]
        const frameData = JSON.stringify(["serverToEsp", cmdObj]);
        io.emit("serverToEsp", "42" + frameData);
        
        console.log("[CMD] Sent to ESP32:", "42" + frameData);
    });

    socket.on("disconnect", () => {
        console.log(`[DISCONNECT] Client: ${socket.id}`);
    });
});

server.listen(PORT, () => {
    console.log(`[SERVER] Berjalan di port ${PORT}`);
});
