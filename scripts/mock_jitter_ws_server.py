#!/usr/bin/env python3
"""
Mock Xiaozhi WebSocket server for testing downstream Opus jitter handling.

The server can encode WAV/MP3/FLAC/OGG input to 16 kHz, mono, 60 ms Opus
frames with FFmpeg, then send those frames through a Xiaozhi binary protocol.
"""

import argparse
import asyncio
import random
import struct
import subprocess
import tempfile
import time
import json
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import websockets


def parse_jitter(value):
    value = value.strip()
    if "-" in value:
        low, high = value.split("-", 1)
        return int(low), int(high)
    jitter = int(value)
    return jitter, jitter


def encode_opus(input_path):
    if input_path.suffix.lower() in (".ogg", ".opus"):
        return input_path

    temp = tempfile.NamedTemporaryFile(suffix=".ogg", delete=False)
    temp.close()
    command = [
        "ffmpeg", "-y", "-v", "error", "-i", str(input_path),
        "-ar", "24000", "-ac", "1", "-c:a", "libopus",
        "-b:a", "32k", "-frame_duration", "60",
        "-application", "audio", "-f", "ogg", temp.name,
    ]
    subprocess.run(command, check=True)
    return Path(temp.name)


def read_ogg_packets(path):
    data = path.read_bytes()
    packets = []
    offset = 0

    while offset + 27 <= len(data):
        if data[offset:offset + 4] != b"OggS":
            offset += 1
            continue

        segment_count = data[offset + 26]
        segment_table_start = offset + 27
        segment_end = segment_table_start + segment_count
        if segment_end > len(data):
            break

        segment_table = data[segment_table_start:segment_end]
        payload_start = segment_end
        current = bytearray()

        for segment_size in segment_table:
            if payload_start + segment_size > len(data):
                return packets
            segment = data[payload_start:payload_start + segment_size]
            payload_start += segment_size
            current.extend(segment)
            if segment_size < 255:
                packets.append(bytes(current))
                current.clear()

        offset = payload_start

    return packets


def opus_payloads(path):
    packets = read_ogg_packets(path)
    if not packets:
        raise RuntimeError("No Opus packets found in Ogg file")

    payloads = []
    for packet in packets:
        if packet.startswith(b"OpusHead") or packet.startswith(b"OpusTags"):
            continue
        if len(packet) > 0:
            payloads.append(packet)
    if not payloads:
        raise RuntimeError("Ogg file only contained Opus metadata")
    return payloads


def binary_packet(version, payload, timestamp):
    if version == 2:
        return struct.pack(">HHIII", 2, 0, 0, timestamp, len(payload)) + payload
    if version == 3:
        return struct.pack(">BBH", 0, 0, len(payload)) + payload
    return payload


def get_lan_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


