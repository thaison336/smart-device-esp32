#!/usr/bin/env python3
"""
Live Microphone Streamer for Xiaozhi ESP32-S3 (Industrial FFmpeg Engine v2).
- Tự động khuếch đại âm lượng Mic Laptop (+12dB) để phát to rõ trên loa ESP32.
- Hỗ trợ kết nối / ngắt kết nối / bấm nút nói lại NHIỀU LẦN liên tục mà không bị đơ.
"""

import asyncio
import json
import socket
import struct
import subprocess
import threading
import time

import websockets

PORT = 8000
SAMPLE_RATE = 16000
FRAME_DURATION_MS = 60


def make_binary_packet_v2(payload: bytes, timestamp: int) -> bytes:
    """Đóng gói BinaryProtocol2 của Xiaozhi (Header 16 bytes)."""
    return struct.pack(">HHIII", 2, 0, 0, timestamp, len(payload)) + payload


def get_lan_ip():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        sock.close()


async def handle_esp_session(websocket):
    print("\n" + "=" * 55)
    print("[+] ESP32 ĐÃ KẾT NỐI VÀO WEBSOCKET!")
    try:
        hello_raw = await asyncio.wait_for(websocket.recv(), timeout=5.0)
    except Exception as e:
        print(f"[!] Lỗi nhận hello từ ESP32: {e}")
        return

    # 1. Gửi server hello
    server_hello = {
        "type": "hello",
        "transport": "websocket",
        "version": 2,
        "session_id": f"live-mic-{int(time.time())}",
        "audio_params": {
            "format": "opus",
            "sample_rate": SAMPLE_RATE,
            "channels": 1,
            "frame_duration": FRAME_DURATION_MS,
        },
    }
    await websocket.send(json.dumps(server_hello, separators=(",", ":")))
    print("[+] Đã bắt tay handshake thành công (v2)")

    # 2. Xả dữ liệu ngược từ ESP32 để không nghẽn TCP
    stop_event = threading.Event()

    async def drain_incoming():
        try:
            async for _ in websocket:
                pass
        except Exception:
            pass

    drain_task = asyncio.create_task(drain_incoming())
    await asyncio.sleep(0.15)

    # 3. Kích hoạt trạng thái SPEAKING trên ESP32
    await websocket.send(json.dumps({
        "session_id": server_hello["session_id"],
        "type": "tts",
        "state": "start",
    }, separators=(",", ":")))
    print("[+] Đã gửi tts:start -> ESP32 chuyển sang SPEAKING!")

    # 4. Mở Micro laptop qua FFmpeg với BỘ KHUẾCH ĐẠI ÂM LƯỢNG (+12dB và Limiter chống rè)
    cmd = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "dshow",
        "-i", "audio=Microphone Array (Intel® Smart Sound Technology for Digital Microphones)",
        "-af", "volume=12dB,alimiter=limit=0.95",  # TĂNG ÂM LƯỢNG MIC GẤP 4 LẦN + CHỐNG VỠ TIẾNG
        "-ar", str(SAMPLE_RATE),
        "-ac", "1",
        "-c:a", "libopus",
        "-b:a", "32k",
        "-frame_duration", str(FRAME_DURATION_MS),
        "-application", "voip",
        "-f", "ogg",
        "pipe:1"
    ]

    try:
        ffmpeg_proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0
        )
    except Exception as e:
        print(f"[!] Không thể khởi động FFmpeg: {e}")
        drain_task.cancel()
        return

    print("\n" + "#" * 55)
    print("🔴 MICROPHONE LAPTOP ĐANG BẬT (+12dB BOOST)!")
    print("   -> Bạn hãy nói vào mic laptop để nghe loa ESP32 phát to rõ!")
    print("   -> Bấm nút trên ESP32 để ngắt / kết nối lại tùy ý.")
    print("#" * 55 + "\n")

    loop = asyncio.get_running_loop()
    packet_queue = asyncio.Queue(maxsize=100)

    def ffmpeg_reader():
        buffer = bytearray()
        current_packet = bytearray()
        try:
            while not stop_event.is_set():
                chunk = ffmpeg_proc.stdout.read(512)
                if not chunk:
                    break
                buffer.extend(chunk)

                while True:
                    idx = buffer.find(b"OggS")
                    if idx == -1:
                        if len(buffer) > 4:
                            buffer = buffer[-4:]
                        break
                    if idx > 0:
                        buffer = buffer[idx:]

                    if len(buffer) < 27:
                        break

                    seg_count = buffer[26]
                    if len(buffer) < 27 + seg_count:
                        break

                    seg_table = buffer[27:27 + seg_count]
                    body_size = sum(seg_table)
                    total_page_len = 27 + seg_count + body_size
                    if len(buffer) < total_page_len:
                        break

                    body = buffer[27 + seg_count:total_page_len]
                    buffer = buffer[total_page_len:]

                    b_pos = 0
                    for seg in seg_table:
                        current_packet.extend(body[b_pos:b_pos + seg])
                        b_pos += seg
                        if seg < 255:
                            if not current_packet.startswith(b"OpusHead") and not current_packet.startswith(b"OpusTags"):
                                if len(current_packet) > 0:
                                    raw_opus = bytes(current_packet)
                                    loop.call_soon_threadsafe(
                                        lambda p=raw_opus: packet_queue.put_nowait(p) if not packet_queue.full() else None
                                    )
                            current_packet.clear()
        except Exception:
            pass

    reader_thread = threading.Thread(target=ffmpeg_reader, daemon=True)
    reader_thread.start()

    timestamp = 0
    sent_count = 0

    try:
        while True:
            raw_packet = await packet_queue.get()
            bin_packet = make_binary_packet_v2(raw_packet, timestamp)
            await websocket.send(bin_packet)
            timestamp += FRAME_DURATION_MS
            sent_count += 1

            if sent_count % 30 == 0:
                print(f"📡 Đang stream mic: {sent_count} frames...", end="\r")

    except (websockets.exceptions.ConnectionClosed, asyncio.CancelledError):
        print(f"\n[-] ESP32 đã ngắt kết nối (Tổng số frames đã gửi: {sent_count}).")
    finally:
        stop_event.set()
        try:
            ffmpeg_proc.terminate()
            ffmpeg_proc.wait(timeout=0.5)
        except Exception:
            pass
        drain_task.cancel()
        print("[i] Đã giải phóng Microphone & FFmpeg. Sẵn sàng cho lần kết nối tiếp theo!\n")


async def main():
    lan_ip = get_lan_ip()
    print("=" * 65)
    print("🎙️  XIAOZHI LIVE MIC STREAMER v2 (BOOSTED AUDIO + MULTI-RECONNECT) 🎙️")
    print(f"[*] WebSocket Server: ws://{lan_ip}:{PORT}/")
    print("[*] Đang lắng nghe kết nối từ ESP32...")
    print("=" * 65)

    async with websockets.serve(handle_esp_session, "0.0.0.0", PORT, max_size=None):
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n[!] Đã dừng chương trình Live Mic Streamer.")
