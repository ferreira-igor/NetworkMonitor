// ============================================================
//  NetworkMonitor
//  A real-time network monitoring device.
// ============================================================
//  Board: LOLIN D32
//  Partition Scheme: No OTA (Large APP)
// ============================================================

#include <Arduino.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <AsyncUDP.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_sntp.h>
#include <Preferences.h>

#include <WiFiManager.h>     // https://github.com/tzapu/WiFiManager/releases/tag/v2.0.17
#include <AsyncTelegram2.h>  // https://github.com/cotestatnt/AsyncTelegram2/releases/tag/2.3.4
#include <ArduinoJson.h>     // https://github.com/bblanchon/ArduinoJson/releases/tag/v6.21.6

// ============================================================
//  CONFIGURATION
// ============================================================

#define AP_NAME "NetworkMonitor"
#define AP_PASSWORD "123456789"

#define PIN_RESET 13

#define MAX_TOKEN_LENGTH 64
#define MAX_CHAT_ID_LENGTH 32
#define MAX_TIMEZONE_LENGTH 32

#define TELEGRAM_BOT_UPDATE_TIME 1000

#define DEBUG_RTOS false

// ============================================================
//  GLOBAL VARIABLES
// ============================================================

char bot_token[MAX_TOKEN_LENGTH] = { 0 };
char chat_id[MAX_CHAT_ID_LENGTH] = { 0 };
char timezone[MAX_TIMEZONE_LENGTH] = "<-03>3";

int64_t chatid = 0;
bool save_config = false;

Preferences preferences;
WiFiManager wm;
AsyncUDP udp;
WiFiClientSecure client;
AsyncTelegram2 bot(client);

// DHCP packet structure for queue and task
struct DHCPPacket {
  uint16_t length;
  uint8_t data[1500];
};

QueueHandle_t dhcpQueue;

TaskHandle_t dhcpTaskHandle = nullptr;

// Notification structure for queue and task
struct Notification {
  char hostname[64];
  char ip[16];
  char mac[18];
};

QueueHandle_t notificationQueue;

TaskHandle_t notificationTaskHandle = nullptr;

// ============================================================
//  NVS STORAGE - Load/Save configuration
// ============================================================

bool loadConfig() {
  preferences.begin("app_config", true);
  strlcpy(
    bot_token,
    preferences.getString("bot_token", "").c_str(),
    MAX_TOKEN_LENGTH);
  strlcpy(
    chat_id,
    preferences.getString("chat_id", "").c_str(),
    MAX_CHAT_ID_LENGTH);
  strlcpy(
    timezone,
    preferences.getString("timezone", "").c_str(),
    MAX_TIMEZONE_LENGTH);
  preferences.end();
  return (strlen(bot_token) > 0 && strlen(chat_id) > 0);
}

void saveConfig() {
  preferences.begin("app_config", false);
  preferences.putString("bot_token", bot_token);
  preferences.putString("chat_id", chat_id);
  preferences.putString("timezone", timezone);
  preferences.end();
}

void resetConfig() {
  preferences.begin("app_config", false);
  preferences.clear();
  preferences.end();
}

// ============================================================
//  DHCP PACKET PARSING
// ============================================================
//
//  DHCP packet structure (simplified):
//  +-------------+----------------+--------------------------+
//  | Offset      | Field          | Description              |
//  +-------------+----------------+--------------------------+
//  | 0           | op             | 1=Request, 2=Reply       |
//  | 1           | htype          | 1=Ethernet               |
//  | 2           | hlen           | 6 = MAC length           |
//  | 28-33       | chaddr         | Client MAC (6 bytes)     |
//  | 236-239     | magic          | 0x63825363 (DHCP cookie) |
//  | 240+        | options        | Variable length options  |
//  +-------------+----------------+--------------------------+
//
//  Important DHCP Options:
//  - Option 0x0C: Hostname
//  - Option 0x35: Message Type (1=Discover, 2=Offer, 3=Request, ...)
//  - Option 0x32: Requested IP Address
//  - Option 0x36: DHCP Server Identifier
//  - Option 0xFF: End of Options
//  - Option 0x00: Padding
//
//  The function below parses a raw DHCP packet, extracts device info,
//  and triggers a Telegram notification when a DHCP Request is detected.
//
// ============================================================

