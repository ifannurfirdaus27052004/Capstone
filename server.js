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

// Fungsi mengubah detik menjadi format "0h 00m 00s"
function formatUptime(seconds) {
    if (!seconds) return "0h 00m 00s";
    const h = Math.floor(seconds / 3600);
    const m = Math.floor((seconds % 3600) / 60);
    const s = Math.floor(seconds % 60);
    return `${h}h ${m}m ${s}s`;
}

io.on("connection", (socket) => {
    // Berikan data terakhir untuk Browser yang baru buka halaman
    socket.emit("espData", lastData);

    // Menerima data dari ESP32
    socket.on("espData", (data) => {
        // Tandai bahwa socket ini adalah milik ESP32
        espSocketId = socket.id;

        // Duplikasi untuk UI
        data.mq2 = data.mq2_raw;
        data.mq3 = data.mq3_raw;
        data.mq5 = data.mq5_raw;
        data.tgs = data.tgs_raw;
        
        // Format Metrik Sistem
        data.uptime_formatted = formatUptime(data.uptime_sec);

        let skorHitung = 0.0;
        let teksKlasifikasi = "Standby (Terhubung)";

        if (data.statusBacaan === "MEMBACA") {
            let avgSensor = ((data.mq2_raw || 0) + (data.mq3_raw || 0) + (data.mq5_raw || 0) + (data.tgs_raw || 0)) / 4;
            skorHitung = Math.min((avgSensor / 4095) * 100, 100);
            
            if (skorHitung < 30) teksKlasifikasi = "Mentah";
            else if (skorHitung < 70) teksKlasifikasi = "Setengah Matang";
            else teksKlasifikasi = "Matang";
        }

        lastData = { 
            ...lastData, 
            ...data, 
            skor: skorHitung.toFixed(1),
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
            io.emit("espData", lastData); // Paksa UI kembali ke status offline
        }
    });
});

server.listen(PORT, () => {
    console.log(`[SERVER] Berjalan di port ${PORT}`);
});