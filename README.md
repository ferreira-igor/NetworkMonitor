# NetworkMonitor

DHCP network monitor for ESP32 with Telegram notifications.

**NetworkMonitor** is an ESP32-based network monitoring project that listens for DHCP traffic and notifies a Telegram chat whenever a device sends a **DHCP Request**. The firmware is designed to run autonomously after the initial configuration and stores its configuration in the ESP32's non-volatile storage.

## Features

- 📡 Monitors DHCP traffic on UDP port `67`
- 🔔 Sends Telegram notifications when a device requests an IP address
- 🏷️ Reports:
  - Device hostname
  - Requested IP address
  - MAC address
- 📶 Wi-Fi configuration through a captive portal using WiFiManager
- 💾 Persistent storage of Telegram and timezone configuration using `Preferences`
- 🕐 Automatic NTP time synchronization
- 🔄 Automatic reboot when the Wi-Fi connection is lost
- 💡 Built-in LED status indication
- 🧵 Uses FreeRTOS tasks and queues to separate packet capture, DHCP parsing, and Telegram notifications
- 🛠️ Optional RTOS diagnostics for queue usage and task stack headroom

## How it works

The firmware is organized around three main stages:

```text
                 ┌─────────────────────┐
                 │      Wi-Fi LAN      │
                 └──────────┬──────────┘
                            │
                       DHCP traffic
                            │
                            ▼
                 ┌─────────────────────┐
                 │      AsyncUDP       │
                 │      UDP :67        │
                 └──────────┬──────────┘
                            │
                      DHCP packet
                            │
                            ▼
                 ┌─────────────────────┐
                 │     DHCP Queue      │
                 └──────────┬──────────┘
                            │
                            ▼
                 ┌─────────────────────┐
                 │     DHCP Task       │
                 │  Packet processing  │
                 └──────────┬──────────┘
                            │
                   DHCP Request only
                            │
                            ▼
                 ┌─────────────────────┐
                 │ Notification Queue  │
                 └──────────┬──────────┘
                            │
                            ▼
                 ┌─────────────────────┐
                 │ Notification Task   │
                 │   Telegram send     │
                 └──────────┬──────────┘
                            │
                            ▼
                 ┌─────────────────────┐
                 │    Telegram Bot     │
                 └─────────────────────┘
```

### DHCP packet processing

NetworkMonitor validates incoming packets before processing them:

1. Checks that the packet is at least 240 bytes.
2. Verifies the DHCP magic cookie.
3. Extracts the client MAC address.
4. Parses DHCP options.
5. Extracts:
   - Option `12` — Hostname
   - Option `50` — Requested IP address
   - Option `53` — DHCP message type
   - Option `54` — DHCP server identifier
6. Only DHCP message type `3` (**Request**) is placed in the notification queue.

This prevents every DHCP packet from generating a Telegram notification.

## Telegram notification

When a DHCP Request is detected, the bot sends a message similar to:

```text
Just accessed your network:

Name: My-Device
IP: 192.168.1.25
MAC Address: AA:BB:CC:DD:EE:FF
```

A startup message is also sent:

```text
NetworkMonitor is online!
```

## Hardware

The current firmware is documented and tested for:

| Component | Specification |
|---|---|
| Microcontroller | ESP32 |
| Tested board | LOLIN D32 |
| Status LED | Built-in LED |
| Reset/configuration button | GPIO 13 |
| Network | 2.4 GHz Wi-Fi |
| DHCP monitoring | UDP port 67 |

### Reset button

GPIO `13` is configured with `INPUT_PULLUP`.

Holding the reset/configuration button **LOW for approximately 10 seconds during boot** clears the stored application configuration.

The WiFiManager configuration is then requested again.

> **Important:** The physical button must connect GPIO 13 to GND.

## Software requirements

The project was developed with:

- Arduino IDE `2.3.10`
- ESP32 Arduino Core `3.3.11`

### Required libraries

Install the following libraries through the Arduino IDE Library Manager or from their respective repositories:

- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [AsyncTelegram2](https://github.com/cotestatnt/AsyncTelegram2)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- AsyncUDP — included with the ESP32 Arduino core
- Preferences — included with the ESP32 Arduino core

The versions referenced by the source code are:

| Library | Version referenced |
|---|---:|
| WiFiManager | 2.0.17 |
| AsyncTelegram2 | 2.3.4 |
| ArduinoJson | 6.21.6 |

## Installation

1. Install Arduino IDE.
2. Install the ESP32 board package.
3. Select the appropriate ESP32 board.
4. Install the required libraries.
5. Open `NetworkMonitor.ino`.
6. Compile and upload the firmware.
7. Open the Serial Monitor at `115200 baud`.
8. On the first boot, connect to the Wi-Fi access point created by NetworkMonitor.
9. Configure:
   - Wi-Fi network
   - Wi-Fi password
   - Telegram Bot Token
   - Telegram Chat ID
   - POSIX timezone
10. Save the configuration.
11. The ESP32 connects to the configured network and starts monitoring DHCP traffic.

## Initial Wi-Fi configuration

If no application configuration is stored, NetworkMonitor starts a WiFiManager captive portal.

The default configuration AP is:

```text
SSID: NetworkMonitor
Password: 123456789
```

After connecting to this access point, open the captive portal and configure the network and Telegram parameters.

The portal also contains the timezone configuration.

## Telegram Bot setup

Create a Telegram bot using **BotFather** and obtain its bot token.

You also need the numeric Telegram chat ID where notifications should be delivered.

Enter both values in the NetworkMonitor configuration portal:

```text
Telegram Token: <your-bot-token>
Telegram Chat ID: <your-chat-id>
Timezone: <-03>3
```

The token and chat ID are stored in the ESP32's NVS/Preferences storage and are not hardcoded in the source code.

## Timezone

NetworkMonitor uses POSIX timezone strings.

The default value in the source is:

```text
<-03>3
```

This represents UTC-3 without daylight-saving adjustment.

For other locations, use an appropriate POSIX timezone string.

## Configuration storage

The firmware uses the ESP32 `Preferences` API with the namespace:

```text
app_config
```

The following values are stored:

| Key | Description |
|---|---|
| `bot_token` | Telegram bot token |
| `chat_id` | Telegram destination chat ID |
| `timezone` | POSIX timezone |

The configuration is automatically loaded at boot.

Holding the reset button for approximately 10 seconds clears the `app_config` namespace.

## FreeRTOS architecture

NetworkMonitor uses separate queues and tasks so that DHCP packet reception does not need to wait for Telegram communication.

### DHCP packet queue

The packet queue contains up to **10 packets**.

Each packet contains:

- Packet length
- Up to 1500 bytes of packet data

The UDP callback performs only lightweight work and places the packet in the queue.

### DHCP task

The DHCP task waits for packets and calls the DHCP parser.

Configuration:

```text
Stack:    6144 bytes
Priority: 3
Queue:    DHCP packet queue
```

### Notification queue

Detected DHCP Requests are converted into a smaller `Notification` structure and placed in a second queue.

The queue can contain up to **10 notifications**.

### Notification task

The notification task waits for DHCP Requests and sends the corresponding Telegram message.

Configuration:

```text
Stack:    8192 bytes
Priority: 2
Queue:    Notification queue
```

The DHCP task has a higher priority because receiving and processing network traffic should take precedence over the comparatively slow Telegram operation.

## Telegram TLS connection workaround

The firmware explicitly calls:

```cpp
client.stop();
```

before sending every notification.

This is intentional.

After long periods without activity, the existing TLS connection can become stale. The workaround forces a new TCP/TLS connection before sending a Telegram message.

The trade-off is an additional TLS handshake for each notification, which is acceptable for this application because DHCP notifications are expected to be relatively infrequent.

## Wi-Fi failure handling

The main loop checks:

```cpp
WiFi.status() != WL_CONNECTED
```

If Wi-Fi remains disconnected, the firmware waits approximately 60 seconds and then restarts the ESP32.

This allows the device to recover automatically from persistent network connectivity problems.

## LED status

The built-in LED is used as a simple startup/status indicator.

During initialization:

```text
LED ON
```

After Wi-Fi, NTP, Telegram and DHCP monitoring have successfully initialized:

```text
LED OFF
```

The LED is assumed to be active-low, as used by the current firmware.

## Debugging

RTOS diagnostics can be enabled by changing:

```cpp
const bool debug_rtos = false;
```

to:

```cpp
const bool debug_rtos = true;
```

When enabled, the firmware periodically reports:

- DHCP queue usage
- Notification queue usage
- Remaining DHCP task stack
- Remaining notification task stack

Example:

```text
[Queue] DHCP: 0/10 | Notification: 0/10
DHCP Stack Free: 4200
Notification Stack Free: 6200
```

The stack values are FreeRTOS high-water marks, representing the minimum amount of unused stack observed.

## DHCP monitoring considerations

NetworkMonitor listens for UDP traffic on port `67`.

DHCP normally uses:

```text
Client → UDP 68
Server → UDP 67
```

The ESP32 therefore listens on the DHCP server port to observe DHCP server-directed traffic.

However, **DHCP packet visibility depends on the network topology and Wi-Fi/AP implementation**. A normal Wi-Fi client is not guaranteed to receive every DHCP frame exchanged by other clients simply because it is listening on UDP port 67.

For reliable monitoring, the ESP32 must be on a network where the relevant DHCP traffic is actually delivered to its network interface.

This project should therefore be tested against the specific router, access point, switch and network configuration being used.

## Limitations

- The project is currently focused on IPv4 DHCP.
- It does not maintain a persistent database of devices.
- It does not correlate DHCP Requests with previous DHCP leases.
- Repeated DHCP Requests from the same device can generate repeated Telegram notifications.
- DHCP traffic visibility depends on the network infrastructure.
- Telegram notifications require an active Internet connection.
- A new TLS connection is established for each notification.
- The current configuration AP password is hardcoded and should be reviewed before deployment.

## Configuration constants

Important constants currently defined in `NetworkMonitor.ino` include:

| Constant | Current value | Purpose |
|---|---:|---|
| `ap_name` | `NetworkMonitor` | Configuration AP name |
| `ap_password` | `123456789` | Configuration AP password |
| `pin_reset` | `13` | Configuration reset input |
| `max_token_length` | `64` | Telegram token buffer |
| `max_chat_id_length` | `32` | Telegram chat ID buffer |
| `max_timezone_length` | `32` | Timezone buffer |
| `telegram_bot_update_time` | `1000 ms` | Telegram bot update interval |
| DHCP queue size | `10` | Maximum queued DHCP packets |
| Notification queue size | `10` | Maximum queued notifications |
| DHCP task stack | `6144` | DHCP task stack size |
| Notification task stack | `8192` | Telegram task stack size |

## Future improvements

Possible improvements for future versions include:

- Device deduplication to avoid repeated alerts
- Persistent device history
- Web interface for monitoring detected devices
- MQTT support
- Home Assistant integration
- Configurable notification rules
- Configurable DHCP monitoring filters
- Better network-interface handling for DHCP broadcast traffic
- More secure configuration AP provisioning
- OTA firmware updates
- More detailed DHCP option parsing
- Device manufacturer identification from MAC/OUI
- Optional local web dashboard
