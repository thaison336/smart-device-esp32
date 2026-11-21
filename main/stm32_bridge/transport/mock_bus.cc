#include "mock_bus.h"
#include "datalogger.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include <chrono>
#include <cstring>
#include <esp_log.h>

static const char* TAG = "MockBus";

MockBus::MockBus() {
    framer_.SetFrameHandler([this](uint8_t frame_type, const uint8_t* payload, size_t length) {
        HandleReceivedFrame(frame_type, payload, length);
    });
}

MockBus::~MockBus() {
    StopPeriodicWorker();
}

bool MockBus::Initialize() {
    ESP_LOGI(TAG, "MockBus initialized");
    return true;
}

void MockBus::SetReceiveCallback(FrameReceiveCallback callback) {
    receive_callback_ = callback;
}

void MockBus::SetScenario(MockScenario scenario) {
    scenario_.store(scenario);
    ESP_LOGI(TAG, "Mock scenario updated to: %d", static_cast<int>(scenario));
}

bool MockBus::SendFrame(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0) return false;
    // Đưa byte stream vào Framer của MockBus (giống hệt bên STM32 nhận SPI/UART DMA)
    framer_.ProcessBytes(data, length);
    return true;
}

void MockBus::EmitFrame(uint8_t frame_type, const uint8_t* payload, size_t length, bool corrupt_crc) {
    uint8_t frame_buffer[StreamFramer::kMaxFrameSize];
    size_t packed_len = StreamFramer::PackFrame(frame_type, payload, length, frame_buffer, sizeof(frame_buffer));
    if (packed_len == 0) return;

    if (corrupt_crc) {
        // Cố tình làm sai byte CRC cuối
        frame_buffer[packed_len - 1] ^= 0xFF;
    }

    if (receive_callback_) {
        receive_callback_(frame_buffer, packed_len);
    }
}

void MockBus::HandleReceivedFrame(uint8_t frame_type, const uint8_t* payload, size_t length) {
    if (scenario_.load() == MockScenario::kStm32Hang) {
        // STM32 treo, không xử lý gì
        return;
    }

    // Decode Envelope để lấy sequence_id và payload
    datalogger_BridgeEnvelope rx_envelope = datalogger_BridgeEnvelope_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, length);
    if (!pb_decode(&stream, datalogger_BridgeEnvelope_fields, &rx_envelope)) {
        ESP_LOGE(TAG, "MockBus failed to decode received BridgeEnvelope");
        return;
    }

    uint32_t rx_seq = rx_envelope.sequence_id;

    if (rx_envelope.msg_type == datalogger_MessageType_MSG_TYPE_COMMAND) {
        auto current_scen = scenario_.load();
        if (current_scen == MockScenario::kTimeout) {
            // Không gửi ACK để ESP32 timeout
            return;
        }

        auto send_ack_lambda = [this, rx_seq, current_scen]() {
            datalogger_BridgeEnvelope ack_env = datalogger_BridgeEnvelope_init_zero;
            ack_env.protocol_version = 1;
            ack_env.session_id = session_id_;
            ack_env.msg_type = datalogger_MessageType_MSG_TYPE_ACK;
            ack_env.sequence_id = rx_seq;
            ack_env.timestamp_ms = 0;
            strncpy(ack_env.source_id, "STM32F4", sizeof(ack_env.source_id) - 1);

            ack_env.which_payload = datalogger_BridgeEnvelope_ack_tag;
            ack_env.payload.ack.command_seq = rx_seq;
            if (current_scen == MockScenario::kNackBusy) {
                ack_env.payload.ack.status = datalogger_AckStatus_ACK_STATUS_BUSY;
                ack_env.payload.ack.result_code = -1;
                strncpy(ack_env.payload.ack.result_msg, "STM32 Busy", sizeof(ack_env.payload.ack.result_msg) - 1);
            } else {
                ack_env.payload.ack.status = datalogger_AckStatus_ACK_STATUS_APPLIED;
                ack_env.payload.ack.result_code = 0;
                strncpy(ack_env.payload.ack.result_msg, "SUCCESS", sizeof(ack_env.payload.ack.result_msg) - 1);
            }

            uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
            pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
            if (pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &ack_env)) {
                bool corrupt = (current_scen == MockScenario::kCorruptCrc);
                EmitFrame(0x00, enc_buf, ostream.bytes_written, corrupt);
                if (current_scen == MockScenario::kDuplicateAck) {
                    // Phát lặp lại cùng sequence ACK
                    EmitFrame(0x00, enc_buf, ostream.bytes_written, false);
                }
            }
        };

        if (current_scen == MockScenario::kDelayedAck) {
            std::thread([send_ack_lambda]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                send_ack_lambda();
            }).detach();
        } else {
            send_ack_lambda();
        }
    } else if (rx_envelope.msg_type == datalogger_MessageType_MSG_TYPE_HANDSHAKE) {
        // Phản hồi Handshake từ STM32
        TriggerHandshake();
    }
}

