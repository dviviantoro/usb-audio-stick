#include <Arduino.h>
#include <USB.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

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

// USBAudioStream on ESP32 requires ARDUINO_USB_CDC_ON_BOOT=0 (OTG mode
// only), so the default "Serial" object doesn't route over USB. This gives
// us a second, composite CDC interface for debug logging over the same
// cable, alongside the Audio class.
USBCDC MySerial;

I2SStream i2sMic;
I2SStream i2sSpeaker;
USBAudioStream usbAudio;

// PCM5102 is a real stereo DAC -- the I2S driver's "mono" TX mode left the
// right channel's slot undefined (silent), so only the left speaker played.
// Duplicate each mono USB sample to L+R explicitly instead.
void streamUsbToSpeaker() {
  static int16_t mono[MIC_READ_SAMPLES];
  static int16_t stereo[MIC_READ_SAMPLES * 2];

  size_t bytesRead = usbAudio.readBytes((uint8_t*)mono, sizeof(mono));
  size_t samplesRead = bytesRead / sizeof(int16_t);
  if (samplesRead == 0) return;

  for (size_t i = 0; i < samplesRead; i++) {
    stereo[i * 2] = mono[i];
    stereo[i * 2 + 1] = mono[i];
  }
  i2sSpeaker.write((const uint8_t*)stereo, samplesRead * 2 * sizeof(int16_t));
}

void streamMicToUsb() {
  static int32_t raw[MIC_READ_SAMPLES];
  static int16_t pcm[MIC_READ_SAMPLES];

  size_t bytesRead = i2sMic.readBytes((uint8_t*)raw, sizeof(raw));
  size_t samplesRead = bytesRead / sizeof(int32_t);
  if (samplesRead == 0) return;

  for (size_t i = 0; i < samplesRead; i++) {
    int32_t v = (raw[i] >> 8) & 0xFFFFFF;
    if (v & 0x800000) v |= 0xFF000000;  // sign-extend 24-bit sample
    pcm[i] = (int16_t)(v >> 8);         // downscale to 16-bit
  }

  usbAudio.write((const uint8_t*)pcm, samplesRead * sizeof(int16_t));
}

void setup() {
  MySerial.begin(115200);
  delay(2000);
  AudioLogger::instance().begin(MySerial, AudioLogger::Warning);
  MySerial.println("booting usb audio build...");

  // Microphone: I2S port 0, RX, native 32-bit frames from the INMP441.
  // streamMicToUsb() converts these down to 16-bit before writing to USB.
  auto cfg_mic = i2sMic.defaultConfig(RX_MODE);
  cfg_mic.port_no = 0;
  cfg_mic.pin_bck = MIC_SCK;
  cfg_mic.pin_ws = MIC_WS;
  cfg_mic.pin_data = MIC_SD;
  cfg_mic.sample_rate = SAMPLE_RATE;
  cfg_mic.channels = 1;
  cfg_mic.bits_per_sample = 32;
  i2sMic.begin(cfg_mic);
  MySerial.println("i2s mic started");

  // Speaker: I2S port 1, TX. Genuine stereo -- see streamUsbToSpeaker().
  auto cfg_speaker = i2sSpeaker.defaultConfig(TX_MODE);
  cfg_speaker.port_no = 1;
  cfg_speaker.pin_bck = DAC_BCK;
  cfg_speaker.pin_ws = DAC_WS;
  cfg_speaker.pin_data = DAC_SD;
  cfg_speaker.sample_rate = SAMPLE_RATE;
  cfg_speaker.channels = 2;
  cfg_speaker.bits_per_sample = 16;
  i2sSpeaker.begin(cfg_speaker);
  MySerial.println("i2s speaker started");

  // USB Audio: capture (mic) + playback (speaker) in one composite device.
  auto cfg_usb = usbAudio.defaultConfig(RXTX_MODE);
  cfg_usb.sample_rate = SAMPLE_RATE;
  cfg_usb.channels = 1;
  cfg_usb.bits_per_sample = 16;
  // arduino-esp32's CDC interface hard-reserves EP3 OUT (esp32-hal-tinyusb.c),
  // but audio-tools' ESP32 default for the speaker's OUT endpoint is also EP3
  // OUT -- two interfaces claiming the same address, which kept the Jetson
  // host from enumerating the speaker (mic was fine on EP3 IN, which CDC
  // never touches). EP4 OUT is unclaimed by CDC, so move the speaker there.
  cfg_usb.ep_out = 0x04;
  // Without this, the host's volume slider is reported to us via
  // volume()/setVolumeCallback() but never actually applied to the audio --
  // the slider moves but playback level doesn't change.
  cfg_usb.volume_active = true;
  // We start the USB stack manually below (after registering both the CDC
  // and Audio interfaces) so the composite descriptor is built correctly.
  cfg_usb.begin_usb = false;
  usbAudio.begin(cfg_usb);
  MySerial.println("usb audio configured");

  USB.begin();  // starts both the CDC debug port and the Audio interface
  MySerial.println("USB.begin() done");
}

void loop() {
  streamMicToUsb();
  streamUsbToSpeaker();
}
