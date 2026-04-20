/*
Project: DIY dog communication buttons with data logging
Version: 1.0.0
Author: Harold Peters Inskipp
Github: https://github.com/HaroldPetersInskipp/DIY-Dog-Buttons

Hardware:
ESP32-S3 N16R8 WROOM - https://www.amazon.com/dp/B0D93DLB6Q
Micro SD Card Module - https://www.amazon.com/dp/B0FSWXYBYH
8GB Micro SD Card - https://www.amazon.com/dp/B0D5HDNMP8
MAX98357A I2S Amplifier - https://www.amazon.com/dp/B0B4GK5R1R
4 Ohm 5 Watt Speaker - https://www.amazon.com/dp/B081169PC5
IP5306 Lithium Battery Boost and Charge Module - https://www.amazon.com/dp/B0836J8LR4
18650 Battery Cell - Salvage from laptop and tool batteries or https://www.amazon.com/dp/B0G4VY3HT6
Battery Holder - https://www.amazon.com/dp/B0BJV7SK5D
4-Inch Buttons with LED - https://www.amazon.com/dp/B071FSKY6Q

Optional Hardware:
Micro USB Power Adapter (for charging 18650 cells easier) - https://www.amazon.com/dp/B0G4VY3HT6
Breadboard + Male to Male Dupont wires (for prototyping) - https://www.amazon.com/dp/B08Y59P6D1
0.1uF ceramic and 100uF electrolytic capacitors (bypass capacitors for amp sound quality) - Just find some cheap ones it's not rocket science
Raspberry Pi Zero 2 W (to host a Node-RED server and MQTT broker for logging data) - https://www.amazon.com/dp/B0DRRRZBMP this kit does not come with power adaptor

Software:
ESP32-audioI2S Library - https://github.com/schreibfaul1/ESP32-audioI2S

Audio:
WAV, PCM, 16-bit, 16kHz, mono audio files recorded with Audacity
*/

// Uncomment serial lines when debugging

#include <time.h>
#include "Arduino.h"
#include "Audio.h"
#include "SPI.h"
#include "SD.h"
#include "FS.h"
#include <WiFi.h>
#include <PubSubClient.h>

// WIFI  CONFIG
const char* ssid = "SSID";
const char* wifi_password = "PASSWORD";

// MQTT CONFIG
const char* mqtt_server = "IP_ADDRESS";
const int   mqtt_port = 1883;
const char* mqtt_user = "USERNAME";
const char* mqtt_pass = "PASSWORD";

// SD SPI
#define SD_CS    10
#define SPI_MOSI 16 
#define SPI_MISO 18
#define SPI_SCK  17

// I2S
#define I2S_DOUT 21
#define I2S_BCLK 20
#define I2S_LRC  47

// Buttons
#define BUTTON1_PIN 4
#define BUTTON2_PIN 8

#define DEBOUNCE_MS 50

unsigned long lastPressTime1 = 0;
unsigned long lastPressTime2 = 0;

// LEDs
#define LED1_PIN 5
#define LED2_PIN 9

Audio audio;

// NTP
bool ntpInitialized = false;
bool timeValid = false;
unsigned long lastNtpCheck = 0;

unsigned long getTimestamp() {
    time_t now = time(nullptr);

    if (now > 1000000000) {
        timeValid = true;
        return (unsigned long)now;
    }

    return millis();
}

// MQTT + RTOS
WiFiClient espClient;
PubSubClient mqttClient(espClient);

QueueHandle_t eventQueueRTOS;

struct Event {
    char name[16];
    unsigned long timestamp;
};

// BUTTON STATE
bool lastState1 = HIGH;
bool lastState2 = HIGH;

bool wasPlaying = false;
int activeLED = -1;

// EVENT QUEUE FUNCTION
void addEvent(const char* name) {
    Event e;
    strncpy(e.name, name, sizeof(e.name));
    e.name[sizeof(e.name) - 1] = '\0';
    e.timestamp = getTimestamp();

    if (xQueueSend(eventQueueRTOS, &e, 0) != pdTRUE) {
        //Serial.println("[QUEUE] Full, dropping event");
    } else {
        //Serial.print("[QUEUE] Added: ");
        //Serial.println(e.name);
    }
}

