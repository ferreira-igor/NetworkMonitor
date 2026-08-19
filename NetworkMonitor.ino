/**
 * @file NetworkMonitor.ino
 * @brief DHCP Network Monitor with Telegram notifications
 * 
 * This firmware monitors DHCP requests on a network and sends notifications
 * via Telegram when new devices request IP addresses. It's designed for
 * ESP32-based boards (tested on LOLIN D32) and provides:
 * - WiFi configuration via captive portal
 * - DHCP packet sniffing on port 67
 * - Telegram bot integration for alerts
 * - NTP time synchronization
 * - Persistent configuration storage
 * 
 * @author Igor Ferreira
 * @version 1.0
 * @date 2026
 * 
 * @hardware LOLIN D32 (ESP32)
 * @toolchain Arduino IDE 2.3.10
 * @core ESP32 Arduino Core 3.3.11
 * 
 * @note DHCP monitoring is done by listening on UDP port 67, which
 *       requires the ESP32 to be in promiscuous-like mode (via AsyncUDP)
 */

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

//=============================================================================
// Configuration Constants
//=============================================================================

/** @brief Access point name for WiFi configuration portal */
const char *ap_name = "NetworkMonitor";

/** @brief Access point password for WiFi configuration portal */
const char *ap_password = "123456789";

/** @brief Built-in LED pin for status indication */
constexpr uint8_t pin_led = LED_BUILTIN;

/** @brief Reset pin (pulled up) - hold low during boot to reset config */
constexpr uint8_t pin_reset = 13;

/** @brief Maximum length for Telegram bot token */
constexpr uint8_t max_token_length = 64;

/** @brief Maximum length for Telegram chat ID */
constexpr uint8_t max_chat_id_length = 32;

/** @brief Maximum length for timezone string */
constexpr uint8_t max_timezone_length = 32;

/** @brief Update interval for Telegram bot (milliseconds) */
const uint32_t telegram_bot_update_time = 1000;

/** @brief Enable RTOS debugging output (stack usage, queue stats) */
const bool debug_rtos = false;

//=============================================================================
// Global Variables
//=============================================================================

/** @brief Telegram bot token (persistent storage) */
char bot_token[max_token_length] = { 0 };

/** @brief Telegram chat ID (persistent storage) */
char chat_id[max_chat_id_length] = { 0 };

/** @brief Timezone string (persistent storage) */
char timezone[max_timezone_length] = "<-03>3";  // https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv

/** @brief Numeric chat ID for Telegram (converted from string) */
int64_t chatid = 0;

/** @brief Flag indicating if configuration should be saved */
bool save_config = false;

/** @brief Non-volatile storage for configuration */
Preferences preferences;

/** @brief WiFi manager for captive portal configuration */
WiFiManager wm;

/** @brief Async UDP instance for DHCP packet listening */
AsyncUDP udp;

/** @brief Secure client for Telegram HTTPS connection */
WiFiClientSecure client;

/** @brief Telegram bot instance */
AsyncTelegram2 bot(client);

/**
 * @brief Structure for passing DHCP packets between tasks
 */
struct DHCPPacket {
  uint16_t length;    /**< Actual packet length */
  uint8_t data[1500]; /**< Packet data buffer (max Ethernet MTU) */
};

/** @brief Queue for passing DHCP packets from ISR to processing task */
QueueHandle_t dhcpQueue;

/** @brief Handle for DHCP processing task */
TaskHandle_t dhcpTaskHandle = nullptr;

/**
 * @brief Structure for device notification data
 */
struct Notification {
  char hostname[64]; /**< Device hostname (from DHCP option 12) */
  char ip[16];       /**< Requested IP address (from DHCP option 50) */
  char mac[18];      /**< MAC address (from DHCP header) */
};

/** @brief Queue for passing notifications to Telegram task */
QueueHandle_t notificationQueue;

/** @brief Handle for notification task */
TaskHandle_t notificationTaskHandle = nullptr;

//=============================================================================
// Configuration Management Functions
//=============================================================================

/**
 * @brief Load configuration from non-volatile storage
 * 
 * Reads bot_token, chat_id, and timezone from Preferences.
 * 
 * @return true if bot_token and chat_id are both non-empty strings
 * @return false if configuration is missing or incomplete
 */
bool loadConfig() {
  preferences.begin("app_config", true);
  strlcpy(bot_token, preferences.getString("bot_token", "").c_str(), max_token_length);
  strlcpy(chat_id, preferences.getString("chat_id", "").c_str(), max_chat_id_length);
  strlcpy(timezone, preferences.getString("timezone", "").c_str(), max_timezone_length);
  preferences.end();
  return (strlen(bot_token) > 0 && strlen(chat_id) > 0);
}

/**
 * @brief Save current configuration to non-volatile storage
 * 
 * Writes bot_token, chat_id, and timezone to Preferences.
 */
