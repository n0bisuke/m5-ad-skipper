#include "camera_pins.h"
#include <WiFi.h>
#include "esp_camera.h"
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include "img_converters.h"
#include "gifenc.h"
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

static const uint32_t CAPTURE_PERIOD_MS = 10 * 1000;  // 撮影～アップロード周期
static const uint32_t FRAME_COUNT = 5;
static const uint32_t GIF_FRAME_DELAY_MS = 200;       // GIF再生時のフレーム間隔（ミリ秒）
static const int GIF_W = 160;
static const int GIF_H = 120;

struct GifFrameRGB {
  uint8_t* rgb; // GIF_W * GIF_H * 3 bytes
};

static std::vector<GifFrameRGB> g_gifFrames;
static uint8_t g_palette[256 * 3];

static bool uploadToGyazo(const uint8_t* data, size_t len, const char* filename,
                          const char* contentType, String& responseJson);

static void init332Palette() {
  int n = 0;
  for (int r = 0; r < 8; r++) {
    for (int g = 0; g < 8; g++) {
      for (int b = 0; b < 4; b++) {
        uint8_t R = (r * 255) / 7;
        uint8_t G = (g * 255) / 7;
        uint8_t B = (b * 255) / 3;
        g_palette[n * 3 + 0] = R;
        g_palette[n * 3 + 1] = G;
        g_palette[n * 3 + 2] = B;
        n++;
      }
    }
  }
}

static void freeGifFrames() {
  for (auto& f : g_gifFrames) {
    if (f.rgb) {
      free(f.rgb);
      f.rgb = nullptr;
    }
  }
  g_gifFrames.clear();
}

static uint8_t* makeGifRGBFrame(const camera_fb_t* fb) {
  if (!fb) return nullptr;
  const int srcW = fb->width;
  const int srcH = fb->height;
  uint8_t* rgbFull = (uint8_t*)malloc(srcW * srcH * 3);
  if (!rgbFull) return nullptr;

  bool ok = fmt2rgb888(fb->buf, fb->len, (pixformat_t)fb->format, rgbFull);
  if (!ok) {
    free(rgbFull);
    return nullptr;
  }

  uint8_t* rgb = (uint8_t*)malloc(GIF_W * GIF_H * 3);
  if (!rgb) {
    free(rgbFull);
    return nullptr;
  }

  for (int y = 0; y < GIF_H; y++) {
    int sy = (int)((int64_t)y * srcH / GIF_H);
    const uint8_t* row = rgbFull + sy * srcW * 3;
    for (int x = 0; x < GIF_W; x++) {
      int sx = (int)((int64_t)x * srcW / GIF_W);
      const uint8_t* srcPix = row + sx * 3;
      int dst = (y * GIF_W + x) * 3;
      // fmt2rgb888() returns data in BGR order for many sensors, swap to RGB
      rgb[dst + 0] = srcPix[2];
      rgb[dst + 1] = srcPix[1];
      rgb[dst + 2] = srcPix[0];
    }
  }

  free(rgbFull);
  return rgb;
}

struct ColorBox {
  int start;
  int end;
  uint8_t rmin, rmax, gmin, gmax, bmin, bmax;
};

static void updateColorBox(ColorBox& box, const std::vector<uint32_t>& colors, const std::vector<uint32_t>& order) {
  uint8_t rmin = 255, rmax = 0, gmin = 255, gmax = 0, bmin = 255, bmax = 0;
  for (int i = box.start; i < box.end; ++i) {
    uint32_t c = colors[order[i]];
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >> 8) & 0xFF;
    uint8_t b = c & 0xFF;
    if (r < rmin) rmin = r;
    if (r > rmax) rmax = r;
    if (g < gmin) gmin = g;
    if (g > gmax) gmax = g;
    if (b < bmin) bmin = b;
    if (b > bmax) bmax = b;
  }
  box.rmin = rmin; box.rmax = rmax;
  box.gmin = gmin; box.gmax = gmax;
  box.bmin = bmin; box.bmax = bmax;
}

static int longestAxis(const ColorBox& box) {
  int rRange = box.rmax - box.rmin;
  int gRange = box.gmax - box.gmin;
  int bRange = box.bmax - box.bmin;
  if (rRange >= gRange && rRange >= bRange) return 0;
  if (gRange >= rRange && gRange >= bRange) return 1;
  return 2;
}

