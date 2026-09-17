#include <Arduino.h>
#include <USB.h>
#include "AudioTools.h"
#include "AudioTools/Communication/USB/USBAudioStream.h"

// ---- INMP441 microphone (I2S port 0, RX) ----
#define MIC_SCK 4
#define MIC_WS  5
#define MIC_SD  3

// Stereo capture needs a second INMP441 sharing this same SCK/WS/SD bus,
// with its L/R select pin tied to VDD instead of GND -- each INMP441 only
// drives SD during its own half of the WS cycle and tri-states the rest, so
// both chips share one data line safely; no extra pins needed. Override via
// build_flags (-D MIC_STEREO=1) for a per-environment toggle instead of
// editing this. Note: the USB Audio Class descriptor shares one channel
// count between the mic (IN) and speaker (OUT) streams, so enabling this
// also switches the playback path to genuine stereo from the host --
// see streamUsbToSpeaker().
#ifndef MIC_STEREO
#define MIC_STEREO 0
#endif
#if MIC_STEREO
constexpr int MIC_CHANNELS = 2;
#else
constexpr int MIC_CHANNELS = 1;
#endif

// ---- PCM5102 DAC (I2S port 1, TX) ----
// PCM5102 breakout notes: also wire VCC/GND, and tie its SCK (system clock)
// pin to GND if your board doesn't already do this -- most breakout boards
// ground it by default since we don't feed an external master clock.
#define DAC_BCK 8  // BCK
#define DAC_WS  10   // LCK
#define DAC_SD  9   // DIN

constexpr int SAMPLE_RATE = 16000;
constexpr int MIC_READ_SAMPLES = 256;  // 32-bit frames per I2S read, per channel

// ---- Mic noise gate (ambient/background noise suppression) ----
// Fades the mic signal towards silence when its level stays below the
// threshold, with a fast attack (opens quickly when speech starts) and slow
// release (fades out gradually instead of clicking when speech stops). This
// suppresses quiet, continuous background noise (room hiss, fan) between
// speech -- it does not remove noise happening *under* speech, which would
// need real spectral/ML denoising instead.
#ifndef NOISE_GATE_THRESHOLD
#define NOISE_GATE_THRESHOLD 400  // 16-bit PCM magnitude -- tune to your mic/room
#endif
constexpr float NOISE_GATE_ATTACK = 0.02f;    // ~3ms time constant at 16kHz
constexpr float NOISE_GATE_RELEASE = 0.0005f; // ~125ms time constant at 16kHz
float noiseGateGain = 0.0f;

void applyNoiseGate(int16_t* samples, size_t count) {
  for (size_t i = 0; i < count; i++) {
    int32_t mag = abs((int32_t)samples[i]);
    float target = (mag >= NOISE_GATE_THRESHOLD) ? 1.0f : 0.0f;
    float rate = (target > noiseGateGain) ? NOISE_GATE_ATTACK : NOISE_GATE_RELEASE;
    noiseGateGain += (target - noiseGateGain) * rate;
    samples[i] = (int16_t)(samples[i] * noiseGateGain);
  }
}

// USBAudioStream on ESP32 requires ARDUINO_USB_CDC_ON_BOOT=0 (OTG mode
// only), so the default "Serial" object doesn't route over USB. This gives
// us a second, composite CDC interface for debug logging over the same
// cable, alongside the Audio class.
USBCDC MySerial;

I2SStream i2sMic;
I2SStream i2sSpeaker;
USBAudioStream usbAudio;

#if MIC_STEREO
// MIC_STEREO also makes the USB playback stream genuine stereo (see the
// shared-channel-count note above), so it already matches the DAC's L+R
// layout -- pass it straight through.
void streamUsbToSpeaker() {
  static int16_t stereo[MIC_READ_SAMPLES * 2];

  size_t bytesRead = usbAudio.readBytes((uint8_t*)stereo, sizeof(stereo));
  if (bytesRead == 0) return;
  i2sSpeaker.write((const uint8_t*)stereo, bytesRead);
}
#else
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
#endif

void streamMicToUsb() {
  static int32_t raw[MIC_READ_SAMPLES * MIC_CHANNELS];
  static int16_t pcm[MIC_READ_SAMPLES * MIC_CHANNELS];

  size_t bytesRead = i2sMic.readBytes((uint8_t*)raw, sizeof(raw));
  size_t samplesRead = bytesRead / sizeof(int32_t);
  if (samplesRead == 0) return;

  for (size_t i = 0; i < samplesRead; i++) {
    int32_t v = (raw[i] >> 8) & 0xFFFFFF;
    if (v & 0x800000) v |= 0xFF000000;  // sign-extend 24-bit sample
    pcm[i] = (int16_t)(v >> 8);         // downscale to 16-bit
  }

  applyNoiseGate(pcm, samplesRead);

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
  cfg_mic.channels = MIC_CHANNELS;
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
  // The UAC2 descriptor builder uses one channel count for both the mic (IN)
  // and speaker (OUT) streaming interfaces, so this follows MIC_CHANNELS --
  // see streamUsbToSpeaker() for how the playback side adapts.
  cfg_usb.channels = MIC_CHANNELS;
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
