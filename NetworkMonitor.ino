// FQBN: esp32:esp32:d32:PartitionScheme=no_ota,EraseFlash=all

#include <Arduino.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <AsyncUDP.h>
#include <time.h>
#include <esp_sntp.h>
#include <Preferences.h>

#include <WiFiManager.h>     // https://github.com/tzapu/WiFiManager/releases/tag/v2.0.17
#include <AsyncTelegram2.h>  // https://github.com/cotestatnt/AsyncTelegram2/releases/tag/2.3.4
#include <ArduinoJson.h>     // https://github.com/bblanchon/ArduinoJson/releases/tag/v6.21.6

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
constexpr uint8_t pin_led = 5;     // Active-Low status LED (on during setup)
constexpr uint8_t pin_reset = 13;  // Hold LOW for 10s at boot to wipe config

// ---------------------------------------------------------------------------
// WiFiManager captive portal credentials
// ---------------------------------------------------------------------------
const char *ap_name = "NetworkMonitor";
const char *ap_password = "123456789";

// ---------------------------------------------------------------------------
// Maximum lengths for user-supplied configuration strings.
// These are used both for the persistent buffers and for the WiFiManager
// form fields, so they must match.
// ---------------------------------------------------------------------------
constexpr uint8_t max_token_length = 64;
constexpr uint8_t max_chat_id_length = 64;
constexpr uint8_t max_timezone_length = 64;

// ---------------------------------------------------------------------------
// Persistent configuration buffers.
// Filled either from NVS at boot or from the WiFiManager portal.
// ---------------------------------------------------------------------------
char bot_token[max_token_length] = { 0 };
char chat_id[max_chat_id_length] = { 0 };
char timezone[max_timezone_length] = { 0 };  // POSIX TZ, see:
                                             // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

// Numeric representation of the Telegram chat ID (parsed from chat_id).
int64_t chatid = 0;

// Set to true by the WiFiManager save callback, so setup() knows it must
// persist the values typed in the captive portal.
volatile bool save_config = false;

// ---------------------------------------------------------------------------
// State machine used to coordinate the UDP sniffer (AsyncUDP task) and the
// main loop (Arduino task). Only one notification is processed at a time.
//
//   READY      -> idle, waiting for a DHCP Request
//   RECEIVING  -> a DHCP Request was seen, waiting for the follow-up ACK
//   RECEIVED   -> notification buffer is filled, main loop must send it
// ---------------------------------------------------------------------------
enum State {
  READY,
  RECEIVING,
  RECEIVED
};

volatile State state = READY;

// Buffer shared between the UDP task (writer) and the main loop (reader).
// Kept as a fixed-size char array instead of String to avoid heap
// fragmentation and to guarantee that a partially-written buffer can never
// point to freed memory.
char notification[256];

// ---------------------------------------------------------------------------
// Global objects
// ---------------------------------------------------------------------------
Preferences preferences;
WiFiManager wm;
AsyncUDP udp;
WiFiClientSecure client;
AsyncTelegram2 bot(client);

// ---------------------------------------------------------------------------
// loadConfig()
//
// Reads the three configuration strings from NVS (namespace "app_config").
// Returns true only when all three are non-empty, so the caller can decide
// whether the captive portal must be opened.
// ---------------------------------------------------------------------------
bool loadConfig() {
  preferences.begin("app_config", true);
  strlcpy(bot_token, preferences.getString("bot_token", "").c_str(), max_token_length);
  strlcpy(chat_id, preferences.getString("chat_id", "").c_str(), max_chat_id_length);
  strlcpy(timezone, preferences.getString("timezone", "").c_str(), max_timezone_length);
  preferences.end();
  return (strlen(bot_token) > 0 && strlen(chat_id) > 0 && strlen(timezone) > 0);
}

// ---------------------------------------------------------------------------
// saveConfig()
//
// Persists the current configuration buffers to NVS.
// ---------------------------------------------------------------------------
void saveConfig() {
  preferences.begin("app_config", false);
  preferences.putString("bot_token", bot_token);
  preferences.putString("chat_id", chat_id);
  preferences.putString("timezone", timezone);
  preferences.end();
}

// ---------------------------------------------------------------------------
// resetConfig()
//
// Wipes the "app_config" namespace from NVS. Called when the user holds the
// reset pin at boot, or when no valid configuration is found.
// ---------------------------------------------------------------------------
void resetConfig() {
  preferences.begin("app_config", false);
  preferences.clear();
  preferences.end();
}

// ---------------------------------------------------------------------------
// saveConfigCallback()
//
// Registered with WiFiManager. Fired when the user taps "Save" on the
// captive portal, so setup() knows the form values must be persisted.
// ---------------------------------------------------------------------------
void saveConfigCallback() {
  save_config = true;
}