void MockBus::TriggerHandshake() {
    datalogger_BridgeEnvelope env = datalogger_BridgeEnvelope_init_zero;
    env.protocol_version = 1;
    env.session_id = session_id_;
    env.msg_type = datalogger_MessageType_MSG_TYPE_HANDSHAKE;
    env.sequence_id = 1;
    strncpy(env.source_id, "STM32F4", sizeof(env.source_id) - 1);

    env.which_payload = datalogger_BridgeEnvelope_handshake_tag;
    env.payload.handshake.protocol_version = 1;
    env.payload.handshake.hardware_version = 2;
    env.payload.handshake.firmware_version = 100;
    strncpy(env.payload.handshake.device_serial, "STM32-MOCK-001", sizeof(env.payload.handshake.device_serial) - 1);

    uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
    pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &env)) {
        EmitFrame(0x00, enc_buf, ostream.bytes_written, false);
    }
}

void MockBus::TriggerHeartbeat() {
    if (scenario_.load() == MockScenario::kStm32Hang) return;

    datalogger_BridgeEnvelope env = datalogger_BridgeEnvelope_init_zero;
    env.protocol_version = 1;
    env.session_id = session_id_;
    env.msg_type = datalogger_MessageType_MSG_TYPE_HEARTBEAT;
    env.sequence_id = 0;
    strncpy(env.source_id, "STM32F4", sizeof(env.source_id) - 1);

    env.which_payload = datalogger_BridgeEnvelope_heartbeat_tag;
    env.payload.heartbeat.uptime_sec = ++uptime_sec_;
    env.payload.heartbeat.free_heap = 48000;
    env.payload.heartbeat.status_flags = 0x01;

    uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
    pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &env)) {
        EmitFrame(0x00, enc_buf, ostream.bytes_written, false);
    }
}

void MockBus::TriggerTelemetry() {
    if (scenario_.load() == MockScenario::kStm32Hang) return;

    datalogger_BridgeEnvelope env = datalogger_BridgeEnvelope_init_zero;
    env.protocol_version = 1;
    env.session_id = session_id_;
    env.msg_type = datalogger_MessageType_MSG_TYPE_TELEMETRY;
    env.sequence_id = 0;
    strncpy(env.source_id, "STM32F4", sizeof(env.source_id) - 1);

    env.which_payload = datalogger_BridgeEnvelope_telemetry_tag;
    env.payload.telemetry.temperature = 28.5f;
    env.payload.telemetry.humidity = 65.2f;
    env.payload.telemetry.pressure = 1013.25f;
    env.payload.telemetry.battery_voltage = 3.95f;
    env.payload.telemetry.current_mA = 145.0f;
    env.payload.telemetry.digital_inputs = 0x03;
    env.payload.telemetry.analog_channels_count = 2;
    env.payload.telemetry.analog_channels[0] = 1.25f;
    env.payload.telemetry.analog_channels[1] = 2.50f;
    strncpy(env.payload.telemetry.error_message, "NONE", sizeof(env.payload.telemetry.error_message) - 1);

    uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
    pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &env)) {
        EmitFrame(0x00, enc_buf, ostream.bytes_written, false);
    }
}

void MockBus::StartPeriodicWorker() {
    if (running_.load()) return;
    running_.store(true);
    worker_thread_ = std::thread([this]() {
        int count = 0;
        while (running_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (!running_.load()) break;
            count++;
            if (count % 2 == 0) {
                // Mỗi 1 giây phát 1 telemetry
                TriggerTelemetry();
            }
            if (count % 4 == 0) {
                // Mỗi 2 giây phát 1 heartbeat
                TriggerHeartbeat();
            }
        }
    });
}

void MockBus::StopPeriodicWorker() {
    if (running_.load()) {
        running_.store(false);
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
    }
}
