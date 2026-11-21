#!/usr/bin/env python3
"""
ESP32 Log Viewer (Terminal 2)
Ket noi den TCP Log Server cua pc_stm32_simulator.py (localhost:9999)
de hien thi rieng biet toan bo luong log ESP-IDF ma khong lam roi Terminal 1.
"""
import sys
import socket
import time

def main():
    host = "127.0.0.1"
    port = 9999
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except ValueError:
            host = sys.argv[1]
    if len(sys.argv) > 2:
        port = int(sys.argv[2])

    print("=" * 65)
    print(f" ESP32 LOG MONITOR (TERMINAL 2)")
    print(f" Dang ket noi den STM32 Simulator tai {host}:{port}...")
    print(" Nhan Ctrl + C de thoat.")
    print("=" * 65)

    while True:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((host, port))
            print(f"\n[+] KET NOI THANH CONG! Dang truyen luong log ESP32...\n" + "-" * 65)
            while True:
                data = s.recv(4096)
                if not data:
                    print("\n[!] STM32 Simulator da dong ket noi.")
                    break
                sys.stdout.write(data.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            s.close()
            time.sleep(1)
        except ConnectionRefusedError:
            print("[*] Dang cho pc_stm32_simulator.py khoi dong... (thu lai sau 2s)", end="\r")
            time.sleep(2)
        except KeyboardInterrupt:
            print("\n[*] Da dong Log Viewer.")
            break
        except Exception as e:
            print(f"\n[!] Loi ket noi: {e}. Thu ket noi lai sau 2s...")
            time.sleep(2)

if __name__ == "__main__":
    main()
