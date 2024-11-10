# NetworkMonitor

NetworkMonitor is an ESP32 application that monitors your network for DHCP packets, parses them, and sends notifications to a Telegram chat whenever a new device connects to your network.

## Features

- **Network Monitoring**: Listens for DHCP packets and parses device information such as IP address, MAC address, and hostname.
- **Telegram Notifications**: Sends notifications to a specified Telegram chat with details of new devices connecting to the network.
- **mDNS Service Browsing**: Lists mDNS services connected to the network upon request.
- **Silent Mode**: Allows the bot to enter and exit silent mode via Telegram commands.

## Hardware

- [Wemos Lolin D32](https://www.wemos.cc/en/latest/d32/d32.html)

## Usage

**1. Create a Bot**

- Open the Telegram app on your phone.
- Search for the `BotFather` user and start a chat with it.
- Type `/newbot` and follow the instructions to create a new bot. You’ll need to provide a name and a username for your bot.
- After creating the bot, `BotFather` will provide you with a bot token. Copy this token as you will need it for the configuration.

**2. Get Your Chat ID**

- Search for the `userinfobot` on Telegram and start a chat with it.
- Type `/start` and the bot will reply with your chat ID.

**3. Get the POSIX Time Zone Format**

- Visit the [POSIX time zone database](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv). This CSV file contains time zones in the POSIX format.
- Locate your time zone in the list. For São Paulo, the POSIX format is `<-03>3` (Brasília Time).

## Configuring the Device

**1. Initial Setup**:

- After uploading the code, open the Serial Monitor. The ESP32 will start in Access Point (AP) mode with the SSID `NetworkMonitor`.
- Connect to this network using a computer or smartphone. The password for the AP is `networkmonitor`.
- A captive portal should automatically open in your browser where you can enter your WiFi credentials, Telegram bot token, chat ID, and time zone.
- If the captive portal does not open automatically, type `192.168.4.1` into your web browser to access the configuration page.

**2. Resetting WiFi Credentials:**

- To reset the WiFi credentials or reconfigure the device, press and hold the reset button while powering on the ESP32. The device will reset the WiFi credentials and reopen the configuration portal, allowing you to re-enter the settings.

## Commands

- `/help`: Displays a list of available commands.
- `/mute`: Enters silent mode; notifications will be suppressed.
- `/unmute`: Exits silent mode; notifications will be sent.
- `/services`: Lists mDNS services connected to the network.

## Troubleshooting

- **Failed to Connect**: Ensure that the ESP32 is within range of your WiFi network and that the credentials are correct.
- **No Notifications**: Verify that the Telegram bot token and chat ID are correctly set and that the bot is not muted.
- **Configuration Issues**: Reflash the code and reconfigure the device if you encounter issues with the configuration.

## Acknowledgements

- [donnersm's NetworkMonitor](https://github.com/donnersm/NetworkMonitor)
- [bitluni's IPSnifferPrinter](https://github.com/bitluni/IPSnifferPrinter)