static bool quantizeFramesToPalette(const std::vector<GifFrameRGB>& frames,
                                    uint8_t* palette,
                                    std::vector<std::vector<uint8_t>>& frameIndices) {
  if (frames.empty()) return false;

  const size_t pixelsPerFrame = GIF_W * GIF_H;
  const size_t totalPixels = pixelsPerFrame * frames.size();

  std::vector<uint32_t> colors(totalPixels);
  std::vector<uint32_t> order(totalPixels);
  for (size_t f = 0; f < frames.size(); ++f) {
    const uint8_t* rgb = frames[f].rgb;
    for (size_t p = 0; p < pixelsPerFrame; ++p) {
      size_t idx = f * pixelsPerFrame + p;
      uint8_t r = rgb[p * 3 + 0];
      uint8_t g = rgb[p * 3 + 1];
      uint8_t b = rgb[p * 3 + 2];
      colors[idx] = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
      order[idx] = idx;
    }
  }

  std::vector<ColorBox> boxes;
  boxes.push_back({0, (int)totalPixels, 0, 255, 0, 255, 0, 255});
  updateColorBox(boxes[0], colors, order);

  while (boxes.size() < 256) {
    int boxIndex = -1;
    int maxRange = -1;
    for (size_t i = 0; i < boxes.size(); ++i) {
      if (boxes[i].end - boxes[i].start <= 1) continue;
      int range = std::max({boxes[i].rmax - boxes[i].rmin,
                            boxes[i].gmax - boxes[i].gmin,
                            boxes[i].bmax - boxes[i].bmin});
      if (range > maxRange) {
        maxRange = range;
        boxIndex = (int)i;
      }
    }
    if (boxIndex < 0) break;

    ColorBox box = boxes[boxIndex];
    int axis = longestAxis(box);
    auto begin = order.begin() + box.start;
    auto midIter = order.begin() + (box.start + box.end) / 2;
    auto end = order.begin() + box.end;

    auto cmp = [&](uint32_t a, uint32_t b) {
      uint8_t ar = (colors[a] >> 16) & 0xFF;
      uint8_t ag = (colors[a] >> 8) & 0xFF;
      uint8_t ab = colors[a] & 0xFF;
      uint8_t br = (colors[b] >> 16) & 0xFF;
      uint8_t bg = (colors[b] >> 8) & 0xFF;
      uint8_t bb = colors[b] & 0xFF;
      switch (axis) {
        case 0: return ar < br;
        case 1: return ag < bg;
        default: return ab < bb;
      }
    };

    std::nth_element(begin, midIter, end, cmp);

    ColorBox boxA{box.start, (int)((box.start + box.end) / 2), 0,0,0,0,0,0};
    ColorBox boxB{boxA.end, box.end, 0,0,0,0,0,0};
    updateColorBox(boxA, colors, order);
    updateColorBox(boxB, colors, order);

    boxes[boxIndex] = boxA;
    boxes.push_back(boxB);
  }

  if (boxes.size() > 256) boxes.resize(256);

  std::vector<uint8_t> paletteIndex(totalPixels, 0);
  for (size_t i = 0; i < boxes.size(); ++i) {
    ColorBox& box = boxes[i];
    uint64_t rSum = 0, gSum = 0, bSum = 0;
    int count = box.end - box.start;
    if (count == 0) count = 1;
    for (int j = box.start; j < box.end; ++j) {
      uint32_t c = colors[order[j]];
      rSum += (c >> 16) & 0xFF;
      gSum += (c >> 8) & 0xFF;
      bSum += c & 0xFF;
      paletteIndex[order[j]] = (uint8_t)i;
    }
    palette[i * 3 + 0] = (uint8_t)(rSum / count);
    palette[i * 3 + 1] = (uint8_t)(gSum / count);
    palette[i * 3 + 2] = (uint8_t)(bSum / count);
  }

  for (size_t i = boxes.size(); i < 256; ++i) {
    palette[i * 3 + 0] = palette[i * 3 + 1] = palette[i * 3 + 2] = 0;
  }

  frameIndices.assign(frames.size(), std::vector<uint8_t>(pixelsPerFrame));
  for (size_t idx = 0; idx < totalPixels; ++idx) {
    size_t frame = idx / pixelsPerFrame;
    size_t offset = idx % pixelsPerFrame;
    frameIndices[frame][offset] = paletteIndex[idx];
  }

  return true;
}

static bool captureFramePrepare(uint8_t** jpegBufOut, size_t* jpegLenOut) {
  *jpegBufOut = nullptr;
  *jpegLenOut = 0;

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[Capture] fb_get failed");
    return false;
  }

  Serial.printf("[Capture] raw frame: %dx%d, %u bytes (format=%d)\n",
                fb->width, fb->height, (unsigned)fb->len, fb->format);

  uint8_t* gifRGB = makeGifRGBFrame(fb);
  if (!gifRGB) {
    Serial.println("[Capture] makeGifRGBFrame failed");
    esp_camera_fb_return(fb);
    return false;
  }

  size_t jpegLen = 0;
  uint8_t* jpegBuf = nullptr;
  if (fb->format == PIXFORMAT_JPEG) {
    jpegLen = fb->len;
    jpegBuf = (uint8_t*)malloc(jpegLen);
    if (!jpegBuf) {
      Serial.println("[Capture] jpeg buffer alloc failed");
      if (gifRGB) free(gifRGB);
      esp_camera_fb_return(fb);
      return false;
    }
    memcpy(jpegBuf, fb->buf, jpegLen);
  } else {
    if (!frame2jpg(fb, 85, &jpegBuf, &jpegLen)) {
      Serial.println("[Capture] frame2jpg failed");
      if (gifRGB) free(gifRGB);
      esp_camera_fb_return(fb);
      return false;
    }
  }

  esp_camera_fb_return(fb);

  if (gifRGB) {
    g_gifFrames.push_back({gifRGB});
    Serial.printf("[Capture] converted frame #%u stored\n", (unsigned)g_gifFrames.size());
  }

  *jpegBufOut = jpegBuf;
  *jpegLenOut = jpegLen;
  return true;
}

