# NetworkMonitor

[![Build](https://github.com/ferreira-igor/NetworkMonitor/actions/workflows/compile-sketch.yml/badge.svg)](https://github.com/ferreira-igor/NetworkMonitor/actions/workflows/compile-sketch.yml)
![Platform](https://img.shields.io/badge/Platform-ESP32-blue)
![Framework](https://img.shields.io/badge/Framework-Arduino-green)

This project transforms an ESP32-based board into a network monitoring device that detects DHCP (Dynamic Host Configuration Protocol) requests and sends real-time notifications via Telegram. When a new device joins the network and requests an IP address through DHCP, the system captures the request and sends an alert containing the device's hostname, MAC address, and requested IP address.

Designed for network administrators, home automation enthusiasts, and security-conscious users, this monitor provides visibility into all devices connecting to your network without requiring any changes to your existing network infrastructure.

The system features a captive portal for easy WiFi configuration, persistent storage of settings, and robust RTOS task management for reliable operation.

## Features

- DHCP Packet Sniffing: Listens on UDP port 67 to capture DHCP requests from devices joining the network
- Telegram Notifications: Sends instant alerts with device details (hostname, MAC address, requested IP)
- Captive Portal Configuration: Easy WiFi setup via web interface without hardcoding credentials
- Persistent Configuration: Saves Telegram settings and timezone preferences in non-volatile storage
- NTP Time Synchronization: Automatic time syncing from multiple NTP servers
- Reset Button Support: Factory reset capability by holding button during boot
- RTOS Task Management: Dedicated tasks for packet processing and notifications for reliable performance
- WiFi Watchdog: Automatic reboot if WiFi connection is lost
- Debugging Support: Optional RTOS monitoring for queue status and task stack usage

## Hardware

### Required Components

- ESP32 Development Board (Tested on LOLIN D32, compatible with most ESP32 boards)
- USB Cable for programming and power
- Reset Button (optional, can use built-in button or external switch)

### Supported Boards

The code is tested on the LOLIN D32 but should work on any ESP32-based board with the following:
- Built-in LED (configurable via LED_BUILTIN)
- GPIO pin for reset detection (configurable via pin_reset)

## Wiring

The project requires minimal wiring:

| Component | ESP32 Pin | Description |
|-----------|-----------|-------------|
| Built-in LED | LED_BUILTIN | Status indicator (active LOW) |
| Reset Button | GPIO 13 | Pull-up input, press during boot to reset config |

Note: If your board uses a different pin for the built-in LED or reset button, modify the pin_led and pin_reset constants in the code accordingly.

## Flashing

### Method 1: Pre-compiled Binary

1. Download the latest binary from the Releases page

2. Install esptool:
   ```bash
   pipx install esptool
   ```

3. Using esptool:
   ```bash
   esptool --port /dev/ttyUSB0 erase-flash
   esptool --port /dev/ttyUSB0 write-flash 0x0 NetworkMonitor.ino.merged.bin
   ```

   Replace /dev/ttyUSB0 with your actual serial port (or leave empty for auto-detect).

### Method 3: Using Arduino IDE

1. Install ESP32 Core:
   - Open Arduino IDE
   - Go to File > Preferences
   - Add https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json to Additional Boards Manager URLs
   - Go to Tools > Board > Boards Manager
   - Search for ESP32 and install esp32 by Espressif Systems

2. Install Required Libraries:
   - Open Sketch > Include Library > Manage Libraries
   - Install the following libraries:
     - WiFiManager by tzapu (v2.0.17)
     - AsyncTelegram2 by cotestatnt (v2.3.4)
     - ArduinoJson by Benoit Blanchon (v6.21.6)

3. Configure and Upload:
   - Open NetworkMonitor.ino in Arduino IDE
   - Select your ESP32 board: Tools > Board > ESP32 Arduino > [Your Board Model]
   - Select the correct port: Tools > Port > [Your USB Port]
   - Select "Erase All Flash Before Sketch Upload: Enabled"
   - Select "Partition Scheme: Huge APP (3MB No OTA/1 MB SPIFFS)"
   - Click Upload (arrow icon) to compile and flash

4. Monitor Serial Output:
   - Open Tools > Serial Monitor
   - Set baud rate to 115200
   - Observe startup logs and DHCP packet captures

## Startup

### First Boot and Configuration

1. Power the Device: Connect the ESP32 via USB or power supply

2. Connect to Captive Portal:
   - The device creates a WiFi access point named NetworkMonitor
   - Connect your phone or computer to this network
   - Password: 123456789

3. Configure Settings:
   - Open a web browser and navigate to http://192.168.4.1
   - Enter your WiFi credentials
   - Configure the following parameters:
     - Telegram Token: Your bot token (get from @BotFather)
     - Telegram Chat ID: Your chat ID (use @userinfobot to get yours)
     - Timezone: [POSIX timezone string](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv) (default: <-03>3 for GMT-3)
   - Click Save and wait for the device to connect to your WiFi

4. Receive Confirmation: The device will send a NetworkMonitor is online! message to your Telegram

### Creating a Telegram Bot

1. Open Telegram and search for @BotFather
2. Send /newbot and follow the instructions
3. Copy the API Token provided by BotFather
4. Search for @userinfobot and send /start
5. Copy your Chat ID

### Factory Reset

To reset all configuration and return to captive portal mode:
1. Power off the device
2. Press and hold the reset button (GPIO 13)
3. Power on the device
4. Keep the button held for 10 seconds
5. Release the button
6. The device will reboot with all settings cleared

## Notes

### Important Considerations

- Network Topology: The monitor listens for DHCP broadcasts on port 67. It must be on the same network segment as the DHCP server and clients to capture requests.

- TLS Connection Workaround: The code includes a workaround for Telegram TLS connections that may fail after long idle periods. It forces a new TLS session for each notification by calling client.stop() before sending.

- Power Requirements: ESP32 boards typically require 5V via USB or 3.3V from a regulated power supply. Power consumption is minimal during monitoring.

- DHCP Packet Structure: The parser expects standard DHCP packets as defined in RFC 2131. Non-standard implementations may not be correctly parsed.

- LED Indicator:
  - ON (LOW): Booting or configuration mode
  - OFF (HIGH): Normal operation

- Captive Portal Timeout: If the device cannot connect to WiFi within 3 minutes during setup, it will reboot and try again.

### Performance and Limitations

- Packet Processing: The DHCP processing task runs at priority 3 and uses a queue to handle packets from the AsyncUDP callback to avoid blocking the network stack.

- Queue Capacity: Both DHCP and notification queues can hold up to 10 items. If the system is overwhelmed, older packets may be dropped.

- RTOS Task Stack: Tasks are configured with sufficient stack sizes (DHCP: 6144 bytes, Notification: 8192 bytes) for reliable operation.

- Debugging: Enable debug_rtos at the top of the code to monitor queue status and task stack usage in the serial output.

### Security Considerations

- WiFi Credentials: Stored in ESP32's Preferences (non-volatile storage) - not encrypted
- Telegram Token: Also stored in Preferences - treat as sensitive information
- DHCP Monitoring: Passive listening only - does not interfere with network operations
- Captive Portal: Open network with simple password - only active during setup

### Troubleshooting

| Issue | Solution |
|-------|----------|
| Captive portal not appearing | Reset configuration using the reset button |
| Telegram messages not sending | Verify token and chat ID are correct; check internet connectivity |
| No DHCP packets detected | Ensure device is on the same network segment as DHCP clients |
| Serial output shows Queue full | System is overloaded; consider increasing queue sizes |
| WiFi disconnects repeatedly | Check WiFi signal strength; adjust antenna placement |
| LED stays ON | Device is in configuration mode or booting; check serial output |
