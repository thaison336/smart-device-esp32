#ifndef STREAM_FRAMER_H
#define STREAM_FRAMER_H

#include <cstdint>
#include <cstddef>
#include <functional>

// Internal frame contract:
// [SYNC0:0xAA] [SYNC1:0x55] [VERSION:0x01] [FRAME_TYPE:0x00] [LEN_HIGH:uint8] [LEN_LOW:uint8] [PAYLOAD:N] [CRC_HIGH:uint8] [CRC_LOW:uint8]

class StreamFramer {
public:
    static constexpr uint8_t kSync0 = 0xAA;
    static constexpr uint8_t kSync1 = 0x55;
    static constexpr uint8_t kProtocolVersion = 0x01;
    static constexpr size_t kHeaderSize = 6;      // SYNC0, SYNC1, VER, TYPE, LEN_H, LEN_L
    static constexpr size_t kCrcSize = 2;         // CRC16 (High, Low)
    static constexpr size_t kMaxPayloadSize = 256;
    static constexpr size_t kMaxFrameSize = kHeaderSize + kMaxPayloadSize + kCrcSize;

    enum class ParseState {
        kWaitSync0,
        kWaitSync1,
        kWaitVersion,
        kWaitType,
        kWaitLengthH,
        kWaitLengthL,
        kWaitPayload,
        kWaitCrcH,
        kWaitCrcL
    };

    using FrameHandler = std::function<void(uint8_t frame_type, const uint8_t* payload, size_t length)>;

    StreamFramer();

    void SetFrameHandler(FrameHandler handler);

    // Xử lý stream byte trôi qua sliding parser state machine
    void ProcessByte(uint8_t byte);
    void ProcessBytes(const uint8_t* data, size_t length);

    // Đóng gói frame từ payload
    static size_t PackFrame(uint8_t frame_type, const uint8_t* payload, size_t length, uint8_t* out_buffer, size_t out_max_len);

    // Tính CRC16 CCITT (0x1021, init 0xFFFF)
    static uint16_t ComputeCrc16(const uint8_t* data, size_t length);

    void Reset();

private:
    ParseState state_{ParseState::kWaitSync0};
    uint8_t current_version_{0};
    uint8_t current_type_{0};
    uint16_t expected_length_{0};
    size_t payload_index_{0};
    uint8_t payload_buffer_[kMaxPayloadSize];
    uint16_t received_crc_{0};

    FrameHandler frame_handler_{nullptr};
};

#endif // STREAM_FRAMER_H
