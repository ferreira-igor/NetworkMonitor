# NetworkMonitor

![ESP32](https://img.shields.io/badge/ESP32-Arduino-blue)
![License](https://img.shields.io/badge/License-GPLv3-green)

> An ESP32-based network monitoring device that listens for DHCP traffic and sends Telegram notifications whenever a new device requests an IP address on the local network.

NetworkMonitor is an educational project created to explore:

- ESP32 development
- FreeRTOS multitasking
- DHCP protocol analysis
- AsyncUDP networking
- Telegram Bot API
- Secure HTTPS communication
- WiFiManager
- ESP32 NVS persistent storage

---

## Features

- 📡 Passive DHCP monitoring (UDP Port 67)
- 📱 Telegram notifications for new devices
- 🔍 Extracts:
  - Hostname
  - Requested IP address
  - MAC address
- ⚡ FreeRTOS-based architecture
- 🚀 Asynchronous packet processing
- 🌐 Automatic Wi-Fi configuration with WiFiManager
- 💾 Configuration stored in ESP32 NVS
- 🕒 NTP time synchronization

---

## Hardware

The project was developed and tested using:

- ESP32 LOLIN D32

### Arduino IDE Settings

| Option | Value |
|----------|------|
| Board | LOLIN D32 |
| Flash Size | 4 MB |
| Partition Scheme | No OTA (Large APP) |

---

## Required Libraries

Install the following libraries using the Arduino IDE Library Manager.

| Library | Version |
|---------|---------|
| WiFiManager | 2.0.17 |
| AsyncTelegram2 | 2.3.4 |
| ArduinoJson | 6.21.6 |

The remaining libraries are included with the ESP32 Arduino Core.

---

## Installation

1. [Install the ESP32 Arduino Core.](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)
2. Install the required libraries listed above.
3. Open `NetworkMonitor.ino`.
4. Select your ESP32 board.
5. Compile and upload.

---

## Initial Configuration

On the first boot (or after clearing the saved configuration), the ESP32 creates a Wi-Fi Access Point.

**SSID**

```
NetworkMonitor
```

**Password**

```
123456789
```

Connect to the Access Point and open **http://192.168.4.1** in your web browser (if the configuration portal does not open automatically).

Complete the configuration portal by entering:

- Wi-Fi credentials
- Telegram Bot Token
- Telegram Chat ID
- [Time Zone](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)

The configuration is automatically stored in the ESP32 Non-Volatile Storage (NVS).

---

## Reset Configuration

To erase the saved configuration:

1. Hold **GPIO13** LOW.
2. Power on (or reset) the ESP32.
3. Keep the pin LOW for approximately **10 seconds**.

The stored configuration will be erased and the device will restart into WiFiManager configuration mode.

---

## How It Works

The ESP32 listens for DHCP packets on UDP Port **67**.

When a **DHCP Request** packet is detected, the firmware extracts:

- Device Hostname
- Requested IP Address
- MAC Address

A notification is then sent to Telegram.

### Internal Architecture

```
                 DHCP Packet
                      │
                      ▼
               AsyncUDP Callback
                      │
                      ▼
                 DHCP Queue
                      │
                      ▼
           DHCP Processing Task
              (FreeRTOS Priority 3)
                      │
                      ▼
            Notification Queue
                      │
                      ▼
        Telegram Notification Task
           (FreeRTOS Priority 2)
                      │
                      ▼
             Telegram Bot API
```

The UDP callback only copies received packets into a queue.

DHCP parsing and HTTPS communication are performed by separate FreeRTOS tasks, preventing slow network operations from interfering with packet reception.

---

## Telegram Notification

Example notification:

```text
Just accessed your network:

Hostname: MyLaptop
Requested IP: 192.168.0.125
MAC Address: AA:BB:CC:DD:EE:FF
```

---

## Technical Notes

### DHCP Monitoring

Only **DHCP Request** packets generate notifications.

This avoids duplicate messages generated during the DHCP Discover and DHCP Offer phases.

---

## Author

**Igor Ferreira**
