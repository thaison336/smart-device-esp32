#include "stream_framer.h"
#include <cstring>

StreamFramer::StreamFramer() {
    Reset();
}

void StreamFramer::SetFrameHandler(FrameHandler handler) {
    frame_handler_ = handler;
}

void StreamFramer::Reset() {
    state_ = ParseState::kWaitSync0;
    current_version_ = 0;
    current_type_ = 0;
    expected_length_ = 0;
    payload_index_ = 0;
    received_crc_ = 0;
}

uint16_t StreamFramer::ComputeCrc16(const uint8_t* data, size_t length) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (static_cast<uint16_t>(data[i]) << 8);
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

size_t StreamFramer::PackFrame(uint8_t frame_type, const uint8_t* payload, size_t length, uint8_t* out_buffer, size_t out_max_len) {
    if (length > kMaxPayloadSize || out_max_len < (kHeaderSize + length + kCrcSize)) {
        return 0;
    }

    out_buffer[0] = kSync0;
    out_buffer[1] = kSync1;
    out_buffer[2] = kProtocolVersion;
    out_buffer[3] = frame_type;
    out_buffer[4] = static_cast<uint8_t>((length >> 8) & 0xFF);
    out_buffer[5] = static_cast<uint8_t>(length & 0xFF);

    if (length > 0 && payload != nullptr) {
        std::memcpy(&out_buffer[6], payload, length);
    }

    // CRC tính từ VERSION đến hết PAYLOAD (bỏ 2 byte sync)
    uint16_t crc = ComputeCrc16(&out_buffer[2], 4 + length);
    size_t crc_offset = kHeaderSize + length;
    out_buffer[crc_offset] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    out_buffer[crc_offset + 1] = static_cast<uint8_t>(crc & 0xFF);

    return crc_offset + kCrcSize;
}

void StreamFramer::ProcessByte(uint8_t byte) {
    switch (state_) {
        case ParseState::kWaitSync0:
            if (byte == kSync0) {
                state_ = ParseState::kWaitSync1;
            }
            break;

        case ParseState::kWaitSync1:
            if (byte == kSync1) {
                state_ = ParseState::kWaitVersion;
            } else if (byte == kSync0) {
                // Giữ lại sync0 nếu gặp double sync0 (ví dụ 0xAA 0xAA 0x55)
                state_ = ParseState::kWaitSync1;
            } else {
                state_ = ParseState::kWaitSync0;
            }
            break;

        case ParseState::kWaitVersion:
            if (byte == kProtocolVersion) {
                current_version_ = byte;
                state_ = ParseState::kWaitType;
            } else {
                state_ = ParseState::kWaitSync0;
            }
            break;

        case ParseState::kWaitType:
            current_type_ = byte;
            state_ = ParseState::kWaitLengthH;
            break;

        case ParseState::kWaitLengthH:
            expected_length_ = (static_cast<uint16_t>(byte) << 8);
            state_ = ParseState::kWaitLengthL;
            break;

        case ParseState::kWaitLengthL:
            expected_length_ |= byte;
            if (expected_length_ > kMaxPayloadSize) {
                // Length quá lớn -> reject và reset parser
                Reset();
            } else if (expected_length_ == 0) {
                state_ = ParseState::kWaitCrcH;
            } else {
                payload_index_ = 0;
                state_ = ParseState::kWaitPayload;
            }
            break;

        case ParseState::kWaitPayload:
            payload_buffer_[payload_index_++] = byte;
            if (payload_index_ >= expected_length_) {
                state_ = ParseState::kWaitCrcH;
            }
            break;

        case ParseState::kWaitCrcH:
            received_crc_ = (static_cast<uint16_t>(byte) << 8);
            state_ = ParseState::kWaitCrcL;
            break;

        case ParseState::kWaitCrcL: {
            received_crc_ |= byte;

            // Tính CRC trên VERSION + TYPE + LEN + PAYLOAD
            uint8_t header_buf[4];
            header_buf[0] = current_version_;
            header_buf[1] = current_type_;
            header_buf[2] = static_cast<uint8_t>((expected_length_ >> 8) & 0xFF);
            header_buf[3] = static_cast<uint8_t>(expected_length_ & 0xFF);

            // Compute running CRC: header rồi payload
            uint16_t computed_crc = 0xFFFF;
            for (size_t i = 0; i < 4; ++i) {
                computed_crc ^= (static_cast<uint16_t>(header_buf[i]) << 8);
                for (int j = 0; j < 8; ++j) {
                    if (computed_crc & 0x8000) {
                        computed_crc = (computed_crc << 1) ^ 0x1021;
                    } else {
                        computed_crc <<= 1;
                    }
                }
            }
            for (size_t i = 0; i < expected_length_; ++i) {
                computed_crc ^= (static_cast<uint16_t>(payload_buffer_[i]) << 8);
                for (int j = 0; j < 8; ++j) {
                    if (computed_crc & 0x8000) {
                        computed_crc = (computed_crc << 1) ^ 0x1021;
                    } else {
                        computed_crc <<= 1;
                    }
                }
            }

            if (computed_crc == received_crc_) {
                if (frame_handler_) {
                    frame_handler_(current_type_, payload_buffer_, expected_length_);
                }
            }
            Reset();
            break;
        }
    }
}

void StreamFramer::ProcessBytes(const uint8_t* data, size_t length) {
    if (data == nullptr) return;
    for (size_t i = 0; i < length; ++i) {
        ProcessByte(data[i]);
    }
}
