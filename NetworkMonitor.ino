/*
Name: NetworkMonitor
Description: An ESP32 based device that sends instant notifications through Telegram whenever a new device joins your network.

Board: LOLIN D32
Partition Scheme: No OTA (Large APP)
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <AsyncUDP.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_sntp.h>

#include <ArduinoJson.h>     // https://github.com/bblanchon/ArduinoJson/releases/tag/v6.21.5
#include <WiFiManager.h>     // https://github.com/tzapu/WiFiManager/releases/tag/v2.0.17
#include <AsyncTelegram2.h>  // https://github.com/cotestatnt/AsyncTelegram2/releases/tag/2.3.1

// Change according to board
#define PIN_RESET 13
#define PIN_LED 5

// Change only if needed
const char *ap_name = "NetworkMonitor";
const char *ap_password = "networkmonitor";
const char *json_config_file = "/config_db.json";

bool format_littlefs_if_failed = true;
bool save_config = false;
bool mute = false;

char bot_token[50] = "";
char chat_id[50] = "";
char time_zone[50] = "UTC0";

String parse_packet;

WiFiManager wm;
AsyncUDP udp;
WiFiClientSecure client;
AsyncTelegram2 bot(client);

enum State {
  READY,
  RECEIVING,
  RECEIVED
};

volatile State state = READY;

// Loads configuration from the LittleFS file system.
bool loadConfigFile(fs::FS &fs) {
  File file = fs.open(json_config_file);
  if (!file || file.isDirectory()) {
    Serial.println("Failed to open configuration file!");
  } else {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
      Serial.print("deserializeJson() failed: ");
      Serial.println(error.c_str());
    } else {
      strcpy(bot_token, doc["bot_token"]);
      strcpy(chat_id, doc["chat_id"]);
      strcpy(time_zone, doc["time_zone"]);
      file.close();
      return true;
    }
  }
  file.close();
  return false;
}

// Saves configuration to the LittleFS file system.
void saveConfigFile(fs::FS &fs) {
  File file = fs.open(json_config_file, FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open configuration file!");
  } else {
    StaticJsonDocument<256> doc;
    doc["bot_token"] = bot_token;
    doc["chat_id"] = chat_id;
    doc["time_zone"] = time_zone;
    serializeJson(doc, file);
  }
  file.close();
}

// Parses DHCP packets to extract device information (MAC, IP, etc.).
void parsePacket(unsigned char *data, int length) {

  const int dhcp_packet_client_addr_len_offset = 2;
  const int dhcp_packet_client_addr_offset = 28;

  const char *hex_digits = "0123456789ABCDEF";

  String device_name = "";
  String device_ip = "";
  String device_mac = "";
  String server_ip = "";

  if (state == RECEIVED) {
    return;
  }

  Serial.println("DHCP Packet");

  for (int i = 0; i < data[dhcp_packet_client_addr_len_offset]; i++) {
    device_mac += hex_digits[data[dhcp_packet_client_addr_offset + i] >> 4];
    device_mac += hex_digits[data[dhcp_packet_client_addr_offset + i] & 15];
    if (i < data[dhcp_packet_client_addr_len_offset] - 1) {
      device_mac += ":";
    }
  }

  Serial.print("MAC Address: ");
  Serial.println(device_mac);

  int opp = 240;

  while (opp < length) {
    switch (data[opp]) {
      case 0x0C:
        for (int i = 0; i < data[opp + 1]; i++) {
          device_name += (char)data[opp + 2 + i];
        }
        Serial.print("Device name: ");
        Serial.println(device_name);
        break;
      case 0x35:
        Serial.print("Packet Type: ");
        switch (data[opp + 2]) {
          case 0x01:
            Serial.println("Discover");
            break;
          case 0x02:
            Serial.println("Offer");
            break;
          case 0x03:
            Serial.println("Request");
            if (state == READY) {
              state = RECEIVING;
            }
            break;
          case 0x05:
            Serial.println("ACK");
            break;
          default:
            Serial.println("Unknown");
        }
        break;
      case 0x32:
        for (int i = 0; i < 4; i++) {
          device_ip += data[opp + 2 + i];
          if (i < 3) {
            device_ip += '.';
          }
        }
        Serial.print("Device IP: ");
        Serial.println(device_ip);
        break;
      case 0x36:
        for (int i = 0; i < 4; i++) {
          server_ip += data[opp + 2 + i];
          if (i < 3) {
            server_ip += '.';
          }
        }
        Serial.print("Server IP: ");
        Serial.println(server_ip);
        break;
      case 0x37:
        Serial.print("Request list: ");
        Serial.write(&data[opp + 2], data[opp + 1]);
        Serial.print("\n");
        break;
      case 0x39:
        Serial.print("Max DHCP message size: ");
        Serial.println((data[opp + 2] << 8) | data[opp + 3]);
        break;
      case 0xff:
        Serial.println("End of options");
        opp = length;
        continue;
      default:
        Serial.print("Unknown option: ");
        Serial.print(data[opp]);
        Serial.print(" (length ");
        Serial.print(data[opp + 1]);
        Serial.println(")");
        Serial.write(&data[opp + 2], data[opp + 1]);
        Serial.print("\n");
    }
    opp += data[opp + 1] + 2;
  }

  if (state == RECEIVING) {
    parse_packet = "Just accessed your network:";
    parse_packet += "\n\n";
    parse_packet += "Name: ";
    parse_packet += device_name;
    parse_packet += "\n";
    parse_packet += "IP: ";
    parse_packet += device_ip;
    parse_packet += "\n";
    parse_packet += "MAC: ";
    parse_packet += device_mac;
    state = RECEIVED;
  }
}

// Queries and returns a list of mDNS services.
String browseServices(const char *service, const char *protocol) {

  String browse_services = "";

  int num_services = MDNS.queryService(service, protocol);

  if (num_services < 0) {
    Serial.println("Error: Service query failed.");
    return "";
  }

  if (num_services == 0) {
    Serial.println("No services found.");
    return "";
  }

  // Preallocate estimated memory
  browse_services.reserve(num_services * 50);  // Adjust size as needed

  for (int i = 0; i < num_services; ++i) {
    browse_services += "Name: ";
    browse_services += MDNS.hostname(i);
    browse_services += "\nIP: ";
    browse_services += MDNS.address(i).toString();  // Convert IP to string
    browse_services += ":";
    browse_services += MDNS.port(i);
    browse_services += "\n\n";
  }

  Serial.println(browse_services);

  return browse_services;
}

void saveConfigCallback() {
  save_config = true;
}

// Prints the current local time.
void printTime() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("No time available (yet)");
  } else {
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
}

// Callback for NTP time synchronization.
void timeAvailable(struct timeval *t) {
  Serial.println("Got time adjustment from NTP!");
  printTime();
}

void setup() {

  // Initializes pins for reset button and LED.

  pinMode(PIN_RESET, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);

  // Initializes serial communication for debugging.

  Serial.begin(115200);

  // Mounts LittleFS and loads configuration.

  if (!LittleFS.begin(format_littlefs_if_failed)) {
    Serial.println("Failed to mount file system...");
  }

  if (!loadConfigFile(LittleFS) || digitalRead(PIN_RESET) == LOW) {
    Serial.println("Forcing configuration mode...");
    wm.resetSettings();
  }

  // Uses WiFiManager for WiFi configuration, sets up Telegram bot credentials.

  WiFi.mode(WIFI_STA);

  WiFiManagerParameter custom_text_box1("key_text1", "Enter your Telegram Token here:", bot_token, 50);
  WiFiManagerParameter custom_text_box2("key_text2", "Enter your Telegram Chat ID here:", chat_id, 50);
  WiFiManagerParameter custom_text_box3("key_text3", "Enter your Timezone here:", time_zone, 50);

  wm.addParameter(&custom_text_box1);
  wm.addParameter(&custom_text_box2);
  wm.addParameter(&custom_text_box3);

  wm.setSaveConfigCallback(saveConfigCallback);

  digitalWrite(PIN_LED, LOW);
  if (!wm.autoConnect(ap_name, ap_password)) {
    Serial.println("Failed to connect and hit timeout! Rebooting...");
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("Connected!");
  }
  digitalWrite(PIN_LED, HIGH);

  if (save_config) {
    strcpy(bot_token, custom_text_box1.getValue());
    strcpy(chat_id, custom_text_box2.getValue());
    strcpy(time_zone, custom_text_box3.getValue());
    saveConfigFile(LittleFS);
  }

  // Configures NTP for time synchronization.

  sntp_set_time_sync_notification_cb(timeAvailable);
  configTzTime(time_zone, "time.google.com", "time.cloudflare.com", "pool.ntp.org");

  // Initializes mDNS service.

  if (!MDNS.begin(ap_name)) {
    Serial.println("Failed to start mDNS service...");
  }

  // Sets up a UDP listener on port 67 to capture DHCP packets.

  if (udp.listen(67)) {
    Serial.print("UDP listening on IP: ");
    Serial.println(WiFi.localIP());
    udp.onPacket([](AsyncUDPPacket packet) {
      parsePacket(packet.data(), packet.length());
    });
  }

  // Initializes the Telegram bot and sends a welcome message.

  client.setCACert(telegram_cert);
  bot.setUpdateTime(1000);
  bot.setTelegramToken(bot_token);

  if (!bot.begin()) {
    Serial.println("Failed to connect to Telegram bot...");
  } else {
    bot.sendTo(atoi(chat_id), "NetworkMonitor is online!"
                              "\n\n"
                              "Type /help to show all available commands.");
  }
}

void loop() {
  // Sends a notification via the Telegram bot if a packet is received.

  if (state == RECEIVED) {
    if (mute == false) {
      bot.sendTo(atoi(chat_id), parse_packet);
    }
    state = READY;
  }

  // Processes incoming Telegram messages for various commands.

  TBMessage bot_msg;

  if (bot.getNewMessage(bot_msg)) {

    String recv_msg = bot_msg.text;

    if (recv_msg.equals("/help")) {
      bot.sendMessage(bot_msg, "Commands:"
                               "\n\n"
                               "/mute - enter silent mode"
                               "\n"
                               "/unmute - exit silent mode"
                               "\n"
                               "/services - shows a list of mDNS services connected to the network");
    } else if (recv_msg.equals("/mute")) {
      mute = true;
      bot.sendMessage(bot_msg, "Bot is silenced until reboot or /unmute command.");
    } else if (recv_msg.equals("/unmute")) {
      mute = false;
      bot.sendMessage(bot_msg, "Bot is no longer silenced.");
    } else if (recv_msg.equals("/services")) {
      bot.sendMessage(bot_msg, "Searching for services!");

      // File Protocols
      bot.sendMessage(bot_msg, browseServices("ftp", "tcp"));
      bot.sendMessage(bot_msg, browseServices("http", "tcp"));
      bot.sendMessage(bot_msg, browseServices("nfs", "tcp"));
      bot.sendMessage(bot_msg, browseServices("sftp-ssh", "tcp"));
      bot.sendMessage(bot_msg, browseServices("smb", "tcp"));
      bot.sendMessage(bot_msg, browseServices("ssh", "tcp"));
      bot.sendMessage(bot_msg, browseServices("telnet", "tcp"));
      bot.sendMessage(bot_msg, browseServices("tunnel", "tcp"));
      bot.sendMessage(bot_msg, browseServices("webdav", "tcp"));

      // Printers and Scanners
      bot.sendMessage(bot_msg, browseServices("printer", "tcp"));
      bot.sendMessage(bot_msg, browseServices("scanner", "tcp"));

      // Google Devices
      // bot.sendMessage(bot_msg, browseServices("googlecast", "tcp"));

      // Amazon Devices
      // bot.sendMessage(bot_msg, browseServices("amazonecho-remote", "tcp"));
      // bot.sendMessage(bot_msg, browseServices("amzn-wplay", "tcp"));

      // Nvidia Shield and Android TV
      // bot.sendMessage(bot_msg, browseServices("androidtvremote", "tcp"));

      // Ubuntu and Raspberry Pi Advertisement
      // bot.sendMessage(bot_msg, browseServices("udisks-ssh", "tcp"));

      // Spotify Connect
      // bot.sendMessage(bot_msg, browseServices("spotify-connect", "tcp"));

      // Philips Hue
      // bot.sendMessage(bot_msg, browseServices("philipshue", "tcp"));

      // Sonos
      // bot.sendMessage(bot_msg, browseServices("sonos", "tcp"));

      // TP-Link
      // bot.sendMessage(bot_msg, browseServices("tplink", "tcp"));

      // Apple Devices
      // bot.sendMessage(bot_msg, browseServices("airplay", "tcp"));
      // bot.sendMessage(bot_msg, browseServices("companion-link", "tcp"));
      // bot.sendMessage(bot_msg, browseServices("hap", "tcp"));
      // bot.sendMessage(bot_msg, browseServices("homekit", "tcp"));

      // Roku Media Player
      // bot.sendMessage(bot_msg, browseServices("roku", "tcp"));
      // bot.sendMessage(bot_msg, browseServices("rsp", "tcp"));

      bot.sendMessage(bot_msg, "Search finished!");
    } else {
      bot.sendMessage(bot_msg, "Type /help to show all available commands.");
    }
  }
}