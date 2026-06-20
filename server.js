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

const PORT = process.env.PORT || 3000;

app.use(express.static(path.join(__dirname, "public")));

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
        lastData = { ...lastData, ...data, connected: true };
        // Broadcast ke SEMUA client lain (dashboard browser)
        socket.broadcast.emit("espData", lastData);
    });

    // ---- DARI DASHBOARD BROWSER: command tombol pompa ----
    // payload: { cmd: "setPompa1", data: { state: true } }
    socket.on("dashboardCmd", (payload) => {
        console.log("[CMD] Dari dashboard:", payload);
        // Teruskan ke semua client (ESP32 akan memprosesnya)
        io.emit("serverToEsp", payload);
    });

    socket.on("disconnect", () => {
        console.log(`[DISCONNECT] Client: ${socket.id}`);
    });
});

server.listen(PORT, () => {
    console.log(`[SERVER] Berjalan di port ${PORT}`);
});
