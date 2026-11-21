#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>

#include "stream_framer.h"
#include "datalogger.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"

// Test 1: Đóng gói và giải mã frame nguyên vẹn
void TestPackAndParseCompleteFrame() {
    std::cout << "[RUN] TestPackAndParseCompleteFrame..." << std::endl;
    StreamFramer framer;
    bool frame_received = false;
    uint8_t received_type = 0xFF;
    std::vector<uint8_t> received_payload;

    framer.SetFrameHandler([&](uint8_t type, const uint8_t* payload, size_t length) {
        frame_received = true;
        received_type = type;
        received_payload.assign(payload, payload + length);
    });

    uint8_t sample_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t frame_buf[StreamFramer::kMaxFrameSize];
    size_t packed_len = StreamFramer::PackFrame(0x10, sample_data, sizeof(sample_data), frame_buf, sizeof(frame_buf));
    assert(packed_len > 0);

    framer.ProcessBytes(frame_buf, packed_len);
    assert(frame_received == true);
    assert(received_type == 0x10);
    assert(received_payload.size() == sizeof(sample_data));
    assert(std::memcmp(received_payload.data(), sample_data, sizeof(sample_data)) == 0);
    std::cout << "[PASS] TestPackAndParseCompleteFrame" << std::endl;
}

// Test 2: Payload bị chia nhỏ truyền từng byte (Sliding byte stream)
void TestFragmentedBytes() {
    std::cout << "[RUN] TestFragmentedBytes..." << std::endl;
    StreamFramer framer;
    bool frame_received = false;

    framer.SetFrameHandler([&](uint8_t type, const uint8_t* payload, size_t length) {
        frame_received = true;
    });

    uint8_t sample_data[] = {0xAA, 0xBB, 0xCC};
    uint8_t frame_buf[StreamFramer::kMaxFrameSize];
    size_t packed_len = StreamFramer::PackFrame(0x01, sample_data, sizeof(sample_data), frame_buf, sizeof(frame_buf));

    // Đẩy từng byte
    for (size_t i = 0; i < packed_len; ++i) {
        assert(!frame_received || i == packed_len - 1);
        framer.ProcessByte(frame_buf[i]);
    }
    assert(frame_received == true);
    std::cout << "[PASS] TestFragmentedBytes" << std::endl;
}

// Test 3: Nhiều frame trong cùng một buffer stream
void TestMultipleFramesInBuffer() {
    std::cout << "[RUN] TestMultipleFramesInBuffer..." << std::endl;
    StreamFramer framer;
    int frame_count = 0;

    framer.SetFrameHandler([&](uint8_t type, const uint8_t* payload, size_t length) {
        frame_count++;
    });

    uint8_t d1[] = {1, 2};
    uint8_t d2[] = {3, 4, 5};
    uint8_t buffer[StreamFramer::kMaxFrameSize * 2];
    size_t len1 = StreamFramer::PackFrame(0x01, d1, sizeof(d1), buffer, sizeof(buffer));
    size_t len2 = StreamFramer::PackFrame(0x02, d2, sizeof(d2), buffer + len1, sizeof(buffer) - len1);

    framer.ProcessBytes(buffer, len1 + len2);
    assert(frame_count == 2);
    std::cout << "[PASS] TestMultipleFramesInBuffer" << std::endl;
}

// Test 4: Sync byte xuất hiện ngẫu nhiên trong payload (Data transparency)
void TestSyncByteInsidePayload() {
    std::cout << "[RUN] TestSyncByteInsidePayload..." << std::endl;
    StreamFramer framer;
    bool frame_received = false;

    framer.SetFrameHandler([&](uint8_t type, const uint8_t* payload, size_t length) {
        frame_received = true;
        assert(length == 4);
        assert(payload[0] == 0xAA && payload[1] == 0x55);
    });

    uint8_t tricky_payload[] = {0xAA, 0x55, 0xAA, 0x55};
    uint8_t frame_buf[StreamFramer::kMaxFrameSize];
    size_t packed_len = StreamFramer::PackFrame(0x05, tricky_payload, sizeof(tricky_payload), frame_buf, sizeof(frame_buf));

    framer.ProcessBytes(frame_buf, packed_len);
    assert(frame_received == true);
    std::cout << "[PASS] TestSyncByteInsidePayload" << std::endl;
}

// Test 5: CRC sai -> Parser reject và không phát callback
void TestCorruptedCrcRejected() {
    std::cout << "[RUN] TestCorruptedCrcRejected..." << std::endl;
    StreamFramer framer;
    bool frame_received = false;

    framer.SetFrameHandler([&](uint8_t type, const uint8_t* payload, size_t length) {
        frame_received = true;
    });

    uint8_t sample[] = {1, 2, 3};
    uint8_t frame_buf[StreamFramer::kMaxFrameSize];
    size_t packed_len = StreamFramer::PackFrame(0x01, sample, sizeof(sample), frame_buf, sizeof(frame_buf));

    // Làm sai CRC
    frame_buf[packed_len - 1] ^= 0x55;

    framer.ProcessBytes(frame_buf, packed_len);
    assert(frame_received == false);
    std::cout << "[PASS] TestCorruptedCrcRejected" << std::endl;
}

