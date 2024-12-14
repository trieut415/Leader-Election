/* Infrared IR/UART TX or RX example
   Created November 2019 -- Emily Lam
   Updated April 13, 2023 -- ESP IDF v 5.0 port

   *Not reliable to run both TX and RX parts at the same time* 

   PWM Pulse          -- pin 26 -- A0
   UART Transmitter   -- pin 25 -- A1
   UART Receiver      -- pin 34 -- A2

   Hardware interrupt -- pin 4 -- A5
   ID Indicator       -- pin 13 -- Onboard LED

   Red LED            -- pin 33
   Green LED          -- pin 32
   Blue LED           -- Pin 14

   Features:
   - Sends UART payload -- | START | myColor | myID | Checksum? |
   - Outputs 38kHz using PWM for IR transmission
   - Onboard LED blinks device ID (myID)
   - Button press to change device ID
   - RGB LED shows traffic light state (red, green, yellow)
   - Timer controls traffic light state (r - 10s, g - 10s, y - 10s)
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>             // Added in 2023..
#include <inttypes.h>           // Added in 2023
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"     // Added in 2023
#include "esp_log.h"
#include "esp_system.h"         // Added in 2023
#include "driver/rmt_tx.h"      // Modified in 2023
#include "soc/rmt_reg.h"        // Not needed?
#include "driver/uart.h"
//#include "driver/periph_ctrl.h"
#include "driver/gptimer.h"     // Added in 2023
#include "soc/clk_tree_defs.h"           // Added in 2023
#include "driver/gpio.h"        // Added in 2023
#include "driver/mcpwm_prelude.h"// Added in 2023
#include "driver/ledc.h"        // Added in 2023

// MCPWM defintions -- 2023: modified
#define MCPWM_TIMER_RESOLUTION_HZ 10000000 // 10MHz, 1 tick = 0.1us
#define MCPWM_FREQ_HZ             38000    // 38KHz PWM -- 1/38kHz = 26.3us
#define MCPWM_FREQ_PERIOD         263      // 263 ticks = 263 * 0.1us = 26.3us
#define MCPWM_GPIO_NUM            25

// LEDC definitions -- 2023: modified 
// NOT USED / altnernative to MCPWM above
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          25
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           6     // Set duty resolution to 6 bits
#define LEDC_DUTY               32    // Set duty to 50%. ((2^6) - 1) * 50% = 32
#define LEDC_FREQUENCY          38000 // Frequency in Hertz. 38kHz

// UART definitions -- 2023: no changes
#define UART_TX_GPIO_NUM 26 // A0
#define UART_RX_GPIO_NUM 34 // A2
#define BUF_SIZE (1024)
#define BUF_SIZE2 (32)

// Hardware interrupt definitions -- 2023: no changes
#define GPIO_INPUT_IO_1       4
#define ESP_INTR_FLAG_DEFAULT 0
#define GPIO_INPUT_PIN_SEL    1ULL<<GPIO_INPUT_IO_1

// LED Output pins definitions -- 2023: minor changes
#define BLUEPIN   33
#define REDPIN    32
#define GREENPIN  14
#define ONBOARD   13
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<BLUEPIN) | (1ULL<<GREENPIN) | (1ULL<<REDPIN) | (1ULL<<ONBOARD) )

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_websocket_client.h"
#include "esp_event.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

// Default ID/color 
#define ID 2
#define COLOR 'G'

// ID0 - josh, ID1 - noah, ID2 - trieu
#define MSG_ELECTION 0x01
#define MSG_ALIVE 0x02
#define MSG_VICTORY 0x03
#define MSG_KEEP_ALIVE 0x04
#define MSG_SERVER 0x05

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define EXAMPLE_ESP_WIFI_SSID "Group_6"
#define EXAMPLE_ESP_WIFI_PASS "smartsys"
#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define MAXIMUM_MEMBERS 3

const char *esp_ip_list[] = {"192.168.1.102", "192.168.1.126", "192.168.1.130"};
int esp_port_list[] = {3001, 3002, 3003};

typedef enum {
    leader = 0,         // blue (leader)
    not_leader = 1,     // red (non-leader)
    timeout = 2,        // timeout (yellow)
    idle = 3,           // waiting state
    election = 4        // in election
} LeaderState;

#define UNDEFINED_LEADER_ID -1
bool leader_not_detected = true;        // Indicates if no leader has been detected
bool received_alive_response = false;   // Indicates if a response was received during election
int current_leader_id = UNDEFINED_LEADER_ID;  // Initialize with an undefined value

bool is_member = false; // Existing variable
bool previous_is_member = false; // New variable to track previous state
// To:
LeaderState previousState = -1;
LeaderState currentState = idle;


#define MAX_LEADER_ID_LEN 40  // Adjust as needed
char previous_leader_id[MAX_LEADER_ID_LEN] = "";
esp_websocket_client_handle_t client;

const char *espID = "1"; // Set this ID for each device (change for each ESP)
static const char *TAG = "wifi station";
static const char *TAG_UART = "UART station";

static int s_retry_num = 0;


TimerHandle_t election_timer;
bool election_timer_expired = false;  // Flag to indicate if the timer has expired

TimerHandle_t keep_alive_timer;
bool keep_alive_timer_expired = false;  // Flag to indicate if the timer has expired

// Variables for my ID, minVal and status plus string fragments
char start = 0x1B;              // START BYTE that UART looks for
char myID = (char) ID;
int votedID = -1;
char type = (char) COLOR;
int len_out = 5;

SemaphoreHandle_t mux = NULL; // 2023: no changes
static EventGroupHandle_t s_wifi_event_group; /* FreeRTOS event group to signal when we are connected*/