/*
 * Parses DHCP packets and extracts device information
 *
 * The function validates the packet, extracts MAC address from the
 * chaddr field, then iterates through DHCP options to find hostname,
 * requested IP, and server IP. When a DHCP Request (type 0x03) is
 * detected, a Telegram notification is sent.
 */
void parsePacket(const uint8_t *data, uint16_t length) {
  // Minimum DHCP packet size is 240 bytes (header up to options)
  if (length < 240) {
    Serial.println("Invalid DHCP packet");
    return;
  }

  /*
   * DHCP Magic Cookie:
   * Located at offset 236, this 4-byte value (0x63825363)
   * identifies the packet as a valid DHCP packet.
   * Also known as the "DHCP Magic Cookie" or "Magic Cookie"
   * as defined in RFC 2131.
   */
  if (memcmp(
        &data[236],
        "\x63\x82\x53\x63",
        4)
      != 0) {
    Serial.println("Invalid DHCP Magic Cookie");
    return;
  }

  // Buffers for extracted information
  char device_name[64] = { 0 };  // Hostname from option 0x0C
  char device_ip[16] = { 0 };    // Requested IP from option 0x32
  char device_mac[18] = { 0 };   // MAC from chaddr field
  char server_ip[16] = { 0 };    // DHCP Server IP from option 0x36

  /*
   * DHCP header field offsets:
   * - hlen (hardware address length) at offset 2
   * - chaddr (client MAC address) at offset 28
   */
  constexpr uint8_t CLIENT_ADDR_LEN_OFFSET = 2;
  constexpr uint8_t CLIENT_ADDR_OFFSET = 28;

  uint8_t packet_type = 0;  // Will store DHCP message type

  /*
   * Extract MAC address from chaddr field
   * The 'hlen' field at offset 2 tells us the length of the
   * hardware address. For Ethernet, this is 6 bytes.
   * The MAC starts at offset 28 and is 6 bytes long.
   */
  uint8_t mac_len = data[CLIENT_ADDR_LEN_OFFSET];
  if (mac_len == 6)  // Standard Ethernet MAC length
  {
    snprintf(
      device_mac,
      sizeof(device_mac),
      "%02X:%02X:%02X:%02X:%02X:%02X",
      data[CLIENT_ADDR_OFFSET + 0],
      data[CLIENT_ADDR_OFFSET + 1],
      data[CLIENT_ADDR_OFFSET + 2],
      data[CLIENT_ADDR_OFFSET + 3],
      data[CLIENT_ADDR_OFFSET + 4],
      data[CLIENT_ADDR_OFFSET + 5]);
  }

  /*
   * Parse DHCP Options (starting at offset 240)
   *
   * DHCP options are TLV (Type-Length-Value) encoded:
   * - Type: 1 byte (0x0C, 0x35, 0x32, etc.)
   * - Length: 1 byte (number of bytes in Value)
   * - Value: Variable length (data)
   *
   * The loop iterates through all options until it finds:
   * - 0xFF: End of options
   * - 0x00: Padding (skip)
   */
  uint16_t opp = 240;  // Current position in options

  while (opp < length) {
    uint8_t option = data[opp];

    if (option == 0xFF)
      break;  // End of options
    if (option == 0x00) {
      opp++;
      continue;
    }  // Padding

    if ((opp + 1) >= length) {
      Serial.println("Malformed DHCP option");
      break;
    }

    uint8_t option_len = data[opp + 1];  // Length of value

    if ((opp + 2 + option_len) > length) {
      Serial.println("Truncated DHCP option");
      break;
    }

    /*
     * Process specific DHCP options
     * Each case extracts data and stores it in the appropriate buffer
     */
    switch (option) {
      case 0x0C:  // Host Name - RFC 2132
        {
          // Copy hostname, limit to buffer size
          size_t copy_len = (option_len < (sizeof(device_name) - 1))
                              ? option_len
                              : (sizeof(device_name) - 1);
          memcpy(
            device_name,
            &data[opp + 2],
            copy_len);
          device_name[copy_len] = '\0';
          break;
        }

      case 0x35:  // DHCP Message Type - RFC 2132
        {
          if (option_len >= 1) {
            /*
             * DHCP Message Type values:
             * 1 = DISCOVER  - Client looking for DHCP servers
             * 2 = OFFER     - Server offering an IP
             * 3 = REQUEST   - Client requesting the offered IP
             * 4 = DECLINE   - Client rejecting an offer
             * 5 = ACK       - Server confirming the IP
             * 6 = NAK       - Server denying the request
             * 7 = RELEASE   - Client releasing an IP
             * 8 = INFORM    - Client asking for configuration only
             *
             * We're most interested in type 3 (REQUEST) which indicates
             * a device is actively joining the network.
             */
            packet_type = data[opp + 2];
          }
          break;
        }

      case 0x32:  // Requested IP Address - RFC 2132
        {
          if (option_len >= 4) {
            // IP is stored as 4 bytes in network order
            snprintf(
              device_ip,
              sizeof(device_ip),
              "%u.%u.%u.%u",
              data[opp + 2], data[opp + 3],
              data[opp + 4], data[opp + 5]);
          }
          break;
        }

      case 0x36:  // DHCP Server Identifier - RFC 2132
        {
          if (option_len >= 4) {
            snprintf(
              server_ip,
              sizeof(server_ip),
              "%u.%u.%u.%u",
              data[opp + 2], data[opp + 3],
              data[opp + 4], data[opp + 5]);
          }
          break;
        }
    }

    // Move to next option: type(1) + length(1) + data(option_len)
    opp += option_len + 2;
  }

  // Display extracted information on Serial
  Serial.println();
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
      Serial.println("Discover");
      break;
    case 0x02:
      Serial.println("Offer");
      break;
    case 0x03:
      Serial.println("Request");
      break;
    case 0x04:
      Serial.println("Decline");
      break;
    case 0x05:
      Serial.println("ACK");
      break;
    case 0x06:
      Serial.println("NAK");
      break;
    case 0x07:
      Serial.println("Release");
      break;
    case 0x08:
      Serial.println("Inform");
      break;
    default:
      Serial.println("Unknown");
      break;
  }
  Serial.println("================================");

  /*
   * Send notification for DHCP Request packets
   *
   * DHCP Request (type 0x03) is the key event that indicates
   * a device is joining the network. At this point, the device
   * has already received an offer and is confirming its IP.
   * This is the best time to notify the user about a new device.
   */
  if (packet_type == 0x03) {
    Notification notification;

    strlcpy(
      notification.hostname,
      device_name,
      sizeof(notification.hostname));

    strlcpy(
      notification.ip,
      device_ip,
      sizeof(notification.ip));

    strlcpy(
      notification.mac,
      device_mac,
      sizeof(notification.mac));

    if (xQueueSend(
          notificationQueue,
          &notification,
          0)
        != pdPASS) {
      Serial.println("Notification queue full");
    }
  }
}