// ---------------------------------------------------------------------------
// timeAvailable()
//
// SNTP sync notification callback. Only used for logging: prints the first
// valid local time received from the NTP servers.
// ---------------------------------------------------------------------------
void timeAvailable(struct timeval *t) {

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) {
    Serial.println("No time available");
  } else {
    Serial.println("Got time adjustment from NTP!");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
}

// ---------------------------------------------------------------------------
// checkConnection()
//
// Called from loop(). If the WiFi connection drops, starts a 60-second
// timer. If the connection is not restored within that window, the ESP is
// rebooted as a last-resort recovery mechanism.
// ---------------------------------------------------------------------------
void checkConnection() {

  static uint32_t disconnected_since = 0;
  const uint32_t timeout = 60000;

  if (WiFi.status() != WL_CONNECTED) {

    if (disconnected_since == 0) {
      disconnected_since = millis();
    }

    if (millis() - disconnected_since >= timeout) {
      Serial.println("The connection was lost! Rebooting...");
      disconnected_since = 0;
      delay(1000);
      ESP.restart();
    }

  } else {
    disconnected_since = 0;
  }
}

// ---------------------------------------------------------------------------
// parsePacket()
//
// Callback invoked by AsyncUDP for every packet received on port 67.
// Runs on the AsyncUDP task, not on the main loop task.
//
// The function expects a DHCP message (RFC 2131). A DHCP packet is a BOOTP
// header followed by a variable-length options field. We only inspect a
// handful of fields:
//
//   - Magic cookie at offset 236 (must be 0x63 0x82 0x53 0x63)
//   - Client hardware address (chaddr) at offset 28
//   - Options starting at offset 240, TLV-encoded:
//       0x0C  Hostname
//       0x32  Requested IP address
//       0x35  DHCP message type
//       0x36  DHCP server identifier
//
// State machine:
//   READY     -> on DHCP Request, transition to RECEIVING
//   RECEIVING -> after the packet has been parsed, fill the notification
//                buffer and transition to RECEIVED
//   RECEIVED  -> ignore further packets until the main loop resets to READY
//
// The data pointer is owned by AsyncUDP and is only valid during this call,
// so all relevant fields must be copied out before returning.
// ---------------------------------------------------------------------------
void parsePacket(const uint8_t *data, uint16_t length) {

  // Already have a notification pending; drop everything until the main
  // loop has sent it and reset the state back to READY.
  if (state == RECEIVED) {
    return;
  }

  // A BOOTP header is 236 bytes, plus 4 bytes of magic cookie = 240 minimum.
  if (length < 240) {
    Serial.println("Invalid DHCP packet");
    return;
  }

  // Magic cookie at offset 236, see RFC 2131 section 3.
  if (memcmp(&data[236], "\x63\x82\x53\x63", 4) != 0) {
    Serial.println("Invalid DHCP Magic Cookie");
    return;
  }

  // Local scratch buffers. All zero-initialized so they can be safely
  // printed even if the corresponding option is missing.
  char device_name[64] = { 0 };
  char device_ip[16] = { 0 };
  char device_mac[18] = { 0 };
  char server_ip[16] = { 0 };

  // BOOTP layout: byte 2 is hlen (hardware address length),
  // chaddr starts at byte 28.
  constexpr uint8_t client_addr_len_offset = 2;
  constexpr uint8_t client_addr_offset = 28;

  uint8_t packet_type = 0;
  uint8_t mac_len = data[client_addr_len_offset];

  // Only handle Ethernet (6-byte) MAC addresses.
  if (mac_len == 6) {
    snprintf(
      device_mac,
      sizeof(device_mac),
      "%02X:%02X:%02X:%02X:%02X:%02X",
      data[client_addr_offset + 0],
      data[client_addr_offset + 1],
      data[client_addr_offset + 2],
      data[client_addr_offset + 3],
      data[client_addr_offset + 4],
      data[client_addr_offset + 5]);
  }

  // Walk the TLV options starting right after the magic cookie.
  uint16_t opp = 240;

  while (opp < length) {
    uint8_t option = data[opp];

    // 0xFF marks the end of options.
    if (option == 0xFF) {
      break;
    }

    // 0x00 is padding, skip a single byte.
    if (option == 0x00) {
      opp++;
      continue;
    }

    // Every other option is TLV: type, length, value.
    if ((opp + 1) >= length) {
      Serial.println("Malformed DHCP option");
      break;
    }

    uint8_t option_len = data[opp + 1];

    // Defensive bound check: value must fit inside the packet.
    if ((opp + 2 + option_len) > length) {
      Serial.println("Truncated DHCP option");
      break;
    }

    switch (option) {
      // Option 12: hostname (not null-terminated in the packet).
      case 0x0C:
        {
          size_t copy_len;
          size_t max_len = sizeof(device_name) - 1;
          if (option_len < max_len) {
            copy_len = option_len;
          } else {
            copy_len = max_len;
          }
          memcpy(device_name, &data[opp + 2], copy_len);
          device_name[copy_len] = '\0';
          break;
        }
      // Option 53: DHCP message type (single byte).
      case 0x35:
        {
          if (option_len >= 1) {
            packet_type = data[opp + 2];
          }
          break;
        }
      // Option 50: requested IP address (4 bytes).
      case 0x32:
        {
          if (option_len >= 4) {
            snprintf(
              device_ip,
              sizeof(device_ip),
              "%u.%u.%u.%u",
              data[opp + 2],
              data[opp + 3],
              data[opp + 4],
              data[opp + 5]);
          }
          break;
        }
      // Option 54: DHCP server identifier (4 bytes).
      case 0x36:
        {
          if (option_len >= 4) {
            snprintf(
              server_ip,
              sizeof(server_ip),
              "%u.%u.%u.%u",
              data[opp + 2],
              data[opp + 3],
              data[opp + 4],
              data[opp + 5]);
          }
          break;
        }
    }

    opp += option_len + 2;
  }

  // Human-readable dump of the parsed packet, useful when monitoring on
  // the serial console.
  Serial.println("");
  Serial.println("================================");
  Serial.println("DHCP Packet");
  Serial.println("================================");
  Serial.print("MAC Address: ");
  Serial.println(device_mac);
  Serial.print("Hostname: ");
  Serial.println(device_name);
  Serial.print("Requested IP: ");
  Serial.println(device_ip);
  Serial.print("DHCP Server: ");
  Serial.println(server_ip);
  Serial.print("Message Type: ");

  switch (packet_type) {
    case 0x01:
      {
        Serial.println("Discover");
        break;
      }
    case 0x02:
      {
        Serial.println("Offer");
        break;
      }
    case 0x03:
      {
        // DHCP Request: a client is asking for an address. This is the
        // trigger we care about; start tracking the transaction.
        Serial.println("Request");
        if (state == READY) {
          state = RECEIVING;
        }
        break;
      }
    case 0x04:
      {
        Serial.println("Decline");
        break;
      }
    case 0x05:
      {
        Serial.println("ACK");
        break;
      }
    case 0x06:
      {
        Serial.println("NAK");
        break;
      }
    case 0x07:
      {
        Serial.println("Release");
        break;
      }
    case 0x08:
      {
        Serial.println("Inform");
        break;
      }
    default:
      {
        Serial.println("Unknown");
        break;
      }
  }
  Serial.println("================================");

  // Once a DHCP Request has been seen, build the notification message.
  // We do not wait for the server ACK, so the client is reported as soon
  // as it asks for an address.
  if (state == RECEIVING) {
    snprintf(
      notification,
      sizeof(notification),
      "Just accessed your network:\n\n"
      "Name: %s\n"
      "IP: %s\n"
      "MAC Address: %s",
      device_name,
      device_ip,
      device_mac);
    state = RECEIVED;
  }
}

