# Claude Buddy (LilyGo T-Display S3 Edition)

Claude for macOS and Windows can connect Claude Cowork and Claude Code to maker devices over BLE, allowing developers to build hardware that displays permission prompts, recent messages, and other real-time interactions.

This project is a high-performance port and enhancement for the **LilyGo T-Display S3**, optimized for its 170x320 parallel display and dual-core ESP32-S3 architecture.

<p align="center">
  <img src="docs/device.jpg" alt="Claude Buddy on T-Display S3" width="500">
</p>

## ✨ Enhanced Features (T-Display S3)

*   **🚀 Zero-Flicker Animation Engine**: Dynamic RAM-based GIF caching ensures smooth 60FPS transitions between pet states.
*   **📐 Dual Orientation Support**: Seamlessly switch between Portrait (170x320) and Landscape (320x170) modes. Settings are persisted to NVS.
*   **📜 Smart Marquee**: In landscape mode, long messages from Claude automatically scroll (ping-pong effect) to ensure full legibility.
*   **💡 High-Frequency PWM Dimming**: Smooth 4-level brightness control via 5000Hz PWM on GPIO 38.
*   **🛡️ Stable Modal System**: Refactored Tool Approval UI using a flicker-free sprite overlay system.
*   **🔋 Advanced HUD**: Integrated status for battery voltage, Bluetooth pairing, and real-time conversation transcripts.

## ⚡ Quick Flashing for Users

If you don't want to compile the code yourself, use the [ESP Web Flasher](https://web.esphome.io/) with these binaries and offsets:

| Binary File       | Flash Address | Description                 |
| ----------------- | ------------- | --------------------------- |
| `bootloader.bin`  | `0x0000`      | System bootloader           |
| `partitions.bin`  | `0x8000`      | Partition table (no_ota)    |
| `firmware.bin`    | `0x10000`     | Main application            |
| `littlefs.bin`    | `0x290000`    | Character assets & pets     |

## 🛠️ Development & Tools

Install [PlatformIO](https://platformio.org/) to build from source.

```bash
# Upload application firmware
pio run -e lilygo-t-display-s3 -t upload

# Upload character assets (GIFs/Configs)
pio run -e lilygo-t-display-s3 -t uploadfs
```

### Included Python Utilities (`tools/`)
*   **`prep_character.py`**: Resizes source GIFs to 96px width and optimizes them for the device.
*   **`flash_character.py`**: Side-loads a character pack folder directly to the device over USB.
*   **`test_xfer.py`**: Debugging tool for the BLE folder-push protocol.

## 📂 Character System

The device supports both classic ASCII pets and modern GIF characters.

### ASCII Buddies (18 Species)
Cycles through: *Axolotl, Blob, Cactus, Capybara, Cat, Chonk, Dragon, Duck, Ghost, Goose, Mushroom, Octopus, Owl, Penguin, Rabbit, Robot, Snail, Turtle.*

### GIF Character Pack (`manifest.json`)
A character pack is a folder containing a JSON manifest and optimized GIFs.
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

## 📡 Developer: BLE Protocol

The device uses the **Nordic UART Service (NUS)** for low-latency communication.
*   **Service UUID**: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
*   **RX UUID**: `6e400002-b5a3-f393-e0a9-e50e24dcca9e` (Write)
*   **TX UUID**: `6e400003-b5a3-f393-e0a9-e50e24dcca9e` (Notify)
*   **Pairing**: MITM / Passkey Entry (DisplayOnly).

---

*Note: This is a community-driven port of the original Claude Buddy project.*
