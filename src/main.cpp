#include "camera_pins.h"
#include <WiFi.h>
#include "esp_camera.h"

// ==== 追加: WPA2-Enterprise 用ヘッダ ====
// extern "C" {
//   #include "esp_wifi.h"
//   #include "esp_wpa2.h"   // WPA2-Enterprise
// }

#include <WiFi.h>  // これは残す

// ==== 構成スイッチ ====
#define USE_ATOMS3R_CAM
// #define USE_ATOMS3R_M12

#define STA_MODE
// #define AP_MODE

// ==== 秘密情報（git管理外） ====
// ここで secrets.h から SSID / EAP 資格情報を取り込みます
#include "secrets.h"

// ==== サーバ・カメラ用グローバル ====
WiFiServer server(80);
camera_fb_t* fb    = NULL;
uint8_t* out_jpg   = NULL;
size_t out_jpg_len = 0;

static void jpegStream(WiFiClient* client);

// ==== カメラ設定 ====
static camera_config_t camera_config = {
  .pin_pwdn     = PWDN_GPIO_NUM,
  .pin_reset    = RESET_GPIO_NUM,
  .pin_xclk     = XCLK_GPIO_NUM,
  .pin_sscb_sda = SIOD_GPIO_NUM,
  .pin_sscb_scl = SIOC_GPIO_NUM,
  .pin_d7       = Y9_GPIO_NUM,
  .pin_d6       = Y8_GPIO_NUM,
  .pin_d5       = Y7_GPIO_NUM,
  .pin_d4       = Y6_GPIO_NUM,
  .pin_d3       = Y5_GPIO_NUM,
  .pin_d2       = Y4_GPIO_NUM,
  .pin_d1       = Y3_GPIO_NUM,
  .pin_d0       = Y2_GPIO_NUM,
  .pin_vsync    = VSYNC_GPIO_NUM,
  .pin_href     = HREF_GPIO_NUM,
  .pin_pclk     = PCLK_GPIO_NUM,
  .xclk_freq_hz = 20000000,
  .ledc_timer   = LEDC_TIMER_0,
  .ledc_channel = LEDC_CHANNEL_0,
#ifdef USE_ATOMS3R_CAM
  .pixel_format = PIXFORMAT_RGB565,
  .frame_size   = FRAMESIZE_QVGA,
#endif
#ifdef USE_ATOMS3R_M12
  .pixel_format = PIXFORMAT_JPEG,
  .frame_size   = FRAMESIZE_UXGA,
#endif
  .jpeg_quality  = 12,
  .fb_count      = 2,
  .fb_location   = CAMERA_FB_IN_PSRAM,
  .grab_mode     = CAMERA_GRAB_LATEST,
  .sccb_i2c_port = 0,
};

#include "esp_heap_caps.h"

// ==== 便利マクロ（未定義チェック）====
#ifndef WIFI_SSID
  #error "WIFI_SSID not defined. Define in secrets.h"
#endif
#ifndef EAP_USERNAME
  #error "EAP_USERNAME not defined. Define in secrets.h"
#endif
#ifndef EAP_PASSWORD
  #error "EAP_PASSWORD not defined. Define in secrets.h"
#endif
#ifndef EAP_IDENTITY
  // 一部環境では identity = username で問題ありません
  #define EAP_IDENTITY EAP_USERNAME
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("psramFound=%d, free PSRAM=%u\n",
                psramFound(),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // カメラ用電源ラインを明確にトグル
  pinMode(POWER_GPIO_NUM, OUTPUT);
  digitalWrite(POWER_GPIO_NUM, HIGH);  // 電源OFF想定
  delay(50);
  digitalWrite(POWER_GPIO_NUM, LOW);   // 電源ON想定（POWER_N）
  delay(300);

  // カメラ初期化
  esp_err_t err = esp_camera_init(&camera_config);
  if (err != ESP_OK) {
    Serial.printf("Camera Init Fail: 0x%08x\n", err);
    delay(1000);
    esp_restart();
  }

#ifdef STA_MODE
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);

  // secrets.h から読み込んだ値を使用
  // 重要: WPA2-Enterprise 用のオーバーロードを使う
  // 形式: WiFi.begin(ssid, WPA2_AUTH_PEAP, identity, username, password);
  // 参考: 公式サンプル WiFiClientEnterprise.ino
  WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);

  Serial.printf("Connecting to %s (WPA2-Enterprise/PEAP)", WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 30000) { // タイムアウト再試行
      Serial.println("\nRe-attempting connection...");
      WiFi.disconnect(true, true);
      delay(300);
      WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);
      t0 = millis();
    }
  }
  Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());
#endif

#ifdef AP_MODE
  // AP_MODE は通常の WPA2-PSK/AP 用。WPA2-Enterprise とは別経路です。
  WiFi.softAP(WIFI_SSID, EAP_PASSWORD); // ※ここでは便宜的に流用（AP運用時のみ）
  Serial.printf("AP IP address: %s\n", WiFi.softAPIP().toString().c_str());
#endif

  server.begin();
}

// ==== MJPEG ストリーム ====
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static void jpegStream(WiFiClient* client) {
  Serial.println("Image stream start");
  client->println("HTTP/1.1 200 OK");
  client->printf("Content-Type: %s\r\n", _STREAM_CONTENT_TYPE);
  client->println("Content-Disposition: inline; filename=capture.jpg");
  client->println("Access-Control-Allow-Origin: *");
  client->println();

  static int64_t last_frame = 0;
  if (!last_frame) last_frame = esp_timer_get_time();

  for (;;) {
    fb = esp_camera_fb_get();
    if (fb) {
#ifdef USE_ATOMS3R_CAM
      frame2jpg(fb, 255, &out_jpg, &out_jpg_len);
#endif
#ifdef USE_ATOMS3R_M12
      out_jpg     = fb->buf;
      out_jpg_len = fb->len;
#endif
      client->print(_STREAM_BOUNDARY);
      client->printf(_STREAM_PART, out_jpg_len);

      int32_t remain = out_jpg_len;
      uint8_t* p = out_jpg;
      const uint32_t chunk = 8 * 1024;
      while (remain > 0) {
        int n = remain > (int32_t)chunk ? chunk : remain;
        if (client->write(p, n) == 0) goto client_exit;
        p += n; remain -= n;
      }

      int64_t now = esp_timer_get_time();
      int64_t ms = (now - last_frame) / 1000;
      last_frame = now;
      Serial.printf("MJPG: %luKB %lums (%.1ffps)\n",
                    (unsigned long)(out_jpg_len / 1024),
                    (unsigned long)ms, 1000.0 / (double)ms);

      esp_camera_fb_return(fb); fb = NULL;
#ifdef USE_ATOMS3R_CAM
      if (out_jpg) { free(out_jpg); out_jpg = NULL; out_jpg_len = 0; }
#endif
    } else {
      Serial.println("Camera capture failed");
    }
  }
client_exit:
  if (fb) { esp_camera_fb_return(fb); fb = NULL; }
#ifdef USE_ATOMS3R_CAM
  if (out_jpg) { free(out_jpg); out_jpg = NULL; out_jpg_len = 0; }
#endif
  client->stop();
  Serial.println("Image stream end");
}

void loop() {
  WiFiClient client = server.available();
  if (client) {
    while (client.connected()) {
      if (client.available()) jpegStream(&client);
    }
    client.stop();
    Serial.println("Client Disconnected.");
  }
}