// MQTT TASK (Core 0)
void mqttTask(void *parameter) {
    unsigned long lastMQTTRetry = 0;

    Event batch[20];
    int batchCount = 0;

    for (;;) {
        // -------- WiFi --------
        static bool wifiConnecting = false;
        static unsigned long wifiStartTime = 0;
        static unsigned long lastWiFiRetry = 0;

        wl_status_t status = WiFi.status();

        if (status == WL_CONNECTED) {
            if (wifiConnecting) {
                //Serial.print("[MQTT] WiFi connected! IP: ");
                //Serial.println(WiFi.localIP());
                wifiConnecting = false;

                // -------- NTP INIT --------
                if (WiFi.status() == WL_CONNECTED && !ntpInitialized) {
                    //Serial.println("[NTP] Initializing time sync...");

                    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

                    // local timezone
                    setenv("TZ", "MST7MDT,M3.2.0,M11.1.0", 1);
                    tzset();

                    ntpInitialized = true;
                }
            }
        }
        else {
            // If not currently trying, start a connection
            if (!wifiConnecting && millis() - lastWiFiRetry > 10000) {
                //Serial.println("[MQTT] Starting WiFi...");
                WiFi.begin(ssid, wifi_password);
                wifiConnecting = true;
                wifiStartTime = millis();
                lastWiFiRetry = millis();
            }

            // If stuck connecting too long → reset and retry
            if (wifiConnecting && millis() - wifiStartTime > 15000) {
                //Serial.println("[MQTT] WiFi connect timeout, retrying...");
                WiFi.disconnect(true);
                wifiConnecting = false;
            }
        }

        // -------- MQTT --------
        if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
            if (millis() - lastMQTTRetry > 5000) {
                //Serial.print("[MQTT] Connecting...");
                if (mqttClient.connect("ESP32Client", mqtt_user, mqtt_pass)) {
                    //Serial.println("connected");
                } else {
                    //Serial.print("failed rc=");
                    //Serial.println(mqttClient.state());
                }
                lastMQTTRetry = millis();
            }
        }

        // Keep MQTT alive
        if (mqttClient.connected()) {
            mqttClient.loop();
        }

        // -------- NTP CHECK --------
        if (ntpInitialized && millis() - lastNtpCheck > 10000) {
            lastNtpCheck = millis();

            time_t now = time(nullptr);

            if (now > 1000000000) {
                if (!timeValid) {
                    //Serial.print("[NTP] Time synced: ");
                    //Serial.println(now);
                }
                timeValid = true;
            } else {
                //Serial.println("[NTP] Waiting for valid time...");
            }
        }

        // -------- Read queue --------
        Event e;
        while (xQueueReceive(eventQueueRTOS, &e, 0) == pdTRUE) {
            if (batchCount < 20) {
                batch[batchCount++] = e;
            }
        }

        // -------- Send batch every 5 sec --------
        static unsigned long lastSend = 0;
        if (batchCount > 0 && millis() - lastSend > 5000) {
            if (mqttClient.connected()) {
                char payload[1024];
                int len = 0;

                len += snprintf(payload + len, sizeof(payload) - len, "[");

                for (int i = 0; i < batchCount; i++) {
                    len += snprintf(payload + len, sizeof(payload) - len,
                        "{\"event\":\"%s\",\"time\":%lu}%s",
                        batch[i].name,
                        batch[i].timestamp,
                        (i < batchCount - 1) ? "," : ""
                    );
                }

                len += snprintf(payload + len, sizeof(payload) - len, "]");

                //Serial.println("[MQTT] Sending:");
                //Serial.println(payload);

                if (mqttClient.publish("dog/buttons", payload)) {
                    //Serial.println("[MQTT] Publish OK");
                    batchCount = 0;
                } else {
                    //Serial.println("[MQTT] Publish FAILED");
                }
            }
            lastSend = millis();
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// SETUP
void setup() {
    Serial.begin(115200);
    delay(1000);
    //Serial.println("\n=== ESP32 START ===");

    pinMode(BUTTON1_PIN, INPUT_PULLUP);
    pinMode(BUTTON2_PIN, INPUT_PULLUP);

    pinMode(LED1_PIN, OUTPUT);
    pinMode(LED2_PIN, OUTPUT);

    digitalWrite(LED1_PIN, LOW);
    digitalWrite(LED2_PIN, LOW);

    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    if (!SD.begin(SD_CS)) {
        //Serial.println("SD FAIL");
        while (1);
    }

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(17);

    // WiFi + MQTT setup
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setKeepAlive(60);

    // Create queue
    eventQueueRTOS = xQueueCreate(40, sizeof(Event));

    // Start MQTT task on Core 0
    xTaskCreatePinnedToCore(
        mqttTask,
        "MQTT Task",
        8192,
        NULL,
        1,
        NULL,
        0
    );
}

// LOOP (Core 1)
void loop() {
    audio.loop();

    bool current1 = digitalRead(BUTTON1_PIN);
    bool current2 = digitalRead(BUTTON2_PIN);

    // BUTTON 1
    if (lastState1 == HIGH && current1 == LOW) {
        if (millis() - lastPressTime1 > DEBOUNCE_MS) {
            lastPressTime1 = millis();
            //Serial.println("Button 1");

            audio.stopSong();
            audio.connecttoFS(SD, "/treat.wav");

            digitalWrite(LED1_PIN, HIGH);
            digitalWrite(LED2_PIN, LOW);
            activeLED = 1;

            addEvent("treat");
        }
    }

    // BUTTON 2
    if (lastState2 == HIGH && current2 == LOW) {
        if (millis() - lastPressTime2 > DEBOUNCE_MS) {
            lastPressTime2 = millis();
    
            //Serial.println("Button 2");

            audio.stopSong();
            audio.connecttoFS(SD, "/outside.wav");

            digitalWrite(LED2_PIN, HIGH);
            digitalWrite(LED1_PIN, LOW);
            activeLED = 2;

            addEvent("outside");
        }
    }

    // LED OFF when playback stops
    bool isPlaying = audio.isRunning();
    if (wasPlaying && !isPlaying) {
        if (activeLED == 1) digitalWrite(LED1_PIN, LOW);
        if (activeLED == 2) digitalWrite(LED2_PIN, LOW);
        activeLED = -1;
    }

    wasPlaying = isPlaying;

    lastState1 = current1;
    lastState2 = current2;

    delay(1);
}