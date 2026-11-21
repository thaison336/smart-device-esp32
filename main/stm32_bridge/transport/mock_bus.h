#ifndef MOCK_BUS_H
#define MOCK_BUS_H

#include "transport_bus.h"
#include "stream_framer.h"
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

class MockBus : public ITransportBus {
public:
    enum class MockScenario {
        kNormal,            // Phản hồi ACK ngay, phát telemetry & heartbeat định kỳ
        kDelayedAck,        // Trả ACK trễ 500ms
        kTimeout,           // Không trả ACK (giả lập timeout)
        kCorruptCrc,        // Gửi frame bị sai CRC
        kDuplicateAck,      // Gửi lặp 2 lần cùng 1 ACK sequence
        kNackBusy,          // Trả NACK BUSY
        kStm32Hang          // Dừng toàn bộ phản hồi và heartbeat (giả lập STM32 treo)
    };

    MockBus();
    ~MockBus() override;

    bool Initialize() override;
    void SetReceiveCallback(FrameReceiveCallback callback) override;
    bool SendFrame(const uint8_t* data, size_t length) override;

    // Điều khiển kịch bản test
    void SetScenario(MockScenario scenario);
    void TriggerTelemetry();
    void TriggerHeartbeat();
    void TriggerHandshake();
    void StartPeriodicWorker();
    void StopPeriodicWorker();

private:
    void HandleReceivedFrame(uint8_t frame_type, const uint8_t* payload, size_t length);
    void EmitFrame(uint8_t frame_type, const uint8_t* payload, size_t length, bool corrupt_crc = false);

    FrameReceiveCallback receive_callback_{nullptr};
    StreamFramer framer_;
    std::atomic<MockScenario> scenario_{MockScenario::kNormal};
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    std::mutex mutex_;
    uint32_t session_id_{1001};
    uint32_t uptime_sec_{0};
};

#endif // MOCK_BUS_H