// ============================================================
//  FREERTOS TASK: DHCP PROCESSING (Priority 3)
// ============================================================
//
//  This task runs independently from the main loop.
//  It waits for packets in the queue and processes them
//  as soon as they arrive, without blocking other tasks.
//
// ============================================================

void dhcpTask(void *pvParameters) {
  DHCPPacket packet;
  while (true) {
    /*
     * xQueueReceive blocks until a packet is available.
     * portMAX_DELAY means it waits indefinitely.
     * When a packet arrives, it's processed by parsePacket.
     */
    if (xQueueReceive(
          dhcpQueue,
          &packet,
          portMAX_DELAY)) {
      parsePacket(
        packet.data,
        packet.length);
    }
  }
}

// ============================================================
//  UDP CALLBACK - Receives packets on port 67
// ============================================================
//
//  This function is called by AsyncUDP whenever a packet
//  arrives on port 67. It copies the packet and sends it
//  to the DHCP queue for processing.
//
// ============================================================

void onPacket(AsyncUDPPacket packet) {
  DHCPPacket msg;
  if (packet.length() > sizeof(msg.data))
    return;

  msg.length = packet.length();
  memcpy(msg.data,
         packet.data(),
         packet.length());

  /*
   * xQueueSend with 1ms timeout:
   * If the queue is full, the packet is dropped.
   * This is acceptable for monitoring - losing a packet
   * doesn't break the system, and the queue will clear.
   */
  if (xQueueSend(
        dhcpQueue,
        &msg,
        pdMS_TO_TICKS(1))
      != pdPASS) {
    Serial.println("DHCP queue full");
  }
}

// ============================================================
//  FREERTOS TASK: NOTIFICATION (Priority 2)
// ============================================================
//
//  This task waits for notification events generated by the DHCP
//  parser. Whenever a new device sends a DHCP Request packet,
//  a formatted message is sent through the configured notification
//  service (currently Telegram).
//
// ============================================================

