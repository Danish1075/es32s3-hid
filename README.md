# ESP32-S3 Wi-Fi Ducky Pro (N16R8 Edition) 🦆

A sophisticated, web-based USB HID injector and remote keyboard control system running on the ESP32-S3. This project utilizes the S3's native USB capabilities and massive PSRAM to buffer and execute large Ducky Scripts over Wi-Fi.

## ⚡ Features

* **Web-Based IDE:** Write, save, run, and delete scripts directly from your browser.
* **Massive Payload Support:** Uses the 8MB PSRAM to buffer scripts up to 2MB in size (far exceeding standard microcontroller limits).
* **Live Remote Control:** Virtual keyboard and text injection area to control the target computer in real-time from your smartphone.
* **Optimized Engine:** Supports standard Ducky Script and a custom `BLOCK` mode for high-speed text dumping.
* **Visual Feedback:** RGB NeoPixel integration (Blue=Busy, Green=Ready, White=Processing).
* **Safety:** Emergency Stop button to immediately halt script execution.

## 🛠 Hardware Supported

* **Chip:** ESP32-S3 (N16R8 variant recommended).
* **Specs:** 16MB Flash, 8MB Octal PSRAM (OPI).
* **Boards:**
    * ESP32-S3-DevKitC-1 (N16R8)
    * Waveshare ESP32-S3-Zero
    * Any generic S3 board with native USB access.



> **Note:** You must use the **Native USB** port (often labeled "USB" or connected to GPIO 19/20), not the UART/COM port, for HID injection to work.

## 🚀 Installation (PlatformIO)

This project is designed for **PlatformIO**.

### 1. Project Setup
Ensure your project folder structure looks like this:
```text
ESP32-Ducky-Pro/
├── src/
│   └── main.cpp       # The source code provided
├── data/              # (Optional) For initial file uploads
├── platformio.ini     # Configuration below
└── README.md