// ---------------------------------------------------------------------------
// setup()
//
// Runs once at boot. Performs:
//   1. GPIO init and status LED
//   2. Serial init
//   3. Optional config wipe via the reset pin
//   4. Configuration load (or captive portal)
//   5. NTP sync
//   6. Telegram bot init
//   7. UDP sniffer on port 67
//   8. Boot notification to Telegram
//
// Every step that depends on an external service blocks until it is
// ready, on purpose: the device must not start operating in an
// inconsistent state.
// ---------------------------------------------------------------------------
void setup() {

  pinMode(pin_led, OUTPUT);
  digitalWrite(pin_led, LOW);  // Active-Low LED. Turns on at the beginning of the setup.

  pinMode(pin_reset, INPUT_PULLUP);

  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("");
  delay(1000);

  // Disable WiFi modem sleep. Required for reliable reception of DHCP
  // broadcast packets, which would otherwise be missed between wakeups.
  WiFi.setSleep(false);

  // If the reset pin is held LOW for 10 seconds at boot, wipe the stored
  // configuration so the captive portal comes up on the next boot.
  if (digitalRead(pin_reset) == LOW) {
    delay(10000);
    if (digitalRead(pin_reset) == LOW) {
      Serial.println("Reseting saved configuration");
      resetConfig();
    }
  }

  if (!loadConfig()) {
    Serial.println("No configuration found!");
    wm.resetSettings();
  }

  // Custom fields added to the WiFiManager captive portal.
  WiFiManagerParameter custom_text_box1("key_text1", "Telegram bot token:", bot_token, max_token_length);
  WiFiManagerParameter custom_text_box2("key_text2", "Telegram chat ID:", chat_id, max_chat_id_length);
  WiFiManagerParameter custom_text_box3("key_text3", "POSIX timezone:", timezone, max_timezone_length);

  wm.addParameter(&custom_text_box1);
  wm.addParameter(&custom_text_box2);
  wm.addParameter(&custom_text_box3);

  // Menu shown on the captive portal. "param" and "info" are omitted on
  // purpose, the custom fields are already in the main form.
  std::vector<const char *> menu = { "wifi", "restart", "exit" };

  wm.setMenu(menu);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);

  if (!wm.autoConnect(ap_name, ap_password)) {
    Serial.println("Could not connect to WiFi! Rebooting...");
    delay(1000);
    ESP.restart();
  }

  // If the user filled the captive portal, copy the form values into the
  // persistent buffers and write them to NVS.
  if (save_config) {
    strlcpy(bot_token, custom_text_box1.getValue(), max_token_length);
    strlcpy(chat_id, custom_text_box2.getValue(), max_chat_id_length);
    strlcpy(timezone, custom_text_box3.getValue(), max_timezone_length);
    saveConfig();
  }

  sntp_set_time_sync_notification_cb(timeAvailable);

  configTzTime(timezone, "time.google.com", "time.cloudflare.com", "pool.ntp.org");

  // Block until the first successful time synchronization. If the ESP is
  // power-cycled together with the router this may take a while, but the
  // device is useless without a valid clock.
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    Serial.println("Waiting for NTP sync");
    delay(1000);
  }

  chatid = atoll(chat_id);

  client.setCACert(telegram_cert);

  bot.setUpdateTime(1000);

  bot.setTelegramToken(bot_token);

  // Block until the bot can talk to the Telegram API.
  while (!bot.begin()) {
    Serial.println("Connecting to Telegram bot...");
    delay(1000);
  }

  // Block until the UDP socket is bound to port 67.
  while (!udp.listen(67)) {
    Serial.println("Waiting for UDP listening...");
    delay(1000);
  }

  Serial.print("UDP Listening on IP: ");
  Serial.println(WiFi.localIP());

  udp.onPacket([](AsyncUDPPacket packet) {
    parsePacket(packet.data(), packet.length());
  });

  bot.sendTo(chatid, "NetworkMonitor is online!");

  digitalWrite(pin_led, HIGH);  // Active-Low LED. Turns off at the end of the setup.
}

