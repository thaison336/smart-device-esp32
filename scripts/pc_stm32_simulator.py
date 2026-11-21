#!/usr/bin/env python3
"""
PC STM32 Simulator & Demuxer Serial Monitor
Giao tiếp 1 cáp Type-C với ESP32-S3 (UART0 115200 bps):
- Lắng nghe byte stream từ ESP32:
  * Tách luồng ESP_LOGI/ESP_LOGE và phát sóng qua TCP Server (localhost:9999) cho Terminal 2.
  * Tách khung nhị phân StreamFramer: [0xAA][0x55][0x01][TYPE][LEN_H][LEN_L][PAYLOAD][CRC16_H][CRC16_L]
  * Tự động phản hồi CommandAck (ACK_APPLIED) khi nhận ControlCommand.
- Tự động phát Heartbeat (mỗi 2s) và SensorTelemetry (mỗi 1s).
- Cho phép người dùng nhập lệnh tương tác trên CLI (Terminal 1 hoàn toàn sạch, không bị trôi log):
  * 't <val>' : set nhiệt độ (vd: 't 32.5')
  * 'h <val>' : set độ ẩm (vd: 'h 70')
  * 'p <val>' : set áp suất (vd: 'p 1015')
  * 'v <val>' : set điện áp pin (vd: 'v 4.15')
  * 'hang'    : giả lập STM32 bị treo
  * 'normal'  : khôi phục bình thường
  * 'crc'     : giả lập gửi frame sai CRC
  * 'timeout' : giả lập không trả ACK (timeout)
  * 'busy'    : giả lập trả NACK_BUSY
  * 'log on' / 'log off': Bật/tắt in log ESP trực tiếp tại Terminal 1
"""

import sys
import os
import time
import argparse
import threading
import struct
import socket

# Kiểm tra & tự động compile protobuf nếu thiếu
script_dir = os.path.dirname(os.path.abspath(__file__))
project_root = os.path.abspath(os.path.join(script_dir, ".."))
proto_dir = os.path.join(project_root, "main", "stm32_bridge", "proto")
proto_file = os.path.join(proto_dir, "datalogger.proto")

try:
    import serial
except ImportError:
    print("[!] Dang thieu thu vien pyserial. Hay chay: pip install pyserial")
    sys.exit(1)

try:
    sys.path.insert(0, proto_dir)
    import datalogger_pb2
except ImportError:
    print("[*] Bien dich datalogger.proto sang Python protobuf...")
    import subprocess
    cmd = [sys.executable, "-m", "grpc_tools.protoc", f"-I{proto_dir}", f"--python_out={proto_dir}", proto_file]
    subprocess.run(cmd, check=True)
    import datalogger_pb2

def compute_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= (byte << 8)
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc

def pack_frame(frame_type: int, payload: bytes, corrupt_crc: bool = False) -> bytes:
    sync0, sync1 = 0xAA, 0x55
    version = 0x01
    length = len(payload)
    header = struct.pack(">BBBBH", sync0, sync1, version, frame_type, length)
    
    # CRC tính từ VERSION (bỏ qua sync0, sync1) đến hết payload
    crc_payload = bytes([version, frame_type, (length >> 8) & 0xFF, length & 0xFF]) + payload
    crc = compute_crc16(crc_payload)
    if corrupt_crc:
        crc ^= 0xFFFF
    crc_bytes = struct.pack(">H", crc)
    return header + payload + crc_bytes

class LogBroadcaster:
    """TCP Server phát toàn bộ log ESP-IDF sang Terminal 2 (localhost:9999)"""
    def __init__(self, host="127.0.0.1", port=9999):
        self.host = host
        self.port = port
        self.clients = []
        self.lock = threading.Lock()
        self.running = True
        self.server_sock = None

    def start(self):
        try:
            self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_sock.bind((self.host, self.port))
            self.server_sock.listen(5)
            threading.Thread(target=self._accept_loop, daemon=True).start()
        except Exception as e:
            print(f"[!] Khong the khoi tao Log Server tren {self.host}:{self.port}: {e}")

    def _accept_loop(self):
        while self.running:
            try:
                client, _ = self.server_sock.accept()
                with self.lock:
                    self.clients.append(client)
            except Exception:
                break

    def broadcast(self, data: bytes):
        with self.lock:
            disconnected = []
            for client in self.clients:
                try:
                    client.sendall(data)
                except Exception:
                    disconnected.append(client)
            for d in disconnected:
                self.clients.remove(d)

    def stop(self):
        self.running = False
        if self.server_sock:
            try:
                self.server_sock.close()
            except Exception:
                pass

