#include "AudioTools.h"
#include "WiFi.h"
#include "WiFiUdp.h"
#include <FastLED.h>

#define NUM_LEDS       18 //1
#define LED_DATA_PIN   23
#define LED_CLOCK_PIN  18

CRGB leds[NUM_LEDS];

//
// HOLY FRICK JUST USE THIS NETCAT COMMAND (with the -p) IT WORKS
// nc -l -p 6000 | play -t raw -r 16000 -e signed -b 32 -c 1 -
//

const char* SSID              = "monkeyphone";
const char* PASS              = "password";
// const char* REMOTE_IP      = "10.0.0.47";  // mac mini (bingowireless2g) -3/22/26
const char* REMOTE_IP      = "172.20.10.3"; // tiny talkbox (bingowireless2g_EXT) -4/20/26 | (bingowireless2g) -3/28/26
// const char* REMOTE_IP         = "10.0.0.154"; // big talkbox (bingowireless2g_EXT) -4/21/26
const int   REMOTE_AUDIO_PORT = 6000;
const int   REMOTE_POT_PORT   = 6001;
const int   AUDIO_PORT        = 6000;
const int   POT_PORT          = 6001;

const int   POT_PIN       = 34;
const int   POT_PERIOD_MS = 500;

WiFiUDP   udp_out;
WiFiUDP   udp_in;
WiFiUDP   pot_udp_out;
WiFiUDP   pot_udp_in;

I2SStream in;
I2SStream out;
VolumeStream volume(out); // you only interface with volume -> it scales then forwards to out FOR you

uint8_t tx_buffer[512];
uint8_t rx_buffer[512];

volatile int tx_bytes = 0;
volatile int rx_bytes = 0;

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

    WiFi.setSleep(WIFI_PS_NONE);

    Serial.println("WiFi connected at IP:");
    Serial.println(WiFi.localIP());
}

void setupLeds() {
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 400);
    FastLED.addLeds<APA102, LED_DATA_PIN, LED_CLOCK_PIN, BGR, DATA_RATE_MHZ(1)>(leds, NUM_LEDS);
    FastLED.setBrightness(80);
    FastLED.setDither(1);
}

void setupMic() {
    auto cfg_in = in.defaultConfig(RX_MODE);
    cfg_in.sample_rate     = 16000;
    cfg_in.bits_per_sample = 16;
    cfg_in.channels        = 1;
    cfg_in.pin_bck         = 32;
    cfg_in.pin_ws          = 15;
    cfg_in.pin_data        = 33;
    cfg_in.port_no         = 0;
    in.begin(cfg_in);
    Serial.println("Mic is running");
}

void setupSpeaker() {
    auto cfg_out = out.defaultConfig(TX_MODE);
    cfg_out.sample_rate     = 16000;
    cfg_out.bits_per_sample = 16;
    cfg_out.channels        = 1;
    cfg_out.pin_bck         = 26;
    cfg_out.pin_ws          = 25; // lrck
    cfg_out.pin_data        = 22; // sd
    cfg_out.port_no         = 1;

    auto vcfg_out = volume.defaultConfig();
    vcfg_out.copyFrom(cfg_out);
    vcfg_out.allow_boost = true;

    out.begin(cfg_out);
    volume.begin(vcfg_out);
    volume.setVolume(1.0);

    Serial.println("Speaker is running");

    udp_in.begin(AUDIO_PORT);
    pot_udp_in.begin(POT_PORT);

    Serial.println("UDP listening on port " + String(AUDIO_PORT));
}

