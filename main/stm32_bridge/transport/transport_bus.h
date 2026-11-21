#ifndef TRANSPORT_BUS_H
#define TRANSPORT_BUS_H

#include <cstdint>
#include <cstddef>
#include <functional>

class ITransportBus {
public:
    using FrameReceiveCallback = std::function<void(const uint8_t* data, size_t length)>;

    virtual ~ITransportBus() = default;

    // Khởi tạo bus phần cứng hoặc mock
    virtual bool Initialize() = 0;

    // Đăng ký callback khi nhận được một frame hợp lệ từ bus
    virtual void SetReceiveCallback(FrameReceiveCallback callback) = 0;

    // Gửi raw frame bytes qua bus
    virtual bool SendFrame(const uint8_t* data, size_t length) = 0;
};

#endif // TRANSPORT_BUS_H
