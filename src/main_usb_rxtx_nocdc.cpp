// DIAGNOSTIC BUILD -- mic + speaker (RXTX), NO composite CDC.
// Adding a composite CDC debug port alongside RXTX audio made afplay fail
// outright ("AudioQueueStart failed") and matches the earlier
// crashed/errored symptom seen in QuickTime -- CDC + bidirectional audio
// together appears to break actual host-side stream negotiation, not just
// enumeration. This build drops CDC (matching the structure that's already
// proven to actually stream) and keeps the stereo I2S TX fix: the PCM5102
// is a real stereo DAC, so each mono USB sample is duplicated to L+R
// instead of relying on the I2S driver's "mono" mode (which left the
// unfed channel undefined and came out as noise).
#include <Arduino.h>
#include <USB.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

#define MIC_SCK 4
#define MIC_WS  5
#define MIC_SD  6

#define DAC_BCK 10  // BCK
#define DAC_WS  8   // LCK
#define DAC_SD  9   // DIN

constexpr int SAMPLE_RATE = 16000;
constexpr int MIC_READ_SAMPLES = 256;

// TEMPORARY: WiFi/UDP debug logging, since a composite CDC interface
// alongside RXTX audio breaks host-side stream startup (AudioQueueStart
// failure). WiFi/UDP doesn't touch the USB descriptor at all, so it can't
// interfere with the Audio class. Remove once the speaker issue is solved.
const char* WIFI_SSID = "hallohallo";
const char* WIFI_PASSWORD = "nasigodhok";
constexpr uint16_t LOG_PORT = 5099;
WiFiUDP logUdp;

void dlog(const char* msg) {
  logUdp.beginPacket(IPAddress(255, 255, 255, 255), LOG_PORT);
  logUdp.print(msg);
  logUdp.endPacket();
}

I2SStream i2sMic;
I2SStream i2sSpeaker;
USBAudioStream usbAudio;

void setup() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
  }
  dlog("booting rxtx-nocdc build, wifi debug active");

  if (!TinyUSBDevice.isInitialized()) {
    TinyUSBDevice.begin(0);
  }

  auto cfg_mic = i2sMic.defaultConfig(RX_MODE);
  cfg_mic.port_no = 0;
  cfg_mic.pin_bck = MIC_SCK;
  cfg_mic.pin_ws = MIC_WS;
  cfg_mic.pin_data = MIC_SD;
  cfg_mic.sample_rate = SAMPLE_RATE;
  cfg_mic.channels = 1;
  cfg_mic.bits_per_sample = 32;
  i2sMic.begin(cfg_mic);

  auto cfg_speaker = i2sSpeaker.defaultConfig(TX_MODE);
  cfg_speaker.port_no = 1;
  cfg_speaker.pin_bck = DAC_BCK;
  cfg_speaker.pin_ws = DAC_WS;
  cfg_speaker.pin_data = DAC_SD;
  cfg_speaker.sample_rate = SAMPLE_RATE;
  cfg_speaker.channels = 2;
  cfg_speaker.bits_per_sample = 16;
  i2sSpeaker.begin(cfg_speaker);
  dlog("i2s speaker started (stereo)");

  auto cfg_usb = usbAudio.defaultConfig(RXTX_MODE);
  cfg_usb.sample_rate = SAMPLE_RATE;
  cfg_usb.channels = 1;
  cfg_usb.bits_per_sample = 16;
  cfg_usb.begin_usb = true;
  usbAudio.begin(cfg_usb);

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }
  dlog("ready");
}

void streamMicToUsb() {
  static int32_t raw[MIC_READ_SAMPLES];
  static int16_t pcm[MIC_READ_SAMPLES];

  size_t bytesRead = i2sMic.readBytes((uint8_t*)raw, sizeof(raw));
  size_t samplesRead = bytesRead / sizeof(int32_t);
  if (samplesRead == 0) return;

  for (size_t i = 0; i < samplesRead; i++) {
    int32_t v = (raw[i] >> 8) & 0xFFFFFF;
    if (v & 0x800000) v |= 0xFF000000;
    pcm[i] = (int16_t)(v >> 8);
  }
  usbAudio.write((const uint8_t*)pcm, samplesRead * sizeof(int16_t));
}

void streamUsbToSpeaker() {
  static int16_t mono[MIC_READ_SAMPLES];
  static int16_t stereo[MIC_READ_SAMPLES * 2];
  static uint32_t usbBytesTotal = 0;
  static uint32_t i2sBytesTotal = 0;
  static uint32_t lastPrint = 0;

  size_t bytesRead = usbAudio.readBytes((uint8_t*)mono, sizeof(mono));
  size_t samplesRead = bytesRead / sizeof(int16_t);
  usbBytesTotal += bytesRead;

  if (samplesRead > 0) {
    for (size_t i = 0; i < samplesRead; i++) {
      stereo[i * 2] = mono[i];
      stereo[i * 2 + 1] = mono[i];
    }
    size_t written = i2sSpeaker.write((const uint8_t*)stereo, samplesRead * 2 * sizeof(int16_t));
    i2sBytesTotal += written;
  }

  uint32_t now = millis();
  if (now - lastPrint > 1000) {
    lastPrint = now;
    char buf[64];
    snprintf(buf, sizeof(buf), "usb-in: %lu B/s  i2s-out: %lu B/s",
             (unsigned long)usbBytesTotal, (unsigned long)i2sBytesTotal);
    dlog(buf);
    usbBytesTotal = 0;
    i2sBytesTotal = 0;
  }
}

void loop() {
  streamMicToUsb();
  streamUsbToSpeaker();
}