void streamMic(void* pvParameters) {
    for (;;) {
        int byte_size = in.readBytes(tx_buffer, 512);
        if (byte_size) {
            udp_out.beginPacket(REMOTE_IP, REMOTE_AUDIO_PORT);
            udp_out.write(tx_buffer, byte_size);
            tx_bytes += byte_size;
            udp_out.endPacket();
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void readSpeaker(void* pvParameters) {
    for (;;) {
        int packet_size = udp_in.parsePacket();

        if (packet_size) {
            udp_in.read(rx_buffer, 512);
            rx_bytes += packet_size;
            volume.write(rx_buffer, packet_size);
            // Serial.print("incoming audio packet size:");
            // Serial.println(packet_size);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

void manageWiFi(void* pvParameters) {
    for (;;) {
        Serial.print("wifi strength: ");
        Serial.println(WiFi.RSSI());
        Serial.printf("tx: %d bytes/sec, rx: %d bytes/sec\n", tx_bytes, rx_bytes);

        tx_bytes = 0;
        rx_bytes = 0;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void streamPot(void* pvParameters) {
    for (;;) {
        // Serial.println(analogRead(POT_PIN));
        float potNorm = (1-(analogRead(POT_PIN) / 4095.0f));
        byte potNorm_bytes[4];
        memcpy(potNorm_bytes, &potNorm, 4);

        pot_udp_out.beginPacket(REMOTE_IP, REMOTE_POT_PORT);
        pot_udp_out.write(potNorm_bytes, 4);
        pot_udp_out.endPacket();

        // Serial.print("streaming pot value: ");
        // Serial.println(potNorm);

        volume.setVolume(potNorm * 5);

        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

void readPot(void* pvParameters) {
    for (;;) {
        int pot_packet_size = pot_udp_in.parsePacket();

        if (pot_packet_size) {
            byte remote_potNorm[4];
            pot_udp_in.read(remote_potNorm, 4);

            float result;
            memcpy(&result, remote_potNorm, 4);

            // Serial.print("reading pot value: ");
            // Serial.println(result);

            for (int i = 0; i < NUM_LEDS; i++) {
                leds[i] = CHSV(24, 200, result * 255);
            }

            FastLED.show();

        } else {
            vTaskDelay(pdMS_TO_TICKS(33));
        }
    }
}

// 13-bit effective range: top 8 bits → RGB value, bottom 5 bits → global brightness
void setHighResBrightness(float norm) {  // norm: 0.0 – 1.0
    uint16_t val = (uint16_t)(norm * 8191);
    uint8_t  rgb = val >> 5;
    uint8_t  gb  = (val & 0x1F);

    float r = rgb * (95.0f / 255.0f);
    float g = rgb * (72.0f / 255.0f);
    float b = rgb * (42.0f / 255.0f);

    FastLED.setBrightness(gb == 0 ? 1 : gb);
    for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = CRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
    FastLED.show();
}


void testLocalPot(void* pvParameters) {
    for (;;) {
        float potNorm = 1.0f - (analogRead(POT_PIN) / 4095.0f);
        
        for (int i = 0; i < NUM_LEDS; i++) {
            leds[i] = CHSV(24, 200, potNorm * 255);
        }

        FastLED.show();
    }
}
                          
// void manageLeds(void* pvParameters) {
//     for (;;) {
//         FastLED.setBrightness(remotePot);

//         for (int i = 0; i < NUM_LEDS; i++) {
//             leds[i] = CRGB::White;
//         }

//         vTaskDelay(pdMS_TO_TICKS(1));
//     }
// }

void setup() {
    Serial.begin(115200);

    connectToWiFi();
    setupLeds();
    setupMic();
    setupSpeaker();

    // Core 1: Important
    xTaskCreatePinnedToCore(streamMic,   "Stream Mic to Remote",          16000, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(readSpeaker, "Playback Remote to Speaker", 16000, NULL, 2, NULL, 1);

    // Core 0: Secondary (shared with WiFi manage.)
    xTaskCreatePinnedToCore(streamPot, "Stream Pot to Remote and Manage Volume", 8000, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(readPot, "Read Remote Pot and Light LEDs", 16000, NULL, 0, NULL, 0);

    // xTaskCreatePinnedToCore(testLocalPot, "[TEST] Read Remote Pot and Light LEDs", 16000, NULL, 0, NULL, 1);
    // xTaskCreatePinnedToCore(manageWiFi,  "WiFi Diagnostics",               8000, NULL, 0, NULL, 0);
}

void loop() {
    vTaskDelete(NULL);
}