void saveConfig() {
  preferences.begin("app_config", false);
  preferences.putString("bot_token", bot_token);
  preferences.putString("chat_id", chat_id);
  preferences.putString("timezone", timezone);
  preferences.end();
}

/**
 * @brief Clear all saved configuration
 * 
 * Wipes the entire "app_config" namespace from Preferences.
 * Used when reset button is held during boot.
 */
void resetConfig() {
  preferences.begin("app_config", false);
  preferences.clear();
  preferences.end();
}

//=============================================================================
// DHCP Packet Parsing
//=============================================================================

/**
 * @brief Parse a DHCP packet and extract relevant information
 * 
 * Analyzes a DHCP packet to extract:
 * - MAC address (from DHCP header)
 * - Hostname (from option 12)
 * - Requested IP (from option 50)
 * - DHCP Server IP (from option 54)
 * - Message type (from option 53)
 * 
 * If the packet is a DHCP Request (type 0x03), a notification is queued
 * for sending via Telegram.
 * 
 * @param data Pointer to raw DHCP packet data
 * @param length Length of the packet data
 * 
 * @note DHCP packet structure:
 *       - Bootp header: first 240 bytes
 *       - Magic cookie: 4 bytes (0x63 0x82 0x53 0x63) at offset 236
 *       - Options: start at offset 240, format: [code][len][data]
 * 
 * @see RFC 2131 for DHCP packet specification
 */