// ---------------------------------------------------------------------------
// loop()
//
// Main Arduino task. Three responsibilities, all non-blocking:
//   1. Watchdog for the WiFi connection (checkConnection).
//   2. If a notification is pending, forward it to Telegram and reset the
//      state machine back to READY.
//   3. Poll the Telegram bot for new commands and reply to /info.
//
// A 10 ms delay at the end yields the CPU to the scheduler and feeds the
// watchdog; no explicit yield() is needed.
// ---------------------------------------------------------------------------
void loop() {

  checkConnection();

  // A packet has been parsed by the UDP task; forward it.
  if (state == RECEIVED) {
    bot.sendTo(chatid, notification);
    state = READY;
  }

  TBMessage message;

  if (bot.getNewMessage(message)) {

    Serial.print("Message received: ");
    Serial.println(message.text);

    if (message.messageType == MessageText) {

      // reply is local so it is guaranteed to start empty on every
      // command, and its heap buffer is released when the loop iteration
      // ends.
      String reply;
      reply.reserve(512);

      if (message.text.equalsIgnoreCase("/info")) {

        reply += "NetworkMonitor\n\n";

        reply += "Network\n";
        reply += "IP: ";
        reply += WiFi.localIP().toString();
        reply += "\n";
        reply += "MAC: ";
        reply += WiFi.macAddress();
        reply += "\n";
        reply += "RSSI: ";
        reply += String(WiFi.RSSI());
        reply += " dBm\n\n";

        reply += "System\n";
        reply += "MCU: ";
        reply += ESP.getChipModel();
        reply += "\n";
        reply += "Free heap: ";
        reply += String(ESP.getFreeHeap() / 1024);
        reply += " KB\n";
        reply += "Total heap: ";
        reply += String(ESP.getHeapSize() / 1024);
        reply += " KB";

        bot.sendMessage(message, reply);
        Serial.print("Message sent: ");
        Serial.println(reply);

      } else {

        reply += "Available commands:\n";
        reply += "/info";

        bot.sendMessage(message, reply);
        Serial.print("Message sent: ");
        Serial.println(reply);
      }
    }
  }
  delay(10);
}