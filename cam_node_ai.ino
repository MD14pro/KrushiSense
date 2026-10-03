#include "esp_camera.h"
#include <WiFi.h>
#include <esp_now.h>
#include <KrushiSense_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

// AI-Thinker Camera Pins
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22
#define FLASH_LED_PIN      4

uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct struct_message {
  char node[12];
  char label[16];
  float confidence;
} struct_message;

typedef struct struct_command {
  bool surveillance_active;
} struct_command;

struct_message outgoingMsg;
volatile bool systemArmed = true; // Default active until Hub instructs otherwise

// Tight False Positive Filters
#define CONFIDENCE_THRESHOLD  0.80  // Raised to 80% to filter noise
#define REQUIRED_CONSECUTIVE_DETECTIONS 2

int consecutive_hits = 0;
char last_candidate[16] = "";
static camera_fb_t *fb = NULL;

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {}
void OnCommandRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {}
void OnCommandRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(struct_command)) {
    struct_command cmd;
    memcpy(&cmd, data, sizeof(cmd));
    systemArmed = cmd.surveillance_active;
  }
}

int raw_feature_get_data(size_t offset, size_t length, float *out_ptr) {
  size_t pixel_ix = offset * 2;
  for (size_t i = 0; i < length; i++) {
    uint8_t byte1 = fb->buf[pixel_ix];
    uint8_t byte2 = fb->buf[pixel_ix + 1];
    uint16_t rgb565 = (byte1 << 8) | byte2;

    uint8_t r = (rgb565 >> 11) & 0x1F;
    uint8_t g = (rgb565 >> 5) & 0x3F;
    uint8_t b = rgb565 & 0x1F;

    r = (r * 255) / 31;
    g = (g * 255) / 63;
    b = (b * 255) / 31;

    out_ptr[i] = (float)((r << 16) | (g << 8) | b);
    pixel_ix += 2;
  }
  return 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);

  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, HIGH);
  delay(50);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(50);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;

  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_96X96;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_camera_init(&config);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() == ESP_OK) {
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnCommandRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 1; // Locked to Hub AP channel
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  strcpy(outgoingMsg.node, "CAM_NODE_01");
}

void loop() {
  // Standby Mode: Agar current time slot me surveillance allow nahi hai
  if (!systemArmed) {
    digitalWrite(FLASH_LED_PIN, LOW);
    consecutive_hits = 0;
    delay(500); // Low-power sleep
    return;
  }

  fb = esp_camera_fb_get();
  if (!fb) { delay(80); return; }

  signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = &raw_feature_get_data;

  ei_impulse_result_t result = { 0 };
  EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
  esp_camera_fb_return(fb);

  if (r != EI_IMPULSE_OK) { delay(60); return; }

  float max_confidence = 0.0;
  const char* top_label = "background";

  for (size_t ix = 0; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
    if (result.classification[ix].value > max_confidence) {
      max_confidence = result.classification[ix].value;
      top_label = result.classification[ix].label;
    }
  }

  // Double Confirmation Gate (Streak verification)
  if (strcmp(top_label, "background") != 0 && max_confidence >= CONFIDENCE_THRESHOLD) {
    if (strcmp(last_candidate, top_label) == 0) {
      consecutive_hits++;
    } else {
      strcpy(last_candidate, top_label);
      consecutive_hits = 1;
    }

    if (consecutive_hits >= REQUIRED_CONSECUTIVE_DETECTIONS) {
      strcpy(outgoingMsg.label, top_label);
      outgoingMsg.confidence = max_confidence * 100.0;
      esp_now_send(broadcastAddress, (uint8_t *)&outgoingMsg, sizeof(outgoingMsg));

      digitalWrite(FLASH_LED_PIN, HIGH);
      delay(80);
      digitalWrite(FLASH_LED_PIN, LOW);

      consecutive_hits = 0;
      delay(1200); // Alert cooldown
    }
  } else {
    consecutive_hits = 0;
    last_candidate[0] = 0;
  }

  delay(40);
}