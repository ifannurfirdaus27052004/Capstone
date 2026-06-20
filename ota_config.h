#ifndef OTA_CONFIG_H
#define OTA_CONFIG_H

// Konfigurasi default untuk koneksi OTA / Socket.IO.
// Nilai ini dapat diubah manual di file ini, atau melalui captive portal
// WiFiManager saat ESP32 pertama kali booting.

const char* default_ws_host = "fruit-maturity.ijuloss.my.id";
const int default_ws_port = 443;
const bool default_use_ssl = true;

#endif // OTA_CONFIG_H
