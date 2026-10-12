# 專案狀態（交接用）

更新：2026-10-12 傍晚

## 已完成並實機驗證

| 功能 | 狀態 |
|---|---|
| 無 microSD 開機（內建 flash 當儲存） | ✅ 4 台實機驗證 |
| 中文字型（LXGW 文楷）＋**預設繁體中文** | ✅ |
| **Wi-Fi 修正**（畫面緩衝改 PSRAM，可用記憶體 47.8KB→94.8KB，掃到 15 個網路） | ✅ |
| **hang 修正**（關掉閒置 2 秒降頻到 10MHz；按鍵不再漏、USB 不再斷） | ✅ `CPU 10MHz: 0` |
| 自動睡眠 30 分鐘（原 10 分） | ✅ 已烤進映像 |
| Readflix 書庫畫面（書單＋下載 EPUB） | ✅ 程式完成 |
| Tailscale（MicroLink）整合進 build | ✅ 編譯＋連結（Flash 87.5%） |
| **Tailscale 手機掃碼登入** | ✅ 程式完成（`TailscaleLoginActivity` 顯示登入網址 QR） |
| 插 SD 自動複製（內建→SD） | ✅ 程式完成，**缺實卡未實測** |

## 待辦（依優先序）

1. **【最重要】內建儲存空間不足**
   - 內建儲存 = app1 = **6.25MB**，中文字型佔 **3.35MB** → **只剩約 3MB 放書**。
   - **這很可能就是「EPUB 上傳不了／打不開」的原因**：readflix 上的書常見 1–11MB，放不下就失敗。
   - 解法（二選一）：
     - (a) **中文字型裁成常用字**（約 1MB）→ 空出 ~2.3MB
     - (b) **加大分割區**：app1(6.25MB) + spiffs(3.4MB) 合併 → **約 10MB**（需改 `partitions.csv` 並重刷分割表；app0 不動）
   - 建議先做 (b)，空間最有感。

2. **Tailscale 實機驗證與記憶體量測**
   - 需把裝置插上 USB。
   - 關鍵數字：Tailscale 執行期約需 **42KB 任務堆疊**（內部 SRAM），目前最壞情況 Min Free ~72KB → **邊緣，必須量**。
   - 設定檔：`/.crosspoint/tailscale.json` = `{"enabled":true,"hostname":"onepage"}`（**空 authKey = 走 QR 掃碼登入**）。

3. **取得你的 Wi-Fi 設定並烤進映像**
   - 裝置連一次 Wi-Fi 後，用 `scripts/export-device-state.sh` 讀出 `/.crosspoint/wifi.json`，再烤進 release 映像。

4. 插 SD 自動複製：需要一張實體 microSD 實測。

## 刷機

```bash
# 會先備份 16MB flash 與 app1（內建儲存），再刷
./scripts/flash-device.sh /dev/cu.usbmodemXXXX
```

## Repos

| repo | 內容 |
|---|---|
| `chuangkevin/onepage-firmware` | 自製韌體原始碼 + release + 刷機腳本 |
| `chuangkevin/onepage-original-firmware` | 原廠 16MB 韌體（三台 SHA256 相同） |
| `chuangkevin/xteink-x4-c61-backup` | 第一台最初備份 + 裝置辨識 |

Tag：`v0.1.0`（無 SD 內建儲存 + 中文 + Wi-Fi 修正）
