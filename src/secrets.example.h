// secrets.h (DO NOT COMMIT)
// このファイルを src/secrets.h にコピーし、利用する Wi-Fi 方式に合わせて値を設定してください。
#pragma once

// ===== WPA/WPA2-PSK (一般的なSSID/パスワード) =====
#define WIFI_SSID       "your-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

/*
// ===== WPA2-Enterprise (PEAP/EAP) を使う場合 =====
// 上の WIFI_PASSWORD 定義を削除またはコメントアウトし、以下を設定してください。
//#define WIFI_PASSWORD_DISABLE
//#define WIFI_SSID       "espresso_STARBUCKS"
//#define EAP_IDENTITY    "Cxxxx@secure"
//#define EAP_USERNAME    "Cxxxx@secure"
//#define EAP_PASSWORD    "your-password-here"
*/

// Gyazo API アクセストークン
#define GYAZO_ACCESS_TOKEN "your-gyazo-access-token"