void notificationTask(void *pvParameters) {
  Notification notification;
  char message[256];

  while (true) {
    if (xQueueReceive(
          notificationQueue,
          &notification,
          portMAX_DELAY)) {
      snprintf(
        message,
        sizeof(message),
        "Just accessed your network:\n\n"
        "Name: %s\n"
        "IP: %s\n"
        "MAC Address: %s",
        notification.hostname,
        notification.ip,
        notification.mac);

      /*
       * Workaround:
       *
       * After long idle periods, AsyncTelegram2 may attempt to reuse
       * an existing TLS connection that has already been closed by the
       * Telegram server. In this situation, sendTo() may silently fail
       * without reporting an error.
       *
       * Closing the underlying WiFiClientSecure connection before each
       * notification forces a new TCP/TLS session to be established,
       * ensuring a valid connection for every message.
       *
       * Although this introduces a new TLS handshake for each
       * notification, the overhead is negligible for this application,
       * where notifications are infrequent.
       */
      client.stop();

      bot.sendTo(chatid, message);
    }
  }
}

// ============================================================
//  CALLBACKS
// ============================================================

void saveConfigCallback() {
  save_config = true;
}

void timeAvailable(struct timeval *t) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("No time available");
  } else {
    Serial.println("Got time adjustment from NTP!");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
  pinMode(PIN_RESET, INPUT_PULLUP);

  Serial.begin(115200);

  // reset config from NVS
  if (digitalRead(PIN_RESET) == LOW) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    if (digitalRead(PIN_RESET) == LOW) {
      Serial.println("Reseting saved configuration and rebooting...");
      resetConfig();
    }
  }

  // Load config from NVS
  if (!loadConfig()) {
    Serial.println("No configuration found!");
    wm.resetSettings();
  }

  // WiFiManager - Configuration portal
  WiFiManagerParameter custom_text_box1(
    "key_text1",
    "Telegram Token:",
    bot_token,
    MAX_TOKEN_LENGTH);
  WiFiManagerParameter custom_text_box2(
    "key_text2",
    "Telegram Chat ID:",
    chat_id,
    MAX_CHAT_ID_LENGTH);
  WiFiManagerParameter custom_text_box3(
    "key_text3",
    "Timezone:",
    timezone,
    MAX_TIMEZONE_LENGTH);

  wm.addParameter(&custom_text_box1);
  wm.addParameter(&custom_text_box2);
  wm.addParameter(&custom_text_box3);

  std::vector<const char *> menu = { "wifi", "restart", "exit" };
  wm.setMenu(menu);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);

  while (!wm.autoConnect(AP_NAME, AP_PASSWORD)) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Waiting for WiFi configuration.");
  }
  if (save_config) {
    strlcpy(
      bot_token,
      custom_text_box1.getValue(),
      MAX_TOKEN_LENGTH);
    strlcpy(
      chat_id,
      custom_text_box2.getValue(),
      MAX_CHAT_ID_LENGTH);
    strlcpy(
      timezone,
      custom_text_box3.getValue(),
      MAX_TIMEZONE_LENGTH);
    saveConfig();
  }

  // NTP Time Sync
  sntp_set_time_sync_notification_cb(timeAvailable);
  configTzTime(
    timezone,
    "time.google.com",
    "time.cloudflare.com",
    "pool.ntp.org");
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Waiting for NTP sync");
  }

  // mDNS service
  while (!MDNS.begin(AP_NAME)) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Setting up mDNS responder!");
  }

  /*
   * DHCP Queue
   *
   * This queue decouples the UDP receive callback from the DHCP parser.
   * The AsyncUDP callback only copies the received packet and immediately
   * returns, keeping interrupt processing as short as possible.
   *
   * The DHCP task consumes packets from this queue asynchronously.
   * A size of 10 provides enough buffering for bursts of DHCP traffic
   * without consuming excessive RAM.
   */
  dhcpQueue = xQueueCreate(10, sizeof(DHCPPacket));

  while (dhcpQueue == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("DHCP Queue creation failed!");
  }

  /*
   * Telegram Notification Queue
   *
   * After parsing a DHCP packet, only the extracted device information
   * (hostname, IP and MAC) is placed into this queue.
   *
   * Sending HTTPS requests to Telegram is significantly slower than
   * parsing DHCP packets, so using a separate queue prevents network
   * latency from delaying packet processing.
   */
  notificationQueue = xQueueCreate(10, sizeof(Notification));

  while (notificationQueue == nullptr) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Notification Queue creation failed!");
  }

  /*
   * DHCP Processing Task
   *
   * This task waits for packets in the DHCP queue and parses them.
   * Priority 3 gives it precedence over the Arduino loop and the
   * notification task, ensuring DHCP packets are processed with the
   * lowest possible latency.
   *
   */
  BaseType_t dhcp_result = xTaskCreate(
    dhcpTask,
    "DHCP Task",
    6144,
    nullptr,
    3,
    &dhcpTaskHandle);

  while (dhcp_result != pdPASS) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("DHCP Task creation failed!");
  }

  /*
   * Telegram Notification Task
   *
   * This task waits for notification requests generated by the DHCP
   * parser and sends them to Telegram over HTTPS.
   *
   * Since network communication is much slower than packet parsing,
   * this task runs independently with a lower priority (2), allowing
   * DHCP processing to continue uninterrupted even while messages are
   * being transmitted.
   *
   */
  BaseType_t notification_result = xTaskCreate(
    notificationTask,
    "Notification Task",
    8192,
    nullptr,
    2,
    &notificationTaskHandle);

  while (notification_result != pdPASS) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Notification Task creation failed!");
  }

  /*
   * Configure the Telegram client
   *
   * Initialization sequence:
   * 1. Convert the chat ID from the NVS string to int64_t.
   * 2. Configure the Telegram root CA certificate used by WiFiClientSecure
   *    to validate Telegram's HTTPS server certificate.
   * 3. Configure the polling/update interval used internally by the library.
   * 4. Set the bot token.
   * 5. Keep trying to initialize the bot until a successful HTTPS connection
   *    with the Telegram API is established.
   *
   * The application only starts listening for DHCP packets after bot.begin()
   * succeeds. This guarantees that notification requests will never be queued
   * before the Telegram client is fully initialized.
   */
  chatid = atoll(chat_id);

  client.setCACert(telegram_cert);

  bot.setUpdateTime(TELEGRAM_BOT_UPDATE_TIME);

  bot.setTelegramToken(bot_token);

  // Optional: allow insecure TLS if certificate validation fails.
  // bot.enableInsecureFallback();

  /*
   * Wait until the Telegram client is successfully initialized.
   * If the Internet connection is temporarily unavailable, the ESP32
   * will keep retrying every second.
   */
  while (!bot.begin()) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Connecting to Telegram bot...");
  }

  /*
   * Send a startup notification indicating that the monitor
   * is online and ready to receive DHCP packets.
   */
  bot.sendTo(chatid, "NetworkMonitor is online!");

  /*
   * UDP Listener on port 67 (DHCP)
   * Port 67 is the DHCP server port. By listening here,
   * we can capture all DHCP packets on the network.
   */
  while (!udp.listen(67)) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Serial.println("Waiting for UDP listening.");
  }
  Serial.print("UDP listening on ");
  Serial.println(WiFi.localIP());
  udp.onPacket(onPacket);
}

