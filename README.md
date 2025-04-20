🔒 ESP32 Fingerprint Sync System using ESP-NOW
This project allows two ESP32 devices to capture, store, and synchronize fingerprint templates using the AS608 (or similar) fingerprint sensor and ESP-NOW communication protocol. Both devices are identical in setup and logic, enabling bi-directional synchronization of fingerprint data without needing Wi-Fi or internet.
.
.
.
🧠 Features
📸 Capture fingerprints with AS608 sensor

💾 Store templates locally on SPIFFS

📡 Automatically send new templates to the other ESP32 via ESP-NOW

🔁 Receive templates from remote ESP32 and store them as remote_*.bin

✅ Match current finger against both local and remote templates

🖥️ Display status and messages on I2C OLED display

🎛️ Physical buttons for:

Save (GPIO 14)

Verify (GPIO 27)
.
.
.
⚙️ Components
ESP32 (x2)

AS608 Fingerprint Sensor (or compatible FPM module)

I2C OLED Display (128x64)

Two push buttons

SPIFFS filesystem for template persistence
.
.
.
🔗 How it works
When a user enrolls a fingerprint on one ESP32, it is saved locally and sent over ESP-NOW to the other ESP32.

The receiving ESP32 saves the fingerprint as a remote template.

Both ESP32s can verify fingerprints against their full set of stored templates (local + remote).

All templates are persisted across reboots and sensor swaps.
