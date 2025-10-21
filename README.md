# ATOMS3R-CAM Gyazo Snapshot

M5Stack ATOMS3R-CAM で JPEG 写真を 1 枚撮影し、そのまま Gyazo にアップロードする最小構成のファームウェアです。

## 主な機能
- `secrets.h` に設定した認証方式（WPA/WPA2-PSK または WPA2-Enterprise/PEAP）で Wi-Fi 接続。
- カメラから RGB565 フレームを取得し、ソフトウェアで JPEG へ変換して Gyazo に送信。
- 撮影およびアップロードの進捗をシリアルに表示。

## ディレクトリ構成
- `src/main.cpp` : 撮影・Gyazo アップロードロジック。
- `include/camera_pins.h` : ATOMS3R-CAM 用ピン定義。
- `src/secrets.h` : **コミット禁止**。Wi-Fi / Gyazo の秘密情報を保持。

## 準備
1. `src/secrets.example.h` を `src/secrets.h` にコピーし、利用環境に合わせて設定します。
   - 一般的な WPA/WPA2-PSK の場合:
     ```cpp
     #define WIFI_SSID       "your-ssid"
     #define WIFI_PASSWORD   "your-wifi-password"
     #define GYAZO_ACCESS_TOKEN "your-gyazo-token"
     ```
   - WPA2-Enterprise (PEAP) の場合は `WIFI_PASSWORD` を削除し、以下を設定します。
     ```cpp
     #define WIFI_SSID       "espresso_STARBUCKS"
     #define EAP_IDENTITY    "your-identity"
     #define EAP_USERNAME    "your-username"
     #define EAP_PASSWORD    "your-password"
     #define GYAZO_ACCESS_TOKEN "your-gyazo-token"
     ```
   `secrets.h` は `.gitignore` 済みです。
2. ATOMS3R-CAM を USB-C で PC に接続します。

## ビルドと書き込み
```bash
platformio run --environment m5stack-atoms3r           # ビルド
platformio run -t upload --environment m5stack-atoms3r # 書き込み
platformio device monitor -b 115200                    # シリアルモニタ
```

## 実行フロー
1. カメラを RGB565/QVGA 設定で初期化し、取得したフレームをソフトウェア JPEG 変換。
2. 指定した認証方式で Wi-Fi に接続。
3. `CAPTURE_PERIOD_MS`（デフォルト 40 秒）ごとに以下を実行：
   - JPEG フレームを 1 枚取得。
   - Gyazo へ multipart/form-data でアップロード。
   - 結果をシリアルに出力。

シリアル出力例:
```
[WiFi] connected. IP=192.168.1.25
[Task] waiting... next capture in 35s
[Task] waiting... next capture in 30s
[Task] capture + upload
[Capture] frame obtained: 320x240, 47210 bytes
[Gyazo] payload sent, awaiting response
[Gyazo] response: { ... }
[Task] upload OK
```

## 注意事項
- Gyazo への HTTPS 通信では `client.setInsecure()` を使用しています。運用環境ではルート CA 証明書を設定してください。
- `CAPTURE_PERIOD_MS` を短くし過ぎると Gyazo 側のレート制限に抵触する可能性があります。
- 写真サイズを変えたい場合は `camera_config` の `frame_size` を、画質を変えたい場合は `frame2jpg` の品質値を調整してください。