static void pwm_init() {

  // Create timer
  mcpwm_timer_handle_t pwm_timer = NULL;
  mcpwm_timer_config_t pwm_timer_config = {
      .group_id = 0,
      .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
      .resolution_hz = MCPWM_TIMER_RESOLUTION_HZ,
      .period_ticks = MCPWM_FREQ_PERIOD,
      .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
  };
  ESP_ERROR_CHECK(mcpwm_new_timer(&pwm_timer_config, &pwm_timer));

  // Create operator
  mcpwm_oper_handle_t oper = NULL;
  mcpwm_operator_config_t operator_config = {
      .group_id = 0, // operator must be in the same group to the timer
  };
  ESP_ERROR_CHECK(mcpwm_new_operator(&operator_config, &oper));

  // Connect timer and operator
  ESP_ERROR_CHECK(mcpwm_operator_connect_timer(oper, pwm_timer));

  // Create comparator from the operator
  mcpwm_cmpr_handle_t comparator = NULL;
  mcpwm_comparator_config_t comparator_config = {
      .flags.update_cmp_on_tez = true,
  };
  ESP_ERROR_CHECK(mcpwm_new_comparator(oper, &comparator_config, &comparator));

  // Create generator from the operator
  mcpwm_gen_handle_t generator = NULL;
  mcpwm_generator_config_t generator_config = {
      .gen_gpio_num = MCPWM_GPIO_NUM,
  };
  ESP_ERROR_CHECK(mcpwm_new_generator(oper, &generator_config, &generator));

  // set the initial compare value, so that the duty cycle is 50%
  ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(comparator,132));
  // CANNOT FIGURE OUT HOW MANY TICKS TO COMPARE TO TO GET 50%

  // Set generator action on timer and compare event
  // go high on counter empty
  ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(generator,
                  MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
  // go low on compare threshold
  ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(generator,
                  MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW)));

  // Enable and start timer
  ESP_ERROR_CHECK(mcpwm_timer_enable(pwm_timer));
  ESP_ERROR_CHECK(mcpwm_timer_start_stop(pwm_timer, MCPWM_TIMER_START_NO_STOP));

}

