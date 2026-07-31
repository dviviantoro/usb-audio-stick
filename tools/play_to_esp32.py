#!/usr/bin/env python3
"""Play a WAV file through the ESP32-S3 speaker (PCM5102) firmware over UDP.

The WAV file must be 16-bit PCM at 16000 Hz to match the firmware's I2S
config -- anything else will play at the wrong speed/pitch since the device
does not resample. Mono files are automatically duplicated to stereo, since
the firmware's I2S TX is configured for 2 channels.

Usage:
    python3 play_to_esp32.py <esp32-ip> <file.wav> [--port 5006]
"""
import argparse
import socket
import sys
import time
import wave

EXPECTED_RATE = 16000
CHUNK_FRAMES = 256  # stereo 16-bit frames per UDP packet


def to_stereo(frames: bytes, channels: int) -> bytes:
    if channels == 2:
        return frames
    if channels != 1:
        print(f"Unsupported channel count: {channels}", file=sys.stderr)
        sys.exit(1)
    # duplicate each mono 16-bit sample into L+R
    out = bytearray(len(frames) * 2)
    for i in range(0, len(frames), 2):
        out[i * 2:i * 2 + 2] = frames[i:i + 2]
        out[i * 2 + 2:i * 2 + 4] = frames[i:i + 2]
    return bytes(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("esp32_ip", help="IP address printed by the ESP32 over Serial")
    parser.add_argument("wav_file")
    parser.add_argument("--port", type=int, default=5006)
    args = parser.parse_args()

    with wave.open(args.wav_file, "rb") as wf:
        if wf.getsampwidth() != 2:
            print("WAV file must be 16-bit PCM.", file=sys.stderr)
            sys.exit(1)
        if wf.getframerate() != EXPECTED_RATE:
            print(f"Warning: file is {wf.getframerate()} Hz, device expects "
                  f"{EXPECTED_RATE} Hz -- playback speed/pitch will be off.")
        channels = wf.getnchannels()
        frames = wf.readframes(wf.getnframes())

    stereo = to_stereo(frames, channels)
    bytes_per_chunk = CHUNK_FRAMES * 2 * 2  # frames * channels * bytes_per_sample
    chunk_seconds = CHUNK_FRAMES / EXPECTED_RATE

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"Sending {len(stereo)} bytes to {args.esp32_ip}:{args.port} ...")
    next_send = time.time()
    for offset in range(0, len(stereo), bytes_per_chunk):
        chunk = stereo[offset:offset + bytes_per_chunk]
        sock.sendto(chunk, (args.esp32_ip, args.port))
        next_send += chunk_seconds
        sleep_for = next_send - time.time()
        if sleep_for > 0:
            time.sleep(sleep_for)

    print("Done.")


if __name__ == "__main__":
    main()
