// DIAGNOSTIC BUILD -- mic capture only, TX_MODE, no speaker, no composite
// CDC. Mirrors the audio-tools library's own validated usb-tx.ino example
// as closely as possible, to isolate whether the RXTX (mic+speaker) +
// composite CDC descriptor in main_usb.cpp is what's breaking real-time
// streaming on the host, independent of the I2S mic conversion code (which
// is already proven correct via the UDP build).
#include <Arduino.h>
#include <USB.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

#define MIC_SCK 4
#define MIC_WS  5
#define MIC_SD  6

constexpr int SAMPLE_RATE = 16000;
constexpr int MIC_READ_SAMPLES = 256;

I2SStream i2sMic;
USBAudioStream usbAudio;

void setup() {
  // Required on cores without built-in TinyUSB support -- harmless no-op
  // on ESP32, included for parity with the library's documented pattern.
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

  auto cfg_usb = usbAudio.defaultConfig(TX_MODE);
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
}

void loop() {
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
