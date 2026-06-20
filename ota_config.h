#ifndef OTA_CONFIG_H
#define OTA_CONFIG_H

// Konfigurasi default untuk koneksi Socket.IO.
// Nilai ini dapat diubah manual di file ini, atau melalui captive portal
// WiFiManager saat ESP32 pertama kali booting.

// Domain fruitmaturity.ijuloss.my.id adalah reverse proxy dengan SSL/TLS
// ESP32 secara otomatis akan mendeteksi dan menggunakan SSL connection
// jika domain mengandung "ijuloss.my.id"

const char* default_ws_host = "fruitmaturity.ijuloss.my.id";
const int default_ws_port = 4000;

#endif // OTA_CONFIG_H