void parsePacket(const uint8_t *data, uint16_t length) {

  // Basic validation: minimum packet size
  if (length < 240) {
    Serial.println("Invalid DHCP packet");
    return;
  }

  // Verify DHCP magic cookie
  if (memcmp(&data[236], "\x63\x82\x53\x63", 4) != 0) {
    Serial.println("Invalid DHCP Magic Cookie");
    return;
  }

  char device_name[64] = { 0 };
  char device_ip[16] = { 0 };
  char device_mac[18] = { 0 };
  char server_ip[16] = { 0 };

  // DHCP header layout:
  // Offset 2: hardware address length (typically 6 for Ethernet)
  // Offset 28: client hardware address (chaddr field)
  constexpr uint8_t client_addr_len_offset = 2;
  constexpr uint8_t client_addr_offset = 28;

  uint8_t packet_type = 0;

  // Extract MAC address from DHCP header
  uint8_t mac_len = data[client_addr_len_offset];

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

  // Parse DHCP options (start at offset 240)
  uint16_t opp = 240;

  while (opp < length) {
    uint8_t option = data[opp];

    if (option == 0xFF) {  // End of options
      break;
    }

    if (option == 0x00) {  // Pad option
      opp++;
      continue;
    }

    if ((opp + 1) >= length) {
      Serial.println("Malformed DHCP option");
      break;
    }

    uint8_t option_len = data[opp + 1];

    if ((opp + 2 + option_len) > length) {
      Serial.println("Truncated DHCP option");
      break;
    }

    switch (option) {
      case 0x0C:  // Hostname option
        {
          size_t copy_len = (option_len < (sizeof(device_name) - 1)) ? option_len : (sizeof(device_name) - 1);
          memcpy(device_name, &data[opp + 2], copy_len);
          device_name[copy_len] = '\0';
          break;
        }
      case 0x35:  // DHCP Message Type (required)
        {
          if (option_len >= 1) {
            packet_type = data[opp + 2];
          }
          break;
        }
      case 0x32:  // Requested IP Address
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
      case 0x36:  // DHCP Server Identifier
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

  // Debug output
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
        Serial.println("Request");
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

  // Queue notification only for DHCP Request packets
  // (When a device requests an IP address from the DHCP server)
  if (packet_type == 0x03) {
    Notification notification;

    strlcpy(notification.hostname, device_name, sizeof(notification.hostname));
    strlcpy(notification.ip, device_ip, sizeof(notification.ip));
    strlcpy(notification.mac, device_mac, sizeof(notification.mac));

    if (xQueueSend(notificationQueue, &notification, 0) != pdPASS) {
      Serial.println("Notification queue full");
    }
  }
}

//=============================================================================
// RTOS Task Functions
//=============================================================================

/**
 * @brief DHCP packet processing task
 * 
 * Runs as a separate RTOS task that waits for DHCP packets from the queue
 * and processes them using parsePacket().
 * 
 * @param pvParameters Task parameters (unused)
 */
void dhcpTask(void *pvParameters) {
  DHCPPacket packet;

  while (true) {
    // Wait indefinitely for a packet
    if (xQueueReceive(dhcpQueue, &packet, portMAX_DELAY)) {
      parsePacket(packet.data, packet.length);
    }
  }
}

/**
 * @brief UDP packet callback (called from AsyncUDP context)
 * 
 * Receives raw UDP packets, validates them, and queues them for processing.
 * This runs in the context of the AsyncUDP task.
 * 
 * @param packet The received UDP packet
 * 
 * @note This function is called from an ISR-like context, so it should
 *       be fast and not do heavy processing.
 */
void onPacket(AsyncUDPPacket packet) {
  DHCPPacket msg;

  if (packet.length() > sizeof(msg.data)) {
    return;
  }

  msg.length = packet.length();
  memcpy(msg.data, packet.data(), packet.length());

  // Non-blocking send to queue (drop if full)
  if (xQueueSend(dhcpQueue, &msg, pdMS_TO_TICKS(1)) != pdPASS) {
    Serial.println("DHCP queue full");
  }
}

/**
 * @brief Telegram notification task
 * 
 * Waits for notifications from the queue and sends them via Telegram.
 * Includes a workaround for TLS connection issues after long idle periods.
 * 
 * @param pvParameters Task parameters (unused)
 * 
 * @note The client.stop() workaround forces a new TLS session for each
 *       message, avoiding stale connection issues with Telegram's servers.
 *       This is necessary because AsyncTelegram2 doesn't automatically
 *       handle connection re-establishment.
 */
void notificationTask(void *pvParameters) {
  Notification notification;
  char message[256];

  while (true) {
    // Wait indefinitely for a notification
    if (xQueueReceive(notificationQueue, &notification, portMAX_DELAY)) {
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

//=============================================================================
// Callback Functions
//=============================================================================

/**
 * @brief WiFiManager save configuration callback
 * 
 * Sets the save_config flag to true, indicating that the configuration
 * should be saved after the captive portal exits.
 */
void saveConfigCallback() {
  save_config = true;
}

/**
 * @brief NTP time synchronization callback
 * 
 * Called when the system time is successfully synchronized via NTP.
 * 
 * @param t Pointer to timeval structure (unused)
 */
void timeAvailable(struct timeval *t) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("No time available");
  } else {
    Serial.println("Got time adjustment from NTP!");
    Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  }
}

//=============================================================================
// Setup Function
//=============================================================================

/**
 * @brief Arduino setup function
 * 
 * Initializes the system in the following order:
 * 1. Hardware initialization (pins, serial)
 * 2. Configuration loading and reset handling
 * 3. WiFi setup via captive portal
 * 4. Time synchronization via NTP
 * 5. RTOS task creation
 * 6. Telegram bot initialization
 * 7. DHCP listening on UDP port 67
 * 8. LED status indication (active LOW)
 */
void setup() {
  //-----------------------------------------------------------------------
  // 1. Hardware Initialization
  //-----------------------------------------------------------------------

  pinMode(pin_led, OUTPUT);
  pinMode(pin_reset, INPUT_PULLUP);

  digitalWrite(pin_led, LOW);  // LED on initially

  Serial.begin(115200);

  // Allow time for serial to initialize
  vTaskDelay(pdMS_TO_TICKS(1000));

  //-----------------------------------------------------------------------
  // 2. Configuration Reset on Button Hold
  //-----------------------------------------------------------------------

  // Check if reset button is pressed (LOW due to INPUT_PULLUP)
  if (digitalRead(pin_reset) == LOW) {
    // Wait 10 seconds with button held to confirm reset
    vTaskDelay(pdMS_TO_TICKS(10000));

    if (digitalRead(pin_reset) == LOW) {
      Serial.println("Reseting saved configuration and rebooting...");
      resetConfig();
    }
  }

  //-----------------------------------------------------------------------
  // 3. Load Configuration
  //-----------------------------------------------------------------------

  // If configuration doesn't exist, reset WiFiManager settings to force
  // captive portal mode
  if (!loadConfig()) {
    Serial.println("No configuration found!");
    wm.resetSettings();
  }

  //-----------------------------------------------------------------------
  // 4. WiFi Configuration via Captive Portal
  //-----------------------------------------------------------------------

  // Create WiFiManager parameters for configuration portal
  WiFiManagerParameter custom_text_box1("key_text1", "Telegram Token:", bot_token, max_token_length);
  WiFiManagerParameter custom_text_box2("key_text2", "Telegram Chat ID:", chat_id, max_chat_id_length);
  WiFiManagerParameter custom_text_box3("key_text3", "Timezone:", timezone, max_timezone_length);

  wm.addParameter(&custom_text_box1);
  wm.addParameter(&custom_text_box2);
  wm.addParameter(&custom_text_box3);

  // Customize captive portal menu
  std::vector<const char *> menu = { "wifi", "restart", "exit" };

  wm.setMenu(menu);
  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(180);  // 3 minutes timeout
  wm.setConnectTimeout(30);        // 30 seconds connection timeout

  if (!wm.autoConnect(ap_name, ap_password)) {
    Serial.println("Could not connect to WiFi! Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP.restart();
  }

  // Save configuration if it was changed in the captive portal
  if (save_config) {
    strlcpy(bot_token, custom_text_box1.getValue(), max_token_length);
    strlcpy(chat_id, custom_text_box2.getValue(), max_chat_id_length);
    strlcpy(timezone, custom_text_box3.getValue(), max_timezone_length);
    saveConfig();
  }

  //-----------------------------------------------------------------------
  // 5. Time Synchronization via NTP
  //-----------------------------------------------------------------------

  // Set callback for when time sync completes
  sntp_set_time_sync_notification_cb(timeAvailable);

  // Configure timezone and NTP servers
  configTzTime(timezone, "time.google.com", "time.cloudflare.com", "pool.ntp.org");

  // Block until time is synchronized
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    Serial.println("Waiting for NTP sync");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  //-----------------------------------------------------------------------
  // 6. RTOS Task and Queue Creation
  //-----------------------------------------------------------------------

  // Create queue for DHCP packets (10 items capacity)
  dhcpQueue = xQueueCreate(10, sizeof(DHCPPacket));

  while (dhcpQueue == nullptr) {
    Serial.println("DHCP Queue creation failed!");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Create queue for notifications (10 items capacity)
  notificationQueue = xQueueCreate(10, sizeof(Notification));

  while (notificationQueue == nullptr) {
    Serial.println("Notification Queue creation failed!");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Create DHCP processing task
  // Stack size: 6144 bytes, Priority: 3 (higher than notification task)
  BaseType_t dhcp_result = xTaskCreate(
    dhcpTask,
    "DHCP Task",
    6144,
    nullptr,
    3,
    &dhcpTaskHandle);

  while (dhcp_result != pdPASS) {
    Serial.println("DHCP Task creation failed!");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Create notification task
  // Stack size: 8192 bytes, Priority: 2 (lower than DHCP task)
  BaseType_t notification_result = xTaskCreate(
    notificationTask,
    "Notification Task",
    8192,
    nullptr,
    2,
    &notificationTaskHandle);

  while (notification_result != pdPASS) {
    Serial.println("Notification Task creation failed!");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  //-----------------------------------------------------------------------
  // 7. Telegram Bot Initialization
  //-----------------------------------------------------------------------

  // Convert chat ID from string to numeric
  chatid = atoll(chat_id);

  // Set Telegram server certificate
  client.setCACert(telegram_cert);

  // Set bot update interval
  bot.setUpdateTime(telegram_bot_update_time);

  // Set bot token
  bot.setTelegramToken(bot_token);

  // Block until bot connects
  while (!bot.begin()) {
    Serial.println("Connecting to Telegram bot...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  // Send startup notification
  bot.sendTo(chatid, "NetworkMonitor is online!");

  //-----------------------------------------------------------------------
  // 8. DHCP Listening
  //-----------------------------------------------------------------------

  // Listen on UDP port 67 (DHCP server port)
  // This allows the ESP32 to see DHCP traffic on the network
  while (!udp.listen(67)) {
    Serial.println("Waiting for UDP listening...");
    vTaskDelay(pdMS_TO_TICKS(1000));
  }

  Serial.print("UDP listening on ");
  Serial.println(WiFi.localIP());
  udp.onPacket(onPacket);

  // Turn off LED to indicate successful startup
  digitalWrite(pin_led, HIGH);
}

//=============================================================================
// Loop Function
//=============================================================================

/**
 * @brief Arduino main loop
 * 
 * Runs periodic checks:
 * - Monitors WiFi connection status
 * - If WiFi disconnects, reboots the ESP32
 * - Optionally outputs RTOS debugging information
 */
void loop() {
  // Check WiFi connection status
  if (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(60000));
    Serial.println("WiFi disconnected! Rebooting...");
    ESP.restart();
  }

  // RTOS debugging output (optional)
  if (debug_rtos) {
    static uint32_t lastMonitor = 0;

    if (millis() - lastMonitor >= 10000) {
      lastMonitor = millis();

      // Queue statistics
      Serial.printf(
        "[Queue] DHCP: %u/%u | Notification: %u/%u\n",
        uxQueueMessagesWaiting(dhcpQueue),
        uxQueueSpacesAvailable(dhcpQueue) + uxQueueMessagesWaiting(dhcpQueue),
        uxQueueMessagesWaiting(notificationQueue),
        uxQueueSpacesAvailable(notificationQueue) + uxQueueMessagesWaiting(notificationQueue));

      // Task stack usage (high water mark = minimum remaining stack)
      Serial.printf("DHCP Stack Free: %u\n", uxTaskGetStackHighWaterMark(dhcpTaskHandle));
      Serial.printf("Notification Stack Free: %u\n", uxTaskGetStackHighWaterMark(notificationTaskHandle));
    }
  }
}
