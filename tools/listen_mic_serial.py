"""
Script thu âm từ cổng COM Type-C của ESP32-S3 và ghi thành file WAV trên máy tính.
Cài đặt thư viện cần thiết:
    pip install pyserial

Cách chạy:
    python tools/listen_mic_serial.py --port COM5 --duration 10 --out mic_test.wav
"""

import sys
import struct
import wave
import argparse

try:
    import serial
except ImportError:
    print("[ERROR] Chưa cài đặt thư viện 'pyserial'.")
    print("Vui lòng chạy lệnh sau trên Command Prompt / PowerShell: pip install pyserial")
    sys.exit(1)

MAGIC_HEADER = bytes([0xA5, 0x5A])
SAMPLE_RATE = 16000  # 16kHz
CHANNELS = 1         # Mono
SAMPLE_WIDTH = 2     # 16-bit (2 bytes per sample)

def record_serial_audio(port, baudrate=921600, duration_sec=10, output_wav="mic_capture.wav"):
    print("=" * 60)
    print(f"[*] Đang mở cổng {port} với tốc độ baud {baudrate}...")
    try:
        ser = serial.Serial(port, baudrate, timeout=1.0)
    except Exception as e:
        print(f"[!] Lỗi: Không thể mở cổng {port}: {e}")
        print("    -> Vui lòng kiểm tra lại cổng COM trong Device Manager!")
        return

    frames = bytearray()
    expected_bytes = duration_sec * SAMPLE_RATE * SAMPLE_WIDTH
    print(f"[*] Đã kết nối thành công!")
    print(f"[*] Bắt đầu thu âm trong {duration_sec} giây...")
    print(f"[*] HÃY NÓI THỬ VÀO MICRO TRÊN KIT WAVESHARE...")
    print("=" * 60)

    buffer = bytearray()
    try:
        while len(frames) < expected_bytes:
            chunk = ser.read(1024)
            if not chunk:
                continue
            buffer.extend(chunk)

            # Quét tìm magic byte 0xA5, 0x5A
            while len(buffer) >= 4:
                idx = buffer.find(MAGIC_HEADER)
                if idx == -1:
                    buffer = buffer[-1:] # Giữ byte cuối phòng trường hợp bị cắt giữa 0xA5 và 0x5A
                    break
                if idx > 0:
                    buffer = buffer[idx:]

                if len(buffer) < 4:
                    break

                # Đọc độ dài payload (uint16_t little-endian)
                length = struct.unpack("<H", buffer[2:4])[0]
                if len(buffer) < 4 + length:
                    break # Chưa nhận đủ nguyên frame, đợi thêm chunk sau

                # Lấy payload PCM
                pcm_data = buffer[4 : 4 + length]
                frames.extend(pcm_data)
                buffer = buffer[4 + length :]
                
                # Cập nhật tiến độ
                progress = min(100, int(len(frames) / expected_bytes * 100))
                print(f"\rTiến độ: {progress}% [{len(frames)}/{expected_bytes} bytes]", end="", flush=True)

    except KeyboardInterrupt:
        print("\n[!] Đã dừng thu âm sớm bởi người dùng.")
    finally:
        ser.close()

    if len(frames) == 0:
        print("\n[!] Không nhận được dữ liệu âm thanh nào từ cổng COM!")
        print("    Hãy chắc chắn firmware trên kit đã kích hoạt cờ xuất Serial audio.")
        return

    print(f"\n[*] Đang đóng gói file WAV ({len(frames)} bytes)...")
    with wave.open(output_wav, "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(frames)

    print("=" * 60)
    print(f"[+] THU ÂM THÀNH CÔNG: {output_wav}")
    print(f"[+] Bạn có thể click đúp chuột vào file '{output_wav}' để nghe trên máy tính!")
    print("=" * 60)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Thu âm từ ESP32-S3 qua cổng Type-C Serial")
    parser.add_argument("--port", type=str, default="COM3", help="Tên cổng COM của kit (VD: COM3, COM5...)")
    parser.add_argument("--baud", type=int, default=921600, help="Tốc độ baud (khuyên dùng 921600 hoặc 115200)")
    parser.add_argument("--duration", type=int, default=10, help="Thời gian thu âm (giây)")
    parser.add_argument("--out", type=str, default="mic_test.wav", help="Tên file WAV đầu ra")
    args = parser.parse_args()

    record_serial_audio(args.port, args.baud, args.duration, args.out)