def start_ota_server(args, ws_url):
    ota_response = json.dumps({
        "websocket": {
            "url": ws_url,
            "token": "",
            "version": args.protocol,
        },
        "server_time": {
            "timestamp": int(time.time() * 1000),
            "timezone_offset": 420,
        },
    }, separators=(",", ":"))

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *values):
            print(f"[ota] {self.address_string()} - {format % values}")

        def send_json(self, status, body):
            data = body.encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path.rstrip("/") == "/ota":
                self.send_json(200, ota_response)
            elif self.path == "/health":
                self.send_json(200, '{"ok":true}')
            else:
                self.send_json(404, '{"error":"not found"}')

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length).decode("utf-8", errors="replace")
            print(f"[ota] POST {self.path}")
            if body:
                print(f"[ota] ESP payload: {body[:500]}")

            if self.path.rstrip("/") == "/ota":
                self.send_json(200, ota_response)
            elif self.path.rstrip("/") == "/ota/activate":
                self.send_json(200, '{"activation":"ok"}')
            else:
                self.send_json(404, '{"error":"not found"}')

    server = ThreadingHTTPServer(("0.0.0.0", args.ota_port), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    print(f"[ota] Listening on http://0.0.0.0:{args.ota_port}/ota/")
    return server


async def main(args):
    jitter_low, jitter_high = parse_jitter(args.jitter)
    source = encode_opus(args.input)
    payloads = opus_payloads(source)
    print(f"[mock] Encoded source: {source}")
    print(f"[mock] Opus packets: {len(payloads)}")
    lan_ip = args.lan_ip or get_lan_ip()
    ws_url = f"ws://{lan_ip}:{args.port}"
    ota_server = start_ota_server(args, ws_url)
    print(f"[ota] ESP OTA URL: http://{lan_ip}:{args.ota_port}/ota/")
    print(f"[ota] ESP WebSocket: {ws_url}")
    print(f"[ota] Protocol: {args.protocol}")

    random.seed(args.seed)

    async def handler(websocket):
        hello = await websocket.recv()
        print(f"[mock] Client hello: {hello}")

        try:
            import json
            client = json.loads(hello)
            version = int(client.get("version", 1))
        except Exception:
            version = 1

        session_id = f"mock-{int(time.time())}"
        server_hello = {
            "type": "hello",
            "transport": "websocket",
            "version": version,
            "session_id": session_id,
            "audio_params": {
                "format": "opus",
                "sample_rate": 24000,
                "channels": 1,
                "frame_duration": 60,
            },
        }
        await websocket.send(json.dumps(server_hello, separators=(",", ":")))
        print(f"[mock] Sent server hello, protocol v{version}")

        # Drain incoming uplink audio from ESP to prevent TCP window saturation
        async def drain_incoming():
            try:
                async for message in websocket:
                    if isinstance(message, str):
                        try:
                            msg_obj = json.loads(message)
                            print(f"[mock] ESP32 event: {msg_obj.get('type')} {msg_obj.get('state', '')}")
                        except Exception:
                            print(f"[mock] ESP32 text: {message[:80]}")
            except Exception:
                pass

        drain_task = asyncio.create_task(drain_incoming())

        await asyncio.sleep(args.start_delay / 1000)
        await websocket.send(json.dumps({
            "session_id": session_id,
            "type": "tts",
            "state": "start",
        }, separators=(",", ":")))

        sent = 0
        lost = 0
        timestamp = 0
        stream_start_time = time.monotonic()

        # Bơm trước 8 frames (480ms) làm Initial Burst để lấp đầy "túi đệm an toàn"
        initial_burst_count = min(8, len(payloads))
        for index in range(initial_burst_count):
            await websocket.send(binary_packet(version, payloads[index], timestamp))
            sent += 1
            timestamp += 60

        for rep in range(args.repeat):
            start_idx = initial_burst_count if rep == 0 else 0
            for index in range(start_idx, len(payloads)):
                payload = payloads[index]
                if random.randint(1, 100) <= args.packet_loss:
                    lost += 1
                    continue

                await websocket.send(binary_packet(version, payload, timestamp))
                sent += 1
                timestamp += 60

                # Mô phỏng một cú nấc cục mạng (Single Hiccup) rồi bắn bù ngay sau đó
                if args.hiccup_at > 0 and sent == args.hiccup_at:
                    print(f"\n[mock] >>> BẮT ĐẦU CÚ LAG MẠNG GIẢ LẬP: {args.hiccup_ms} ms tại gói #{sent} <<<")
                    await asyncio.sleep(args.hiccup_ms / 1000.0)
                    print(f"[mock] >>> MẠNG THÔNG LẠI! BẮN DỒN BÙ CÁC GÓI TIẾP THEO <<<")
                    continue

                if args.burst > 0 and sent % args.burst == 0:
                    await asyncio.sleep(args.burst_gap / 1000)
                else:
                    # Lead Pacing: Nhắm nhịp 57ms/frame (nhanh hơn tốc độ tiêu thụ 60ms một chút)
                    # để queue luôn có dự phòng, không bao giờ rơi về 0, nhưng không chạm ngưỡng tràn 20 frame.
                    frame_target_sec = 0.057
                    expected_time = stream_start_time + (sent * frame_target_sec)
                    now = time.monotonic()
                    sleep_needed = expected_time - now

                    if jitter_high > jitter_low:
                        jitter_offset = random.randint(jitter_low - 60, jitter_high - 60) / 1000.0
                        sleep_needed += jitter_offset

                    if sleep_needed > 0:
                        await asyncio.sleep(sleep_needed)

            if args.repeat_gap > 0 and rep < args.repeat - 1:
                await asyncio.sleep(args.repeat_gap / 1000)
                stream_start_time = time.monotonic()
                sent = 0

        elapsed = time.monotonic() - stream_start_time
        await websocket.send(json.dumps({
            "session_id": session_id,
            "type": "tts",
            "state": "stop",
        }, separators=(",", ":")))
        print(
            f"[mock] Stream complete: sent={sent}, lost={lost}, "
            f"jitter={jitter_low}-{jitter_high} ms, elapsed={elapsed:.3f} s"
        )

        await asyncio.Future()

    async with websockets.serve(handler, "0.0.0.0", args.port, max_size=None):
        print(f"[mock] Listening on ws://0.0.0.0:{args.port}")
        print("[mock] Press Ctrl+C to stop")
        await asyncio.Future()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path, help="WAV/MP3/FLAC/OGG/Opus input")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--ota-port", type=int, default=8001)
    parser.add_argument("--lan-ip", default=None,
                        help="LAN IP advertised to ESP; auto-detected by default")
    parser.add_argument("--protocol", type=int, choices=(1, 2, 3), default=2,
                        help="Xiaozhi binary protocol version")
    parser.add_argument("--jitter", default="80-120",
                        help="Random inter-frame delay in ms, or fixed ms")
    parser.add_argument("--packet-loss", type=int, default=0,
                        help="Packet loss probability percent")
    parser.add_argument("--burst", type=int, default=0,
                        help="Frames sent before a burst gap")
    parser.add_argument("--burst-gap", type=int, default=0,
                        help="Gap between bursts in ms")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--repeat-gap", type=int, default=500)
    parser.add_argument("--start-delay", type=int, default=250,
                        help="Delay before tts/start in ms")
    parser.add_argument("--hiccup-at", type=int, default=0,
                        help="Inject a single simulated network hiccup at packet N")
    parser.add_argument("--hiccup-ms", type=int, default=140,
                        help="Duration of the single hiccup in ms")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    asyncio.run(main(args))