static void uart_init() {
  // Basic configs
  const uart_config_t uart_config = {
      .baud_rate = 1200, // Slow BAUD rate
      .data_bits = UART_DATA_8_BITS,
      .parity    = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT
  };
  uart_param_config(UART_NUM_1, &uart_config);

  // Set UART pins using UART0 default pins
  uart_set_pin(UART_NUM_1, UART_TX_GPIO_NUM, UART_RX_GPIO_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

  // Reverse receive logic line
  uart_set_line_inverse(UART_NUM_1,UART_SIGNAL_RXD_INV);

  // Install UART driver
  uart_driver_install(UART_NUM_1, BUF_SIZE * 2, 0, 0, NULL, 0);
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
      esp_wifi_connect();
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
      if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY)
      {
          esp_wifi_connect();
          s_retry_num++;
          ESP_LOGI(TAG, "retry to connect to the AP");
      }
      else
      {
          xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
      }
      ESP_LOGI(TAG, "connect to the AP fail");
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

void wifi_init_sta(void)
{
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &event_handler,
                                                      NULL,
                                                      &instance_got_ip));

  wifi_config_t wifi_config = {
      .sta = {
          .ssid = EXAMPLE_ESP_WIFI_SSID,
          .password = EXAMPLE_ESP_WIFI_PASS,
      },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
    * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE,
                                          pdFALSE,
                                          portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
    * happened. */
  if (bits & WIFI_CONNECTED_BIT)
  {
      ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
  }
  else if (bits & WIFI_FAIL_BIT)
  {
      ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
  }
  else
  {
      ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
}

// A simple structure to pass "events" to main task -- 2023: modified
typedef struct {
    uint64_t event_count;
} example_queue_element_t;

// Create a FIFO queue for timer-based events -- Modified
example_queue_element_t ele;
QueueHandle_t timer_queue;

// System tags for diagnostics -- 2023: modified
//static const char *TAG_SYSTEM = "ec444: system";       // For debug logs
static const char *TAG_TIMER = "ec444: timer";         // For timer logs
/////////////////////////////////////////////////////////////


// Checksum -- 2023: no changes
char genCheckSum(char *p, int len) {
  char temp = 0;
  for (int i = 0; i < len; i++){
    temp = temp^p[i];
  }
  // printf("%X\n",temp);  // Diagnostic

  return temp;
}
bool checkCheckSum(uint8_t *p, int len) {
  char temp = (char) 0;
  bool isValid;
  for (int i = 0; i < len-1; i++){
    temp = temp^p[i];
  }
  // printf("Check: %02X ", temp); // Diagnostic
  if (temp == p[len-1]) {
    isValid = true; }
  else {
    isValid = false; }
  return isValid;
}

// GPIO init for LEDs -- 2023: modified
static void led_init() {
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
}

void send_task(char message_type) {
    char *data_out = (char *) malloc(len_out);
    if (data_out == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for data_out");
        return;
    }

    data_out[0] = start;             // Start byte
    data_out[1] = message_type;      // Message type (Election, Alive, Victory)
    data_out[2] = (char) myID;       // Device ID
    data_out[3] = (char) votedID;    // Voted ID
    data_out[4] = genCheckSum(data_out, len_out - 1); // Checksum

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Socket creation failed, errno: %d", errno);
        free(data_out);
        return;
    }

    // Broadcast to all devices, regardless of ID
    for (int i = 0; i < MAXIMUM_MEMBERS; i++) {
        if (i == ID) continue;

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(esp_port_list[i]);
        int ret = inet_pton(AF_INET, esp_ip_list[i], &server_addr.sin_addr);

        if (ret <= 0) {
            ESP_LOGE(TAG, "Invalid IP address format: %s", esp_ip_list[i]);
            continue;
        }

        ssize_t sent_bytes = sendto(sockfd, data_out, len_out, 0,
                                    (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (sent_bytes < 0) {
            ESP_LOGE(TAG, "Failed to send to %s:%d, errno: %d (%s)",
                    esp_ip_list[i], esp_port_list[i], errno, strerror(errno));
        }
    }

    close(sockfd);
    free(data_out);
}


void start_election_timer(uint32_t timeout_ms);
void stop_election_timer();

void start_keep_alive_timer(uint32_t timeout_ms);
void reset_keep_alive_timer(uint32_t timeout_ms);
void stop_keep_alive_timer();


// AI-Generated
void send_message_to_server(uint8_t *message, int message_len) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        ESP_LOGE(TAG, "Socket creation failed, errno: %d", errno);
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(4000); // Server's UDP port

    // Replace with your server's IP address
    const char *server_ip = "192.168.1.103"; // Update with the actual IP

    int ret = inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Invalid server IP address: %s", server_ip);
        close(sockfd);
        return;
    }

    ssize_t sent_bytes = sendto(sockfd, message, message_len, 0,
                                (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sent_bytes < 0) {
        ESP_LOGE(TAG, "Failed to send to server, errno: %d (%s)", errno, strerror(errno));
    } else {
        ESP_LOGI(TAG, "Message sent to server");
    }

    close(sockfd);
}

// Receive task -- looks for Start byte then stores received values -- 2023: minor changes
void recv_task(void *pvParameters) {
    int input_port = *((int *) pvParameters);
    uint8_t *data_in = (uint8_t *) malloc(len_out);

    // Create a UDP socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        free(data_in);
        return;
    }

    // Set up the server address structure
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(input_port);

    // Bind the socket to the specified port
    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        free(data_in);
        return;
    }

    while (1) {
        // Receive data
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int len_in = recvfrom(sockfd, data_in, BUF_SIZE2, 0,
                              (struct sockaddr *)&client_addr, &client_addr_len);

        // Process data if valid length is received
        if (len_in >= len_out) {
            int nn = 0;

            // Locate the start marker in data_in
            while (nn < len_in && data_in[nn] != start) {
                nn++;
            }

            // Ensure enough bytes remain after locating start
            if (nn <= len_in - len_out) {
                uint8_t copied[len_out];
                memcpy(copied, data_in + nn, len_out * sizeof(uint8_t));

                // Checksum validation
                if (checkCheckSum(copied, len_out)) {
                    char message_type = copied[1];
                    char sender_id = copied[2];  // Extract sender's ID if needed

                    // Process each message type
                    switch (message_type) {
                        case MSG_ELECTION:
                            if (sender_id < myID) {
                                send_task(MSG_ALIVE);
                                // Start an election if not already in election state
                                if (currentState != election) {
                                    ESP_LOGI(TAG, "Received ELECTION message from lower ID. Starting my own election.");
                                    currentState = election;
                                    leader_not_detected = true;
                                    received_alive_response = false;
                                    send_task(MSG_ELECTION);
                                    start_election_timer(5000);
                                }
                            }
                            // No action needed if sender_id > myID
                            break;

                        case MSG_ALIVE:
                            if (currentState == election) {
                                received_alive_response = true;
                            }
                            break;

                        case MSG_VICTORY:
                            current_leader_id = sender_id;
                            stop_election_timer();

                            if (sender_id == myID) {
                                currentState = leader;
                                stop_keep_alive_timer();
                            } else {
                                if (sender_id < myID) {
                                    // Detected lower-ID device as leader; start new election
                                    currentState = election;
                                    leader_not_detected = true;
                                    received_alive_response = false;
                                    send_task(MSG_ELECTION);
                                    start_election_timer(5000);
                                } else {
                                    currentState = not_leader;
                                    start_keep_alive_timer(6000);
                                }
                            }
                            break;


                        case MSG_KEEP_ALIVE:
                            if (currentState == not_leader) {
                                reset_keep_alive_timer(6000); // Reset the keep-alive timer
                            }
                            // Ignore KEEP_ALIVE messages during election
                            break;

                        case MSG_SERVER:
                            if(currentState == leader){
                                // Send message to server
                                send_message_to_server(copied, len_out);
                            } else {
                                ESP_LOGI(TAG, "Received MSG_SERVER but not the leader, ignoring.");
                            }
                            break;

                        default:
                            // Unknown message type
                            ESP_LOGI(TAG, "Unknown message type received");
                            break;
                    }
                }
            }
        }
    }

    // Clean up
    close(sockfd);
    free(data_in);
}