// Test 6: Duplicate sync bytes ở đầu frame (0xAA 0xAA 0x55)
void TestDuplicateLeadingSync() {
    std::cout << "[RUN] TestDuplicateLeadingSync..." << std::endl;
    StreamFramer framer;
    bool frame_received = false;

    framer.SetFrameHandler([&](uint8_t type, const uint8_t* payload, size_t length) {
        frame_received = true;
    });

    uint8_t sample[] = {10, 20};
    uint8_t frame_buf[StreamFramer::kMaxFrameSize];
    size_t packed_len = StreamFramer::PackFrame(0x01, sample, sizeof(sample), frame_buf, sizeof(frame_buf));

    // Chèn thêm một byte 0xAA rác ở đầu
    uint8_t noisy_buf[StreamFramer::kMaxFrameSize + 1];
    noisy_buf[0] = 0xAA;
    std::memcpy(&noisy_buf[1], frame_buf, packed_len);

    framer.ProcessBytes(noisy_buf, packed_len + 1);
    assert(frame_received == true);
    std::cout << "[PASS] TestDuplicateLeadingSync" << std::endl;
}

// Test 7: Nanopb Round-trip Telemetry
void TestNanopbTelemetryRoundTrip() {
    std::cout << "[RUN] TestNanopbTelemetryRoundTrip..." << std::endl;

    datalogger_BridgeEnvelope tx_env = datalogger_BridgeEnvelope_init_zero;
    tx_env.protocol_version = 1;
    tx_env.session_id = 999;
    tx_env.msg_type = datalogger_MessageType_MSG_TYPE_TELEMETRY;
    tx_env.sequence_id = 42;
    strncpy(tx_env.source_id, "STM32F4", sizeof(tx_env.source_id) - 1);

    tx_env.which_payload = datalogger_BridgeEnvelope_telemetry_tag;
    tx_env.payload.telemetry.temperature = 25.4f;
    tx_env.payload.telemetry.humidity = 60.5f;
    tx_env.payload.telemetry.pressure = 1012.8f;
    tx_env.payload.telemetry.battery_voltage = 4.12f;
    tx_env.payload.telemetry.current_mA = 120.0f;
    tx_env.payload.telemetry.digital_inputs = 0x05;
    tx_env.payload.telemetry.analog_channels_count = 2;
    tx_env.payload.telemetry.analog_channels[0] = 1.1f;
    tx_env.payload.telemetry.analog_channels[1] = 2.2f;
    strncpy(tx_env.payload.telemetry.error_message, "OK", sizeof(tx_env.payload.telemetry.error_message) - 1);

    uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
    pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    bool enc_ok = pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &tx_env);
    assert(enc_ok == true);
    assert(ostream.bytes_written > 0);

    // Decode
    datalogger_BridgeEnvelope rx_env = datalogger_BridgeEnvelope_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(enc_buf, ostream.bytes_written);
    bool dec_ok = pb_decode(&istream, datalogger_BridgeEnvelope_fields, &rx_env);
    assert(dec_ok == true);
    assert(rx_env.sequence_id == 42);
    assert(rx_env.msg_type == datalogger_MessageType_MSG_TYPE_TELEMETRY);
    assert(rx_env.which_payload == datalogger_BridgeEnvelope_telemetry_tag);
    assert(rx_env.payload.telemetry.temperature > 25.3f && rx_env.payload.telemetry.temperature < 25.5f);
    assert(rx_env.payload.telemetry.analog_channels_count == 2);
    assert(std::strcmp(rx_env.payload.telemetry.error_message, "OK") == 0);
    std::cout << "[PASS] TestNanopbTelemetryRoundTrip" << std::endl;
}

int main() {
    std::cout << "==========================================" << std::endl;
    std::cout << "Running STM32 Bridge Host Unit Tests (g++)" << std::endl;
    std::cout << "==========================================" << std::endl;

    TestPackAndParseCompleteFrame();
    TestFragmentedBytes();
    TestMultipleFramesInBuffer();
    TestSyncByteInsidePayload();
    TestCorruptedCrcRejected();
    TestDuplicateLeadingSync();
    TestNanopbTelemetryRoundTrip();

    std::cout << "==========================================" << std::endl;
    std::cout << "ALL 7 HOST UNIT TESTS PASSED SUCCESSFULLY!" << std::endl;
    std::cout << "==========================================" << std::endl;
    return 0;
}
