# ATOMS3R-CAM Gyazo GIF Snapshot

M5Stack ATOMS3R-CAM で 10 秒ごとにフレームを撮影し、3 枚溜まるごとに GIF を生成して Gyazo へアップロードするファームウェアです。

## 主な機能
- `secrets.h` に設定した認証方式（WPA/WPA2-PSK または WPA2-Enterprise/PEAP）で Wi-Fi 接続。
- カメラから RGB565 フレームを 10 秒ごとに 1 枚取得し、即座に JPEG として Gyazo へ送信。
- 3 枚撮影したタイミングで 160x120 / 256色の GIF を組み立て（中央値分割によるパレット生成）、合わせて Gyazo に送信。
- 撮影およびアップロードの進捗をシリアルに表示。

## ディレクトリ構成
- `src/main.cpp` : バースト撮影・GIF 生成・Gyazo アップロードロジック。
- `lib/gifenc/` : 簡易 GIF エンコーダ実装。
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
3. `CAPTURE_PERIOD_MS`（デフォルト 10 秒）ごとに以下を実行：
   - RGB565 フレームを 1 枚取得し、JPEG 変換結果を Gyazo へアップロード。
   - 同時に GIF 用の 160x120 8bit フレームを保持。
   - 5 枚溜まったら GIF を生成して Gyazo にアップロード（再生ディレイは `GIF_FRAME_DELAY_MS` で制御）。

シリアル出力例:
```
[WiFi] connected. IP=192.168.1.25
[Task] waiting... next capture in 9s
[Task] waiting... next capture in 4s
[Task] capture #1
[Capture] raw frame: 320x240, 153600 bytes (format=0)
[Capture] converted frame #1 stored
[Gyazo] payload sent, awaiting response
[Gyazo] response: { ... }
[Task] JPEG upload OK
[Task] waiting... next capture in 9s
...（省略）
[Task] capture #5
[Task] GIF built: 9300 bytes
[Gyazo] payload sent, awaiting response
[Task] GIF upload OK
```

## 注意事項
- Gyazo への HTTPS 通信では `client.setInsecure()` を使用しています。運用環境ではルート CA 証明書を設定してください。
- `CAPTURE_PERIOD_MS` を短くし過ぎると Gyazo 側のレート制限に抵触する可能性があります。
- GIF の解像度を変えたい場合は `GIF_W` / `GIF_H` と `camera_config.frame_size` を揃えて調整してください。再生速度は `GIF_FRAME_DELAY_MS` で調整できます。