static bool buildGifToLittleFS(int delay_ms) {
  if (g_gifFrames.empty()) return false;

  if (LittleFS.exists("/timelapse.gif")) {
    LittleFS.remove("/timelapse.gif");
  }

  std::vector<std::vector<uint8_t>> frameIndices;
  if (!quantizeFramesToPalette(g_gifFrames, g_palette, frameIndices)) {
    Serial.println("[GIF] quantize failed");
    return false;
  }

  ge_GIF* gif = ge_new_gif(
      "/littlefs/timelapse.gif",
      (uint16_t)GIF_W,
      (uint16_t)GIF_H,
      g_palette,
      8,
      0,
      0);
  if (!gif) {
    Serial.println("[GIF] ge_new_gif failed");
    return false;
  }

  uint16_t delay_cs = (delay_ms <= 0 ? 10 : (uint16_t)(delay_ms / 10));
  for (size_t i = 0; i < frameIndices.size(); ++i) {
    memcpy(gif->frame, frameIndices[i].data(), GIF_W * GIF_H);
    ge_add_frame(gif, delay_cs);
    Serial.printf("[GIF] frame %u appended (delay=%u cs)\n",
                  (unsigned)(i + 1), (unsigned)delay_cs);
  }

  ge_close_gif(gif);
  Serial.println("[GIF] file closed");
  return true;
}

static bool uploadGifIfReady() {
  if (g_gifFrames.size() < FRAME_COUNT) return false;

  if (!buildGifToLittleFS((int)GIF_FRAME_DELAY_MS)) {
    freeGifFrames();
    return false;
  }

  File f = LittleFS.open("/timelapse.gif", "r");
  if (!f) {
    Serial.println("[Task] open gif failed");
    freeGifFrames();
    return false;
  }

  size_t gifLen = f.size();
  uint8_t* gifBuf = (uint8_t*)malloc(gifLen);
  if (!gifBuf) {
    Serial.println("[Task] gif buffer alloc failed");
    f.close();
    freeGifFrames();
    LittleFS.remove("/timelapse.gif");
    return false;
  }
  f.readBytes((char*)gifBuf, gifLen);
  f.close();

  Serial.printf("[Task] GIF built: %u bytes\n", (unsigned)gifLen);

  String resp;
  bool ok = uploadToGyazo(gifBuf, gifLen, "timelapse.gif", "image/gif", resp);

  free(gifBuf);
  freeGifFrames();
  LittleFS.remove("/timelapse.gif");

  if (ok) {
    Serial.println("[Task] GIF upload OK");
  } else {
    Serial.println("[Task] GIF upload FAILED");
  }
  return ok;
}

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

  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("cam_hal", ESP_LOG_NONE);
  esp_log_level_set("camera", ESP_LOG_NONE);
  esp_log_level_set("sensor", ESP_LOG_NONE);
  Serial.setDebugOutput(false);

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

  init332Palette();

  if (!LittleFS.begin(true)) {
    Serial.println("[FS] LittleFS initial mount failed");
  }

  connectWiFi();
}

void loop() {
  static uint32_t lastCapture = 0;
  static uint32_t lastProgress = 0;
  static uint32_t photoCount = 0;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] reconnecting...");
    connectWiFi();
  }

  if (millis() - lastCapture >= CAPTURE_PERIOD_MS) {
    lastCapture = millis();
    lastProgress = lastCapture;
    photoCount++;
    Serial.printf("[Task] capture #%u\n", (unsigned)photoCount);

    uint8_t* jpegBuf = nullptr;
    size_t jpegLen = 0;
    if (captureFramePrepare(&jpegBuf, &jpegLen)) {
      char name[32];
      snprintf(name, sizeof(name), "snapshot_%lu.jpg", (unsigned long)photoCount);
      String resp;
      bool ok = uploadToGyazo(jpegBuf, jpegLen, name, "image/jpeg", resp);
      free(jpegBuf);
      if (ok) {
        Serial.println("[Task] JPEG upload OK");
      } else {
        Serial.println("[Task] JPEG upload FAILED");
      }
      uploadGifIfReady();
    }
  } else if (millis() - lastProgress >= 5000) {
    uint32_t elapsed = millis() - lastCapture;
    if (elapsed < CAPTURE_PERIOD_MS) {
      uint32_t remain = (CAPTURE_PERIOD_MS - elapsed) / 1000;
      Serial.printf("[Task] waiting... next capture in %us\n", (unsigned)remain);
    }
    lastProgress = millis();
  }

  delay(50);
}
