# Claude Buddy (LilyGo T-Display S3 強化版)

Claude Buddy 是一個能讓 Claude 桌面應用程式 (macOS/Windows) 透過藍牙 (BLE) 連接到硬體裝置的專案。它能讓 Claude Cowork 或 Claude Code 在您的桌面小螢幕上顯示授權請求、即時對話紀錄以及可愛的寵物動畫。

本專案是針對 **LilyGo T-Display S3** 進行的深度移植與優化版本，專為其 170x320 螢幕與雙核心 ESP32-S3 架構量身打造。

<p align="center">
  <img src="docs/device.jpg" alt="Claude Buddy T-Display S3 實拍" width="500">
</p>

## ✨ 移植版特色功能

*   **🚀 零閃爍動畫引擎**：採用動態 RAM 快取技術處理 GIF，確保在切換狀態或循環播放時完全不會產生閃爍或黑屏。
*   **📐 雙向螢幕支援**：完美支援直向 (170x320) 與橫向 (320x170) 模式無縫切換。
*   **📜 智能跑馬燈 (Landscape Marquee)**：在橫向模式下，過長的對話紀錄會自動啟動左右來回捲動效果，確保訊息不被裁切。
*   **💡 PWM 亮度調節**：支援 4 段硬體級 PWM 亮度控制 (5000Hz)，可根據環境光線隨時調整。
*   **💾 斷電記憶功能**：自動記憶您最後設定的螢幕角度與亮度，重開機後無需重新設定。
*   **🛡️ 穩定授權介面**：重構了工具授權彈窗邏輯，利用無閃爍的圖層疊加系統，操作更穩定。
*   **🔋 進階 HUD 資訊**：整合電池電壓、藍牙配對狀態與即時對話紀錄顯示。
*   **🇨🇳 滿版中文支援**：針對直立模式優化中文字體排版，完美利用 170px 寬度，提升閱讀體驗。

## ⚡ 快速刷機（給一般網友）

如果您不想自行編譯程式碼，可以透過瀏覽器直接將編譯好的韌體燒錄到設備中：

1. 前往 [最新 GitHub Release 頁面](https://github.com/oh001738/claude-desktop-buddy-for-tdisplay-s3/releases) 下載 4 個 `.bin` 檔案。
2. 使用 Chrome 或 Edge 瀏覽器開啟 [ESP Web Flasher](https://web.esphome.io/)。
3. 將 T-Display S3 透過 USB 接上電腦，點擊 **Connect**。
4. 加入剛剛下載的四個檔案，並精準填寫對應的偏移位址 (Offset)：

| 檔案名稱          | 燒錄位址   | 說明                         |
| ----------------- | ---------- | ---------------------------- |
| `bootloader.bin`  | `0x0000`   | 系統引導程式                 |
| `partitions.bin`  | `0x8000`   | 分區表 (no_ota)              |
| `firmware.bin`    | `0x10000`  | 主程式韌體                   |
| `littlefs.bin`    | `0x290000` | 角色素材與 GIF 檔案庫         |

5. 確認無誤後點擊 **Program**，等待進度條跑完重啟即可！

## 🎮 操作說明

| 動作                    | 直向模式 (Portrait)    | 橫向模式 (Landscape)       |
| ----------------------- | ---------------------- | -------------------------- |
| **正面/中間按鈕 (Btn A)**| 切換畫面 / 核准授權    | 核准授權請求               |
| **側邊上方按鈕 (Btn B)**| 滾動紀錄 / 切換分頁    | 拒絕授權 / 切換螢幕角度     |
| **長按正面按鈕**        | 開啟主選單             | 開啟主選單                 |
| **電源鍵 (短按)**       | 螢幕休眠 / 喚醒        | 螢幕休眠 / 喚醒            |
| **搖晃裝置**            | 觸發頭暈動畫           | 觸發頭暈動畫               |
| **螢幕朝下放置**        | 進入睡眠模式 (恢復體力) | 進入睡眠模式 (恢復體力)     |

## 📂 角色系統

本專案同時支援傳統的 ASCII 寵物與現代化的 GIF 動畫角色。

### ASCII 寵物 (內建 18 種)
包含：*Axolotl, Blob, Cactus, Capybara, Cat, Chonk, Dragon, Duck, Ghost, Goose, Mushroom, Octopus, Owl, Penguin, Rabbit, Robot, Snail, Turtle.*

### GIF 角色包 (`manifest.json`)
自訂角色包是一個資料夾，裡面包含 JSON 設定檔與優化過的 GIF（建議寬度 96px，最高 160px）。
```json
{
  "name": "bufo",
  "colors": { "body": "#6B8E23", "bg": "#000000", "text": "#FFFFFF" },
  "states": {
    "sleep": "sleep.gif",
    "idle": ["idle_1.gif", "idle_2.gif"],
    "busy": "busy.gif",
    "attention": "attention.gif"
  }
}
```

## 🛠️ 開發與編譯

本專案基於 [PlatformIO](https://platformio.org/) 開發。

```bash
# 編譯並上傳主程式
pio run -e lilygo-t-display-s3 -t upload

# 編譯並上傳 LittleFS 素材 (包含 GIF 角色)
pio run -e lilygo-t-display-s3 -t uploadfs
```

### 內建 Python 工具 (`tools/`)
*   **`level_up.py`**: 透過序列埠傳送虛擬的 Token 數量，強制讓您的 Buddy 升級。
*   **`prep_character.py`**: 自動調整來源 GIF 尺寸為 96px 寬，並優化供設備使用。
*   **`flash_character.py`**: 直接透過 USB 將角色包資料夾傳入設備。
*   **`test_xfer.py`**: BLE 資料夾推送協議的除錯工具。

## 📡 開發者：藍牙 (BLE) 協議

本設備採用 **Nordic UART Service (NUS)** 進行低延遲通訊。
*   **Service UUID**: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
*   **RX UUID**: `6e400002-b5a3-f393-e0a9-e50e24dcca9e` (寫入)
*   **TX UUID**: `6e400003-b5a3-f393-e0a9-e50e24dcca9e` (通知)
*   **配對模式**: MITM / Passkey Entry (DisplayOnly)。

## ✒️ 致謝與字體聲明
*   **[Zpix (最像素)](https://github.com/SolidZORO/zpix-pixel-font)**：本專案使用的中文字體，由 **SolidZORO** 開發。本專案為個人非營利性質，符合其個人免費使用條款。若您打算將本專案用於商業用途，請務必聯繫原作者取得正式授權。
*   **[bufo](https://bufo.zone)**：內建的角色素材來源。

---
*註：本專案為社群驅動的移植版本，旨在優化硬體體驗，非官方正式產品功能。*