class Stm32Simulator:
    def __init__(self, port: str, baudrate: int = 115200, show_esp_log: bool = False, log_port: int = 9999):
        self.port_name = port
        self.baudrate = baudrate
        self.show_esp_log = show_esp_log
        self.log_port = log_port
        self.ser = serial.Serial(port, baudrate, timeout=0.05)
        self.running = True

        # Trạng thái mô phỏng
        self.temperature = 28.5
        self.humidity = 65.0
        self.pressure = 1013.25
        self.vbat = 4.05
        self.current_ma = 135.0
        self.uptime_sec = 0
        self.scenario = "normal"  # normal, hang, timeout, busy, corrupt_crc

        self.rx_buffer = bytearray()
        self.lock = threading.Lock()

        # Khoi dong Log Broadcaster sang Terminal 2
        self.broadcaster = LogBroadcaster(port=self.log_port)
        self.broadcaster.start()

    def send_handshake(self):
        env = datalogger_pb2.BridgeEnvelope()
        env.protocol_version = 1
        env.session_id = 2026
        env.msg_type = datalogger_pb2.MSG_TYPE_HANDSHAKE
        env.sequence_id = 1
        env.source_id = "STM32F4-PC"
        env.handshake.protocol_version = 1
        env.handshake.hardware_version = 2
        env.handshake.firmware_version = 100
        env.handshake.device_serial = "STM32-PC-SIMULATOR"

        frame = pack_frame(0x00, env.SerializeToString())
        with self.lock:
            self.ser.write(frame)
        print("[TX -> ESP32] Handshake sent")

    def send_heartbeat(self):
        if self.scenario == "hang":
            return
        self.uptime_sec += 2
        env = datalogger_pb2.BridgeEnvelope()
        env.protocol_version = 1
        env.session_id = 2026
        env.msg_type = datalogger_pb2.MSG_TYPE_HEARTBEAT
        env.sequence_id = 0
        env.source_id = "STM32F4-PC"
        env.heartbeat.uptime_sec = self.uptime_sec
        env.heartbeat.free_heap = 64000
        env.heartbeat.status_flags = 0x01

        corrupt = (self.scenario == "corrupt_crc")
        frame = pack_frame(0x00, env.SerializeToString(), corrupt_crc=corrupt)
        with self.lock:
            self.ser.write(frame)

    def send_telemetry(self):
        if self.scenario == "hang":
            return
        env = datalogger_pb2.BridgeEnvelope()
        env.protocol_version = 1
        env.session_id = 2026
        env.msg_type = datalogger_pb2.MSG_TYPE_TELEMETRY
        env.sequence_id = 0
        env.source_id = "STM32F4-PC"
        env.telemetry.temperature = float(self.temperature)
        env.telemetry.humidity = float(self.humidity)
        env.telemetry.pressure = float(self.pressure)
        env.telemetry.battery_voltage = float(self.vbat)
        env.telemetry.current_mA = float(self.current_ma)
        env.telemetry.digital_inputs = 0x03
        env.telemetry.analog_channels.extend([1.23, 2.45])
        env.telemetry.error_message = "SD_OK"

        corrupt = (self.scenario == "corrupt_crc")
        frame = pack_frame(0x00, env.SerializeToString(), corrupt_crc=corrupt)
        with self.lock:
            self.ser.write(frame)

    def handle_frame(self, frame_type: int, payload: bytes):
        try:
            env = datalogger_pb2.BridgeEnvelope()
            env.ParseFromString(payload)
            if env.msg_type == datalogger_pb2.MSG_TYPE_COMMAND:
                cmd = env.command
                print(f"\n[RX <- ESP32 COMMAND] Seq={env.sequence_id}, Action='{cmd.action}', ParamInt={cmd.param_int}, ParamFlt={cmd.param_float}, ParamStr='{cmd.param_str}'")

                if self.scenario == "timeout" or self.scenario == "hang":
                    print("  -> Simulator in TIMEOUT/HANG mode. Ignoring ACK.")
                    return

                # Gửi ACK
                ack_env = datalogger_pb2.BridgeEnvelope()
                ack_env.protocol_version = 1
                ack_env.session_id = 2026
                ack_env.msg_type = datalogger_pb2.MSG_TYPE_ACK
                ack_env.sequence_id = env.sequence_id
                ack_env.source_id = "STM32F4-PC"

                if self.scenario == "busy":
                    ack_env.ack.command_seq = env.sequence_id
                    ack_env.ack.status = datalogger_pb2.ACK_STATUS_BUSY
                    ack_env.ack.result_code = -1
                    ack_env.ack.result_msg = "BUSY_MOTOR_RUNNING"
                else:
                    ack_env.ack.command_seq = env.sequence_id
                    ack_env.ack.status = datalogger_pb2.ACK_STATUS_APPLIED
                    ack_env.ack.result_code = 0
                    ack_env.ack.result_msg = "SUCCESS_APPLIED"

                frame = pack_frame(0x00, ack_env.SerializeToString())
                with self.lock:
                    self.ser.write(frame)
                print(f"  -> [TX -> ESP32 ACK] Sent for Seq={env.sequence_id} ({ack_env.ack.result_msg})\nSTM32> ", end="", flush=True)

            elif env.msg_type == datalogger_pb2.MSG_TYPE_HANDSHAKE:
                print("\n[RX <- ESP32 HANDSHAKE PROBE] Tra loi handshake...")
                self.send_handshake()
                print("STM32> ", end="", flush=True)

        except Exception as e:
            print(f"[!] Loi parse Protobuf: {e}")

    def demux_stream(self, data: bytes):
        """Tach luong byte stream: ban log text sang TCP (Terminal 2), trich xuat frame nhi phan"""
        self.rx_buffer.extend(data)
        while len(self.rx_buffer) > 0:
            # Tim vi tri sync header 0xAA 0x55
            idx = self.rx_buffer.find(b"\xAA\x55")
            if idx == -1:
                # Toan bo byte hien tai la Log ASCII cua ESP-IDF
                text_bytes = bytes(self.rx_buffer)
                self.rx_buffer.clear()

                # 1. Phat song sang Terminal 2 qua TCP
                self.broadcaster.broadcast(text_bytes)

                # 2. Chi in ra Terminal 1 neu bat show_esp_log
                if self.show_esp_log:
                    sys.stdout.write(text_bytes.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                break
            elif idx > 0:
                # In cac byte log truoc header frame
                text_bytes = bytes(self.rx_buffer[:idx])

                # 1. Phat song sang Terminal 2 qua TCP
                self.broadcaster.broadcast(text_bytes)

                # 2. Chi in ra Terminal 1 neu bat show_esp_log
                if self.show_esp_log:
                    sys.stdout.write(text_bytes.decode("utf-8", errors="replace"))
                    sys.stdout.flush()

                self.rx_buffer = self.rx_buffer[idx:]

            # Da tim thay 0xAA 0x55 o dau buffer
            if len(self.rx_buffer) < 6:
                # Chua du header (6 bytes: SYNC0, SYNC1, VER, TYPE, LEN_H, LEN_L)
                break

            ver = self.rx_buffer[2]
            frame_type = self.rx_buffer[3]
            length = (self.rx_buffer[4] << 8) | self.rx_buffer[5]

            if ver != 0x01 or length > 512:
                # Khong phai frame hop le -> bo qua 1 byte roi quet tiep
                b_byte = bytes([self.rx_buffer[0]])
                self.broadcaster.broadcast(b_byte)
                if self.show_esp_log:
                    sys.stdout.write(chr(self.rx_buffer[0]))
                    sys.stdout.flush()
                self.rx_buffer.pop(0)
                continue

            total_frame_len = 6 + length + 2
            if len(self.rx_buffer) < total_frame_len:
                # Chua du toan bo payload va CRC
                break

            # Tach payload va kiem tra CRC
            payload = bytes(self.rx_buffer[6 : 6 + length])
            recv_crc = (self.rx_buffer[6 + length] << 8) | self.rx_buffer[6 + length + 1]

            crc_calc = compute_crc16(bytes([ver, frame_type, self.rx_buffer[4], self.rx_buffer[5]]) + payload)
            if crc_calc == recv_crc:
                # Frame dung!
                self.handle_frame(frame_type, payload)
            else:
                print(f"\n[!] CRC mismatch tren frame nhan: got {recv_crc:04X}, expected {crc_calc:04X}")

            self.rx_buffer = self.rx_buffer[total_frame_len:]

    def rx_worker(self):
        while self.running:
            try:
                data = self.ser.read(64)
                if data:
                    self.demux_stream(data)
            except Exception as e:
                if self.running:
                    print(f"\n[!] Serial error: {e}")
                break

    def periodic_worker(self):
        sec_count = 0
        while self.running:
            time.sleep(1.0)
            sec_count += 1
            try:
                self.send_telemetry()
                if sec_count % 2 == 0:
                    self.send_heartbeat()
            except Exception:
                pass

    def start(self):
        print("=" * 65)
        print(f" STM32 SIMULATOR & INTERACTIVE CLI ({self.port_name} @ {self.baudrate})")
        print(f" [*] Log Server da mo tai TCP localhost:{self.log_port}")
        print(f" [*] Mo Terminal 2 va chay de xem log ESP32:")
        print(f"     python scripts/esp_log_viewer.py")
        print("-" * 65)
        print(" Cac lenh tuong tac tren Terminal 1:")
        print("   t <val>   : Dat nhiet do (vd: t 31.2)")
        print("   h <val>   : Dat do am (vd: h 68)")
        print("   v <val>   : Dat dien ap pin (vd: v 4.18)")
        print("   normal    : Che do hoat dong binh thuong (Normal)")
        print("   timeout   : Gia lap Timeout (khong gui ACK)")
        print("   busy      : Gia lap tra NACK_BUSY")
        print("   crc       : Gia lap gui frame loi CRC")
        print("   hang      : Gia lap STM32 bi treo hoan toan")
        print("   log on/off: Bat/tat hien thi log ESP32 tai terminal nay")
        print("=" * 65)

        # Gui Handshake chao ESP32 khi vua mo cong
        self.send_handshake()

        t_rx = threading.Thread(target=self.rx_worker, daemon=True)
        t_tx = threading.Thread(target=self.periodic_worker, daemon=True)
        t_rx.start()
        t_tx.start()

        try:
            while self.running:
                cmd = input("STM32> ")
                cmd = cmd.strip()
                if not cmd:
                    continue
                parts = cmd.split()
                c = parts[0].lower()

                if c == "t" and len(parts) > 1:
                    self.temperature = float(parts[1])
                    print(f"[*] Set Nhiet do = {self.temperature} C")
                elif c == "h" and len(parts) > 1:
                    self.humidity = float(parts[1])
                    print(f"[*] Set Do am = {self.humidity} %")
                elif c == "v" and len(parts) > 1:
                    self.vbat = float(parts[1])
                    print(f"[*] Set Pin = {self.vbat} V")
                elif c == "normal":
                    self.scenario = "normal"
                    print("[*] Da chuyen sang: NORMAL (Chay tot)")
                elif c == "timeout":
                    self.scenario = "timeout"
                    print("[*] Da chuyen sang: TIMEOUT (Khong tra ACK)")
                elif c == "busy":
                    self.scenario = "busy"
                    print("[*] Da chuyen sang: NACK_BUSY")
                elif c == "crc":
                    self.scenario = "corrupt_crc"
                    print("[*] Da chuyen sang: CORRUPT CRC")
                elif c == "hang":
                    self.scenario = "hang"
                    print("[*] Da chuyen sang: STM32 HANG (Dung phat tin)")
                elif c == "log":
                    if len(parts) > 1 and parts[1].lower() == "on":
                        self.show_esp_log = True
                        print("[*] Da BAT hien thi log ESP32 tren Terminal 1.")
                    else:
                        self.show_esp_log = False
                        print("[*] Da TAT hien thi log ESP32 tren Terminal 1 (xem tai Terminal 2).")
                elif c == "exit" or c == "quit":
                    self.running = False
                    break
                else:
                    print(f"[?] Lenh khong hop le: {cmd}")
        except KeyboardInterrupt:
            self.running = False

        self.broadcaster.stop()
        self.ser.close()
        print("\n[*] Da dong cong Serial Simulator & Log Server.")

def main():
    parser = argparse.ArgumentParser(description="STM32 Simulator & Demuxer for ESP32-S3 Xiaozhi")
    parser.add_argument("--port", "-p", default="COM10", help="Cong Serial COM (vd: COM10)")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--show-logs", action="store_true", help="Hien thi ca log ESP32 tren Terminal 1")
    parser.add_argument("--log-port", type=int, default=9999, help="TCP Port phat log sang Terminal 2 (default: 9999)")
    args = parser.parse_args()

    sim = Stm32Simulator(args.port, args.baud, show_esp_log=args.show_logs, log_port=args.log_port)
    sim.start()

if __name__ == "__main__":
    main()
