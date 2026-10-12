# OnePage 開發踩雷 + 燒錄方法（交接）

> 給下一個動這台的人。ESP32-C61 單核、2MB PSRAM、800×480 單色 e-ink。

## 燒錄方法

```sh
# 1) 建置
pio run -e onepage

# 2) ⚠️ 一定要把新產物複製到 release/（flash-device.sh 讀的是 release/，不是 .pio/）
cp -f .pio/build/onepage/{bootloader,partitions,firmware}.bin release/

# 3) 燒錄
./scripts/flash-device.sh [--no-backup] /dev/cu.usbmodemXXXX
```

分割表偏移（`flash-device.sh` 內）：`0x0` bootloader、`0x8000` partitions、
`0x10000` firmware、`0x650000` internal-storage。

⚠️ 踩雷：
- **忘了做步驟 2 就會燒到舊的 `release/firmware.bin`**，改了 code 卻沒生效（曾發生）。
- **任何佔住序列埠的東西（監控 / 擷取）都會讓燒錄失敗**，錯誤長這樣：
  `A serial exception error occurred: device reports readiness to read but returned no data`。燒錄前先
  `pkill -f "python.*serial"` / 關掉 monitor。
- **裝置 hang 會讓 USB-CDC 消失**（`/dev/cu.usbmodem*` 不見）→ 拔插 USB 重新列舉後再燒。
- `esptool` 版本用 `uvx` 自動抓；本機有 5.1.2（build 用）與 5.3.1（flash script 用）並存，正常。

## 建置踩雷

- **`ModuleNotFoundError: No module named 'SCons.Tool.FortranCommon'`**
  - 成因：**PlatformIO Core 6.2.0 在 `platformio/dependencies.py` 硬寫 `"tool-scons": "~4.41101.0"`**；
    `~` 會選到最新的 4.11.1（**SCons 4.11.1 已移除 `FortranCommon`**），把 pioarduino platform 指定的
    `scons-4.8.1` 蓋掉，IDF 建置就炸。
  - 修法：編輯 PlatformIO 安裝目錄的 `platformio/dependencies.py`，把該行改成**精確** pin：
    ```
    "tool-scons": "4.40801.0",
    ```
    驗證：`pio system info | grep -i scons` → `tool-scons @ 4.40801.0 (4.8.1)`。
  - ⚠️ 這是**本機 PlatformIO 安裝**的修改，不在 repo；PlatformIO 重裝/換機要再 patch。
- **build 會刪掉 repo 根目錄的 `CMakeLists.txt` 與 `dependencies.lock`**（IDF component manager 行為）。
  commit 前記得 `git checkout -- CMakeLists.txt dependencies.lock` 還原。
- 改 `custom_sdkconfig`（在 `[env:onepage]`）會觸發**全量重編**（~4 分鐘）；一般增量 ~20–40 秒。
- 首次 build 會下載數 GB（pioarduino platform、ESP-IDF 5.5、Arduino 3.3.7、工具鏈）。

## 功能踩雷

### Tailscale（MicroLink）— 已從 UI 移除
單核 C61 + 內部 SRAM 太小，最後判定不穩、把 Readflix/Tailscale 選單拔掉。以下是當時修過/踩過的：
- **單核 pin core 1 → assert 重開**：C61 只有 core 0。改 `src/network/TailscaleManager.cpp` 與
  `components/microlink/include/microlink_internal.h`（`ML_TASK_COORD_CORE`/`ML_TASK_WG_MGR_CORE`）、
  `components/microlink/src/ml_udp.c` 為 core 0。
- **內部 SRAM 不足**：連上 Wi-Fi 後只剩 ~19–28KB，`microlink_start` 開不了任務
  （`Failed to create derp_tx task`）。開 `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
  + 把任務堆疊放 PSRAM（`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`）+ `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`。
- **🔴 PSRAM 堆疊 + 寫 NVS/flash → `CPU_LOCKUP`（rst:0x1a）**：寫 flash 時 cache 關閉，堆疊在 PSRAM 的任務無法被排程。
  **只有會碰 flash 的任務必須用內部堆疊**：`ts_bringup`（`microlink_init` 寫 NVS）與 `wg_mgr`（`ml_peer_nvs_save`）。
  其餘（`net_io`/`derp_tx`/`coord`/`udp_rx`）可放 PSRAM。
- **Readflix 太早抓書單**：tailnet 要 ~30 秒才 up，原程式立刻抓 → `ESP_ERR_HTTP_CONNECT`。要在抓之前
  poll `TailscaleManager::isUp()`（最多 ~40 秒）。
- **HTTPS/TLS 開不起來**：內部 SRAM 剩 ~5KB。可 `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` 讓 mbedTLS 走 PSRAM。

### 面板（open-onepage-sdk `EInkDisplay`）
- **SD 卡與面板共用 SPI bus**（`SD_CS=26 // shared SPI bus with EPD`），且 **GPIO27 = EPD reset + SD/MIC 電源**
  → **不能硬體 reset EPD**（會連 SD 一起斷電），只在 `HalGPIO::begin()` 早期做一次。
- **面板 BUSY 偶爾卡死**（重讀 SD 字型時）→ 驅動原本等 **30 秒** timeout，看起來像 hang。已把
  `pollBusy` 的 timeout 由 `30000` 改成 **`5000`**（`open-onepage-sdk/libs/display/EInkDisplay/src/EInkDisplay.cpp`）。

### 介面中文字型（`src/CjkUiFontSystem.*`）
- UI 中文字是**寫死的讀取路徑**（`kCandidatePaths`，預設 `/.fonts/LXGWWenKai-Regular/LXGWWenKai-Regular_12.cpfont`）。
- 已加設定 `uiFontFamilyName` / `uiFontSize`（`設定 → 顯示 → 介面字型`），從 SD 卡家族清單挑，**離開設定頁即時重載**
  （`CjkUiFontSystem::reload`）。轉字型用 `lib/EpdFont/scripts/fontconvert_sdcard.py`（PingFang TC 那批放在 `fonts/PingFangTC/`）。
- e-ink 只有 1-bit，中文小字本就會鋸齒；粗一點/大一號會好一點。

### 名牌（Nameplate）
- 睡眠模式 `SLEEP_SCREEN_MODE::NAMEPLATE` → 讀 **`/.crosspoint/badge.bmp`**；
  方向 `SETTINGS.nameplateOrientation`（**預設 0 = Portrait 直向 480×800**）。
- Web 頁：**`/badge`**（`src/network/html/BadgePage.html`），瀏覽器端合成 → 產 **480×800 1-bit BMP** →
  POST 到現成的 **`/upload?path=/.crosspoint`**。導覽列已加「名牌」。
- 版面：**SARA logo（上）→ 照片（中）→ 名字／職稱（下）**；照片用 **Floyd–Steinberg 網點**。logo 以 base64 內嵌。
- 刷新：名牌用 `renderBitmapSleepScreen(bitmap)` 後**再做一次 `displayBuffer(HalDisplay::FULL_REFRESH)`**（v2）。
  實測這版（render + full）對比/殘影最可接受；「先 clear+FULL 再 half」與「只 half」都更糟。

## TODO

- [x] **名牌載具條碼**：`BadgePage.html` 可輸入 `/` 開頭的 7 碼載具號；用 Code 39 產生黑白條碼、在條碼下列印原號，再和名牌一起存成 BMP。留空不顯示；格式錯誤會在欄位下提示。
