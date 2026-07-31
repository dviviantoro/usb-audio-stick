#!/usr/bin/env python3
"""Record audio streamed from the ESP32-S3 mic firmware over UDP.

Usage:
    python3 record_from_esp32.py <esp32-ip> [--duration 5] [--port 5005] [--out recording.wav]
"""
import argparse
import socket
import sys
import time
import wave

SAMPLE_RATE = 16000
SAMPLE_WIDTH = 2  # bytes (16-bit)
CHANNELS = 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("esp32_ip", help="IP address printed by the ESP32 over Serial")
    parser.add_argument("--port", type=int, default=5005)
    parser.add_argument("--duration", type=float, default=5.0, help="seconds to record")
    parser.add_argument("--out", default="recording.wav")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)

    # "Register" as the listener -- the firmware streams mic audio to
    # whoever last sent it a packet on this port.
    sock.sendto(b"hello", (args.esp32_ip, args.port))

    frames = bytearray()
    start = time.time()
    got_any_data = False

    print(f"Recording for {args.duration}s from {args.esp32_ip}:{args.port} ...")
    while time.time() - start < args.duration:
        try:
            data, _ = sock.recvfrom(4096)
            frames.extend(data)
            got_any_data = True
        except socket.timeout:
            if not got_any_data:
                print("No data received yet -- re-sending registration packet "
                      "(check the IP/WiFi and that the device is running).")
                sock.sendto(b"hello", (args.esp32_ip, args.port))

    if not got_any_data:
        print("Never received any audio. Nothing written.", file=sys.stderr)
        sys.exit(1)

    with wave.open(args.out, "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(bytes(frames))

    seconds = len(frames) / SAMPLE_WIDTH / SAMPLE_RATE
    print(f"Wrote {args.out} ({seconds:.2f}s of audio, {len(frames)} bytes)")


if __name__ == "__main__":
    main()