// ============================================================
//  LOOP - Low priority task (Priority 1)
// ============================================================

void loop() {
  // WiFi reconnection check
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected, reconnecting...");
    vTaskDelay(pdMS_TO_TICKS(5000));
    WiFi.reconnect();
  }

  if (DEBUG_RTOS) {
    static uint32_t lastMonitor = 0;

    if (millis() - lastMonitor >= 10000) {
      lastMonitor = millis();

      Serial.printf(
        "[Queue] DHCP: %u/%u | Notification: %u/%u\n",
        uxQueueMessagesWaiting(dhcpQueue),
        uxQueueSpacesAvailable(dhcpQueue) + uxQueueMessagesWaiting(dhcpQueue),
        uxQueueMessagesWaiting(notificationQueue),
        uxQueueSpacesAvailable(notificationQueue) + uxQueueMessagesWaiting(notificationQueue));

      Serial.printf(
        "DHCP Stack Free: %u\n",
        uxTaskGetStackHighWaterMark(dhcpTaskHandle));

      Serial.printf(
        "Notification Stack Free: %u\n",
        uxTaskGetStackHighWaterMark(notificationTaskHandle));
    }
  }

  // Yield CPU to higher priority tasks (DHCP Task)
  vTaskDelay(pdMS_TO_TICKS(10));
}
