#include "camera_pins.h"
#include <WiFi.h>
#include "esp_camera.h"
#include "img_converters.h"
#include <WiFiClientSecure.h>
#include "secrets.h"   // DO NOT COMMIT（Wi-Fi / Gyazo の秘密を定義）
#include "esp_log.h"

#ifndef WIFI_SSID
  #error "WIFI_SSID not defined. Define in secrets.h"
#endif

#if defined(WIFI_PASSWORD) && !defined(WIFI_PASSWORD_DISABLE)
  #define WIFI_USE_WPA2_PSK 1
#elif defined(EAP_USERNAME) && defined(EAP_PASSWORD)
  #define WIFI_USE_WPA2_ENTERPRISE 1
  #ifndef EAP_IDENTITY
    #define EAP_IDENTITY EAP_USERNAME
  #endif
#else
  #error "Define WIFI_PASSWORD for WPA2-PSK or EAP_USERNAME/EAP_PASSWORD for WPA2-Enterprise"
#endif

#ifndef GYAZO_ACCESS_TOKEN
  #error "GYAZO_ACCESS_TOKEN not defined. Define in secrets.h"
#endif

static const uint32_t CAPTURE_PERIOD_MS = 40 * 1000;  // 撮影～アップロード周期

static bool uploadToGyazo(const uint8_t* data, size_t len, const char* filename,
                          const char* contentType, String& responseJson) {
  if (!data || len == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();

  const char* host = "upload.gyazo.com";
  const int   port = 443;
  const char* boundary = "------------------------ESP32GyazoBoundary7e3c9a0";

  String head;
  head += "--"; head += boundary; head += "\r\n";
  head += "Content-Disposition: form-data; name=\"imagedata\"; filename=\"";
  head += (filename ? filename : "snapshot.jpg");
  head += "\"\r\n";
  head += "Content-Type: ";
  head += (contentType ? contentType : "application/octet-stream");
  head += "\r\n\r\n";

  String tail;
  tail += "\r\n--"; tail += boundary; tail += "--\r\n";

  size_t contentLength = head.length() + len + tail.length();

  if (!client.connect(host, port)) {
    Serial.println("[Gyazo] connect failed");
    return false;
  }
  Serial.println("[Gyazo] connected");

  String url = "/api/upload?access_token="; url += GYAZO_ACCESS_TOKEN;

  client.printf("POST %s HTTP/1.1\r\n", url.c_str());
  client.printf("Host: %s\r\n", host);
  client.println("Connection: close");
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.println();

  client.print(head);
  const size_t CHUNK = 8 * 1024;
  size_t remain = len;
  const uint8_t* p = data;
  while (remain > 0) {
    size_t n = (remain > CHUNK ? CHUNK : remain);
    if (client.write(p, n) == 0) {
      Serial.println("[Gyazo] write failed");
      return false;
    }
    p += n;
    remain -= n;
    Serial.printf("[Gyazo] sent %u/%u bytes\n",
                  (unsigned)(len - remain), (unsigned)len);
  }
  client.print(tail);
  Serial.println("[Gyazo] payload sent, awaiting response");

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }
  responseJson = client.readString();
  Serial.println("[Gyazo] response: " + responseJson);
  return responseJson.length() > 0;
}

static bool captureAndUploadPhoto() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Capture] fb_get failed");
    return false;
  }

  Serial.printf("[Capture] frame obtained: %dx%d, %u bytes (format=%d)\n",
                fb->width, fb->height, (unsigned)fb->len, fb->format);

  uint8_t* jpg_buf = nullptr;
  size_t jpg_len = 0;
  const uint8_t* payload = fb->buf;
  size_t payload_len = fb->len;
  bool need_free = false;

  if (fb->format == PIXFORMAT_JPEG) {
    jpg_buf = nullptr;
    jpg_len = 0;
  } else {
    if (!frame2jpg(fb, 80, &jpg_buf, &jpg_len)) {
      Serial.println("[Capture] frame2jpg failed");
      esp_camera_fb_return(fb);
      return false;
    }
    payload = jpg_buf;
    payload_len = jpg_len;
    need_free = true;
    Serial.printf("[Capture] converted to JPEG: %u bytes\n", (unsigned)payload_len);
  }

  esp_camera_fb_return(fb);

  String resp;
  bool ok = uploadToGyazo(payload, payload_len, "snapshot.jpg", "image/jpeg", resp);

  if (need_free && jpg_buf) {
    free(jpg_buf);
  }

  if (ok) {
    Serial.println("[Task] upload OK");
  } else {
    Serial.println("[Task] upload FAILED");
  }
  return ok;
}

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
  .pixel_format = PIXFORMAT_RGB565,
  .frame_size   = FRAMESIZE_QVGA,
  .jpeg_quality = 12,
  .fb_count     = 1,
  .fb_location  = CAMERA_FB_IN_PSRAM,
  .grab_mode    = CAMERA_GRAB_LATEST,
  .sccb_i2c_port = 0,
};

#if defined(WIFI_USE_WPA2_ENTERPRISE)
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  Serial.println("[WiFi] connecting (WPA2-Enterprise / PEAP)...");
  WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 30000) {
      Serial.println("\n[WiFi] retry...");
      WiFi.disconnect(true, true);
      delay(300);
      WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);
      t0 = millis();
    }
  }
  Serial.printf("\n[WiFi] connected. IP=%s\n", WiFi.localIP().toString().c_str());
}
#elif defined(WIFI_USE_WPA2_PSK)
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(true, true);
  Serial.println("[WiFi] connecting (WPA2-PSK)...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - t0 > 30000) {
      Serial.println("\n[WiFi] retry...");
      WiFi.disconnect(true, true);
      delay(300);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      t0 = millis();
    }
  }
  Serial.printf("\n[WiFi] connected. IP=%s\n", WiFi.localIP().toString().c_str());
}
#endif

void setup() {
  Serial.begin(115200);
  delay(200);

  esp_log_level_set("cam_hal", ESP_LOG_ERROR);
  esp_log_level_set("camera", ESP_LOG_ERROR);
  esp_log_level_set("sensor", ESP_LOG_WARN);

  pinMode(POWER_GPIO_NUM, OUTPUT);
  digitalWrite(POWER_GPIO_NUM, HIGH);
  delay(50);
  digitalWrite(POWER_GPIO_NUM, LOW);
  delay(300);

  if (esp_camera_init(&camera_config) != ESP_OK) {
    Serial.println("[Cam] init failed. rebooting...");
    delay(1000);
    esp_restart();
  }

  connectWiFi();
}

void loop() {
  static uint32_t lastUpload = 0;
  static uint32_t lastProgress = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] reconnecting...");
    connectWiFi();
  }

  if (millis() - lastUpload >= CAPTURE_PERIOD_MS) {
    lastUpload = millis();
    lastProgress = lastUpload;
    Serial.println("[Task] capture + upload");
    captureAndUploadPhoto();
  } else if (millis() - lastProgress >= 5000) {
    uint32_t elapsed = millis() - lastUpload;
    if (elapsed < CAPTURE_PERIOD_MS) {
      uint32_t remain = (CAPTURE_PERIOD_MS - elapsed) / 1000;
      Serial.printf("[Task] waiting... next capture in %us\n", (unsigned)remain);
    }
    lastProgress = millis();
  }

  delay(50);
}
