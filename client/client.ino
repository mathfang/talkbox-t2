#include "AudioTools.h"
#include "WiFi.h"
#include "WiFiUdp.h"

//
// HOLY FRICK JUST USE THIS NETCAT COMMAND (with the -p) IT WORKS
// nc -l -p 6000 | play -t raw -r 16000 -e signed -b 32 -c 1 -
//

const char* SSID = "bingowireless2g_EXT";
const char* PASS = "draco10935";
// const char* SERVER_IP = "10.0.0.47"; // mac mini (bingowireless2g) -3/22/26
const char* REMOTE_IP = "10.0.0.154";  // tiny talkbox (bingowireless2g_EXT) -4/20/26 | (bingowireless2g) -3/28/26
const int REMOTE_AUDIO_PORT = 6000;
const int REMOTE_POT_PORT = 5001;

const int POT_PIN = 34;
const int POT_PERIOD_MS = 500;                           

WiFiUDP udp;
I2SStream in;

uint8_t buffer[512];

void connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(500);
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Connecting to WiFi...");
    Serial.println(WiFi.RSSI());
    delay(2000);
  }
  Serial.println("WiFi connected at IP:");
  Serial.println(WiFi.localIP());
}

void setupMic() {
  auto cfg_in = in.defaultConfig(RX_MODE);
  cfg_in.sample_rate = 16000;
  cfg_in.bits_per_sample = 32;
  cfg_in.channels = 1;
  cfg_in.pin_bck = 32;
  cfg_in.pin_ws = 15;
  cfg_in.pin_data = 33;
  cfg_in.port_no = 0;

  in.begin(cfg_in);

  Serial.println("Mic is running");
}

void streamMic(void* pvParameters) {
  for (;;) {
    in.readBytes(buffer, 512);

    udp.beginPacket(REMOTE_IP, REMOTE_AUDIO_PORT);
    udp.write(buffer, 512);
    udp.endPacket();
  }
}

void manageWiFi(void* pvParameters) {
  for (;;) {
    Serial.print("wifi strength: ");
    Serial.print(WiFi.RSSI());
    // Serial.print(", connected:");
    // Serial.println(audio_client.connected());
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}


void setup() {
  Serial.begin(115200);

  connectToWiFi();
  setupMic();

  xTaskCreatePinnedToCore(
    streamMic,
    "Stream Mic to Server",
    16000,
    NULL,
    1,
    NULL,
    1);

  xTaskCreatePinnedToCore(
    manageWiFi,
    "Print WiFi Diagnostics",
    8000,
    NULL,
    0,
    NULL,
    1);

  // xTaskCreatePinnedToCore(
  //     streamPot,
  //     "Stream Pot to Server",
  //     4096,
  //     NULL,
  //     0,
  //     NULL,
  //     0
  // );
}

void loop() {
  vTaskDelete(NULL);
}
