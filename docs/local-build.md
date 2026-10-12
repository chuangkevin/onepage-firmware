# 本機建置與燒錄（OnePage / ESP32-C61）

開發機（macOS）從零建置 `onepage` 環境的踩坑與正確流程。

## 前置

- PlatformIO（`pio`）需要 Python 3.10+。用 uv 安裝最短：
  ```sh
  uv tool install platformio
  export PATH="$HOME/.local/bin:$PATH"
  ```
- 第一次 `pio run -e onepage` 會下載 pioarduino platform、Arduino/ESP-IDF 工具鏈與 libs
  （數百 MB，網路慢時會跑很久）。之後為增量編譯。

## 已知地雷：`SCons.Tool.FortranCommon` 缺失

症狀：

```
*** [.pio/build/onepage/firmware.elf] ModuleNotFoundError : No module named 'SCons.Tool.FortranCommon'
========================= [FAILED] =========================
```

**根因**：pioarduino 的 `platform.json` 指定 `tool-scons` 為 `scons-4.8.1`（`SCons/Tool/FortranCommon.py` 還在），
但 **PlatformIO Core 會在 `platformio/dependencies.py` 硬記 `"tool-scons": "~4.41101.0"`**；`~` 會選到最新
**4.11.1（SCons 4.11.1 已移除 `FortranCommon`）**，把 platform 指定的 4.8.1 蓋掉 → IDF 建置失敗。

**修法**：把 PlatformIO Core 的 `tool-scons` **精確 pin 到 `4.40801.0`**（= SCons 4.8.1）。

編輯你安裝 PlatformIO 的那個 site-packages 下的 `dependencies.py`（uv tool 安裝範例）：

```
# 檔案：<uv>/tools/platformio/lib/python3.x/site-packages/platformio/dependencies.py
-        "tool-scons": "~4.41101.0",
+        "tool-scons": "4.40801.0",
```

驗證：`pio system info | grep -i scons` 應顯示 `tool-scons @ 4.40801.0 (4.8.1)`。

> 這是**本機 PlatformIO 安裝**的修改，不在本 repo；PlatformIO 重裝（升級/換機）後要再 patch 一次。

## 建置

```sh
pio run -e onepage
```

產物在 `.pio/build/onepage/`：`bootloader.bin`、`partitions.bin`、`firmware.bin`。

## 燒錄

`scripts/flash-device.sh` 燒的是 **`release/`** 底下的檔案（不是 `.pio/`）。
所以自建後要先同步：

```sh
cp -f .pio/build/onepage/{bootloader,partitions,firmware}.bin release/
./scripts/flash-device.sh [--no-backup] /dev/cu.usbmodemXXXX
```

> ⚠️ 不同步的話 `flash-device.sh` 會燒到 `release/` 裡的**舊** `firmware.bin`，改了 code 也沒用。

分割表偏移（腳本用）：`0x0` bootloader、`0x8000` partitions、`0x10000` firmware、`0x650000` internal-storage。