// LED task to blink onboard LED based on ID -- 2023: no changes
void id_task(){
  while(1) {
    for (int i = 0; i < (int) myID; i++) {
      gpio_set_level(ONBOARD,1);
      vTaskDelay(200 / portTICK_PERIOD_MS);
      gpio_set_level(ONBOARD,0);
      vTaskDelay(200 / portTICK_PERIOD_MS);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void election_timer_callback(TimerHandle_t xTimer) {
    election_timer_expired = true;  // Set flag to true when the timer expires
    ESP_LOGI(TAG_TIMER, "Election timer expired");
}

void start_election_timer(uint32_t timeout_ms) {
    if (election_timer == NULL) {
        // Create the timer if it hasn’t been created yet (one-shot timer)
        election_timer = xTimerCreate("ElectionTimer", pdMS_TO_TICKS(timeout_ms), pdFALSE, (void *)0, election_timer_callback);
    }
    
    if (election_timer != NULL) {
        election_timer_expired = false;  // Reset the flag when starting the timer
        xTimerStart(election_timer, 0);  // Start the timer
    } else {
        ESP_LOGE(TAG_TIMER, "Failed to create/start election timer");
    }
}

void stop_election_timer() {
    if (election_timer != NULL) {
        xTimerStop(election_timer, 0);  // Stop the timer if it's running
        election_timer_expired = false;  // Reset the flag
    }
}

void keep_alive_timer_callback(TimerHandle_t xTimer) {
    keep_alive_timer_expired = true;
    ESP_LOGI(TAG_TIMER, "Keep-alive timer expired");
}

void start_keep_alive_timer(uint32_t timeout_ms) {
    if (keep_alive_timer == NULL) {
        keep_alive_timer = xTimerCreate("KeepAliveTimer", pdMS_TO_TICKS(timeout_ms), pdFALSE, NULL, keep_alive_timer_callback);
    }
    if (keep_alive_timer != NULL) {
        keep_alive_timer_expired = false;
        xTimerStart(keep_alive_timer, 0);
    } else {
        ESP_LOGE(TAG_TIMER, "Failed to create/start keep-alive timer");
    }
}

void reset_keep_alive_timer(uint32_t timeout_ms) {
    if (keep_alive_timer != NULL) {
        xTimerStop(keep_alive_timer, 0);
        keep_alive_timer_expired = false;
        xTimerChangePeriod(keep_alive_timer, pdMS_TO_TICKS(timeout_ms), 0);
        xTimerStart(keep_alive_timer, 0);
    }
}

void stop_keep_alive_timer() {
    if (keep_alive_timer != NULL) {
        xTimerStop(keep_alive_timer, 0);
        keep_alive_timer_expired = false;
    }
}

void bully_algorithm_task(void *pvParameters) {
    TickType_t last_send_time = xTaskGetTickCount();
    previousState = -1; // Initialize to an invalid state

    while (1) {
        if (previousState != currentState) {
            // State has changed; update LEDs accordingly
            switch (currentState) {
                case idle:
                    ESP_LOGI(TAG, "State changed to IDLE");
                    // Set LEDs for idle state (all off or as desired)
                    gpio_set_level(BLUEPIN, 0);
                    gpio_set_level(REDPIN, 0);
                    gpio_set_level(GREENPIN, 0);
                    break;

                case election:
                    ESP_LOGI(TAG, "State changed to ELECTION");
                    // Set LEDs for election state (green)
                    gpio_set_level(BLUEPIN, 0);
                    gpio_set_level(REDPIN, 0);
                    gpio_set_level(GREENPIN, 1);
                    break;

                case leader:
                    ESP_LOGI(TAG, "State changed to LEADER");
                    // Set LEDs for leader state (blue)
                    gpio_set_level(BLUEPIN, 1);
                    gpio_set_level(REDPIN, 0);
                    gpio_set_level(GREENPIN, 0);
                    break;

                case not_leader:
                    ESP_LOGI(TAG, "State changed to NOT_LEADER");
                    // Set LEDs for not_leader state (red)
                    gpio_set_level(BLUEPIN, 0);
                    gpio_set_level(REDPIN, 1);
                    gpio_set_level(GREENPIN, 0);
                    break;

                default:
                    ESP_LOGW(TAG, "Unknown state");
                    break;
            }
            previousState = currentState; // Update previousState after handling the change
        }

        // State machine logic
        switch (currentState) {
            case idle:
                // Transition to election state upon startup
                currentState = election;
                leader_not_detected = true;
                received_alive_response = false;
                send_task(MSG_ELECTION);
                ESP_LOGI(TAG, "Sent ELECTION message");
                start_election_timer(5000);
                last_send_time = xTaskGetTickCount();
                break;

            case election:
                if (received_alive_response) {
                    ESP_LOGI(TAG, "Received ALIVE response, waiting for VICTORY message");
                    // Remain in election state and wait for VICTORY message
                } else if (election_timer_expired) {
                    ESP_LOGI(TAG, "Election timer expired, declaring self as LEADER");
                    currentState = leader;
                    current_leader_id = myID;
                    leader_not_detected = false;
                    send_task(MSG_VICTORY);
                    ESP_LOGI(TAG, "Sent VICTORY message");
                    stop_election_timer();
                } else {
                    // Resend election message every 500 ms
                    if (xTaskGetTickCount() - last_send_time >= pdMS_TO_TICKS(1000)) {
                        send_task(MSG_ELECTION);
                        ESP_LOGI(TAG, "Resent ELECTION message");
                        last_send_time = xTaskGetTickCount();
                    }
                }
                break;

            case leader:
                // Send keep-alive messages every 2 seconds
                if (xTaskGetTickCount() - last_send_time >= pdMS_TO_TICKS(2000)) {
                    send_task(MSG_KEEP_ALIVE);
                    ESP_LOGI(TAG, "Sent KEEP_ALIVE message");
                    last_send_time = xTaskGetTickCount();
                }
                break;

            case not_leader:
                if (keep_alive_timer_expired) {
                    ESP_LOGI(TAG, "Keep-alive timer expired, initiating new ELECTION");
                    // Leader is assumed dead; initiate a new election
                    currentState = election;
                    leader_not_detected = true;
                    received_alive_response = false;
                    stop_keep_alive_timer();
                    send_task(MSG_ELECTION);
                    start_election_timer(5000);
                    last_send_time = xTaskGetTickCount();
                }
                break;

            default:
                ESP_LOGW(TAG, "Unknown state");
                break;
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

// Receive task -- looks for Start byte then stores received values -- 2023: minor changes
void recv_ir_task(){
  // Buffer for input data
  // Receiver expects message to be sent multiple times
  uint8_t *data_in = (uint8_t *) malloc(BUF_SIZE2);
  while (1) {
    int len_in = uart_read_bytes(UART_NUM_1, data_in, BUF_SIZE2, 100 / portTICK_PERIOD_MS);
    ESP_LOGE(TAG_UART, "Length: %d", len_in);
    if (len_in > 10) {
      int nn = 0;
      //ESP_LOGE(TAG_UART, "Length greater than 10");
      while (data_in[nn] != start) {
        nn++;
      }
      uint8_t copied[len_out];
      memcpy(copied, data_in + nn, len_out * sizeof(uint8_t));
      //printf("before checksum");
      //ESP_LOG_BUFFER_HEXDUMP(TAG_UART, copied, len_out, ESP_LOG_INFO);
      if (checkCheckSum(copied,len_out)) {
        int coordinator_socket;
        struct sockaddr_in server_addr;

        // Create a UDP socket
        if ((coordinator_socket = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("Socket creation failed");
            exit(EXIT_FAILURE);
        }

        // Setup server address structure
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(esp_port_list[current_leader_id]);

        // Convert IP address from text to binary form
        if (inet_pton(AF_INET, esp_ip_list[current_leader_id], &server_addr.sin_addr) <= 0) {
            perror("Invalid IP address");
            close(coordinator_socket);
            exit(EXIT_FAILURE);
        }

        // Send the message
        if (sendto(coordinator_socket, copied, len_out, 0, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
            perror("Message send failed");
        } else {
            printf("Message sent to %s:%d\n", esp_ip_list[current_leader_id], esp_port_list[current_leader_id]);
        }

        close(coordinator_socket);        

        ESP_LOG_BUFFER_HEXDUMP(TAG_UART, copied, len_out, ESP_LOG_INFO);
	uart_flush(UART_NUM_1);
      }
    }
    else{
      // printf("Nothing received.\n");
    }
    //vTaskDelay(5 / portTICK_PERIOD_MS);
  }
  free(data_in);
}

void send_ir_task(){
  while(1) {

    char *data_out = (char *) malloc(len_out);
    xSemaphoreTake(mux, portMAX_DELAY);
    data_out[0] = start;             // Start byte
    data_out[1] = MSG_SERVER;      // Message type (Election, Alive, Victory)
    data_out[2] = (char) myID;       // Device ID
    data_out[3] = (char) votedID;    // Voted ID
    data_out[4] = genCheckSum(data_out, len_out - 1); // Checksum

    uart_write_bytes(UART_NUM_1, data_out, len_out);
    xSemaphoreGive(mux);

    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

void app_main() {
    // Initialize NVS flash for Wi-Fi credentials
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    pwm_init();
    uart_init();

    // Log start and initialize Wi-Fi in station mode
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta(); // Initialize Wi-Fi

    // Initialize timers
    election_timer = NULL;
    keep_alive_timer = NULL;

    // Mutex for managing access to shared resources during send operations
    mux = xSemaphoreCreateMutex();

    // Timer queue initialization for managing timeouts in the election process
    timer_queue = xQueueCreate(10, sizeof(example_queue_element_t));
    if (!timer_queue) {
        ESP_LOGE(TAG_TIMER, "Creating queue failed");
        return;
    }

    // Initialize LED GPIOs to show different states
    led_init();

    // Create the task to handle incoming UDP messages
    xTaskCreate(recv_task, "recv_task", 1024 * 4, &esp_port_list[ID], 5, NULL);

    // Start the leader election state machine task
    xTaskCreate(bully_algorithm_task, "bully_algorithm_task", 1024 * 4, NULL, 5, NULL);

    xTaskCreate(send_ir_task, "send_ir_task", 1024 * 4, NULL, 5, NULL);
    xTaskCreate(recv_ir_task, "recv_ir_task", 1024 * 4, NULL, 5, NULL);

    // Create a task to periodically send the device ID via LED blinks
    xTaskCreate(id_task, "id_task", 1024 * 2, NULL, 5, NULL);

    // Main task loop (idle), waiting for the system to run
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
