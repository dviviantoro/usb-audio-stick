#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "AudioTools.h"

// ---- WiFi ----
// TODO: fill these in.
const char* WIFI_SSID = "hallohallo";
const char* WIFI_PASSWORD = "nasigodhok";

// ---- Network ----
constexpr uint16_t MIC_PORT = 5005;      // device -> PC (microphone audio)
constexpr uint16_t SPEAKER_PORT = 5006;  // PC -> device (speaker audio)

// ---- INMP441 microphone (I2S port 0, RX) ----
#define MIC_SCK 4
#define MIC_WS  5
#define MIC_SD  6

// ---- PCM5102 DAC (I2S port 1, TX) ----
// PCM5102 breakout notes: also wire VCC/GND, and tie its SCK (system clock)
// pin to GND if your board doesn't already do this -- most breakout boards
// ground it by default since we don't feed an external master clock.
#define DAC_BCK 10  // BCK
#define DAC_WS  8   // LCK
#define DAC_SD  9   // DIN

constexpr int SAMPLE_RATE = 16000;
constexpr int MIC_READ_SAMPLES = 256;  // 32-bit frames per I2S read

I2SStream i2sMic;
I2SStream i2sSpeaker;
WiFiUDP micUdp;
WiFiUDP speakerUdp;

IPAddress micListenerIp;
uint16_t micListenerPort = 0;
bool haveMicListener = false;

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP address: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWifi();

  // Microphone: I2S port 0, RX
  auto cfg_mic = i2sMic.defaultConfig(RX_MODE);
  cfg_mic.port_no = 0;
  cfg_mic.pin_bck = MIC_SCK;
  cfg_mic.pin_ws = MIC_WS;
  cfg_mic.pin_data = MIC_SD;
  cfg_mic.sample_rate = SAMPLE_RATE;
  cfg_mic.channels = 1;
  cfg_mic.bits_per_sample = 32;
  i2sMic.begin(cfg_mic);

  // Speaker: I2S port 1, TX
  auto cfg_speaker = i2sSpeaker.defaultConfig(TX_MODE);
  cfg_speaker.port_no = 1;
  cfg_speaker.pin_bck = DAC_BCK;
  cfg_speaker.pin_ws = DAC_WS;
  cfg_speaker.pin_data = DAC_SD;
  cfg_speaker.sample_rate = SAMPLE_RATE;
  cfg_speaker.channels = 2;
  cfg_speaker.bits_per_sample = 16;
  i2sSpeaker.begin(cfg_speaker);

  micUdp.begin(MIC_PORT);
  speakerUdp.begin(SPEAKER_PORT);

  Serial.println("Ready.");
  Serial.printf("Mic:     send any UDP packet to %s:%u to start receiving audio.\n",
                 WiFi.localIP().toString().c_str(), MIC_PORT);
  Serial.printf("Speaker: send 16-bit stereo PCM to %s:%u to play it back.\n",
                 WiFi.localIP().toString().c_str(), SPEAKER_PORT);
}

// A listener "registers" by sending any UDP packet to MIC_PORT; we remember
// its address and stream mic audio back to it until a different sender
// registers.
void serviceMicRegistration() {
  int packetSize = micUdp.parsePacket();
  if (packetSize <= 0) return;

  micListenerIp = micUdp.remoteIP();
  micListenerPort = micUdp.remotePort();
  haveMicListener = true;

  uint8_t discard[32];
  micUdp.read(discard, min(packetSize, (int)sizeof(discard)));
  Serial.printf("Mic listener registered: %s:%u\n", micListenerIp.toString().c_str(), micListenerPort);
}

void streamMicToUdp() {
  static int32_t raw[MIC_READ_SAMPLES];
  static int16_t pcm[MIC_READ_SAMPLES];

  size_t bytesRead = i2sMic.readBytes((uint8_t*)raw, sizeof(raw));
  size_t samplesRead = bytesRead / sizeof(int32_t);
  if (samplesRead == 0 || !haveMicListener) return;

  for (size_t i = 0; i < samplesRead; i++) {
    int32_t v = (raw[i] >> 8) & 0xFFFFFF;
    if (v & 0x800000) v |= 0xFF000000;  // sign-extend 24-bit sample
    pcm[i] = (int16_t)(v >> 8);         // downscale to 16-bit
  }

  micUdp.beginPacket(micListenerIp, micListenerPort);
  micUdp.write((const uint8_t*)pcm, samplesRead * sizeof(int16_t));
  micUdp.endPacket();
}

void serviceSpeakerUdp() {
  int packetSize = speakerUdp.parsePacket();
  if (packetSize <= 0) return;

  static uint8_t buf[1472];  // fits in one UDP payload without IP fragmentation
  int len = speakerUdp.read(buf, min(packetSize, (int)sizeof(buf)));
  if (len > 0) {
    i2sSpeaker.write(buf, len);
  }
}

void loop() {
  serviceMicRegistration();
  streamMicToUdp();
  serviceSpeakerUdp();
}
