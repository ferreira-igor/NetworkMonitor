# NetworkMonitor

An ESP32-based network monitor that listens for DHCP traffic on a local network and sends Telegram notifications when a client requests an IP address.

## Features

- Monitors DHCP traffic on UDP port `67`
- Detects DHCP `Request` messages
- Extracts the device hostname, requested IP address, MAC address, and DHCP server address
- Sends notifications to a Telegram chat over HTTPS
- Sends a startup notification when the monitor is ready
- Provides the `/info` Telegram command
- Configures Wi-Fi, Telegram credentials, and timezone through a captive portal
- Stores configuration in ESP32 NVS
- Synchronizes the system clock using NTP
- Automatically restarts after 60 seconds without a Wi-Fi connection

## Usage

On the first boot, the ESP32 creates a WiFiManager access point:

```text
SSID: NetworkMonitor
Password: 123456789
```

Connect to the access point and open the captive portal at `192.168.4.1`.

Configure:

- Wi-Fi network
- Telegram bot token
- Telegram chat ID
- POSIX timezone

After configuration, NetworkMonitor starts monitoring DHCP traffic.

When a DHCP `Request` is detected, a notification is sent to Telegram:

```text
Just accessed your network:

Name: device
IP: 192.168.1.25
MAC Address: AA:BB:CC:DD:EE:FF
```

Use `/info` in Telegram to view network and ESP32 system information.

## Installation

1. Open `NetworkMonitor.ino` in Arduino IDE.
2. Install the following libraries:
   - WiFiManager `2.0.17`
   - AsyncTelegram2 `2.3.4`
   - ArduinoJson `6.21.6`
3. Select the ESP32 `D32` board.
4. Use the following FQBN:

```text
esp32:esp32:d32:PartitionScheme=no_ota,EraseFlash=all
```

5. Upload the firmware.
6. Open the Serial Monitor at `115200 baud`.

## Notes

- The ESP32 must be connected to the same local network and broadcast domain as the clients being monitored.
- Client isolation, VLAN separation, DHCP relay, or other network configurations may prevent DHCP packets from reaching the ESP32.
- The Telegram bot token and chat ID are stored in ESP32 NVS.
- Hold GPIO `13` LOW for at least 10 seconds during boot to erase the saved configuration.
- GPIO `5` can be used for the optional active-low status LED.