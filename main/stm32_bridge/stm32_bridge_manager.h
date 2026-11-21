#ifndef STM32_BRIDGE_MANAGER_H
#define STM32_BRIDGE_MANAGER_H

#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include <future>
#include <map>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "transport/transport_bus.h"
#include "transport/stream_framer.h"
#include "datalogger.pb.h"
#include "device_state.h"

enum class Stm32LinkState {
    kBoot,
    kHandshake,
    kSyncing,
    kOnlineIdle,
    kCommandExec,
    kDegraded,
    kStm32Recovery
};

enum class CommandResultStatus {
    kQueued,
    kSent,
    kAckOk,
    kAckApplied,
    kNackBusy,
    kNackInvalid,
    kErrTimeout,
    kErrInternal,
    kUnknownAfterTimeout
};

struct CommandResponse {
    uint32_t sequence_id{0};
    CommandResultStatus status{CommandResultStatus::kErrInternal};
    int32_t result_code{0};
    std::string result_msg;
};

class Stm32BridgeManager {
public:
    static Stm32BridgeManager& GetInstance();

    // Khởi động Bridge Manager với transport chỉ định (MockBus hoặc Hardware Bus)
    bool Start(std::shared_ptr<ITransportBus> transport);
    void Stop();

    // Trạng thái Link hiện tại
    Stm32LinkState GetLinkState() const;

    // Lấy bản snapshot Telemetry mới nhất (Thread-safe)
    datalogger_SensorTelemetry GetLatestTelemetry();

    // Lấy Heartbeat mới nhất
    datalogger_Heartbeat GetLatestHeartbeat();

    // Gửi lệnh đồng bộ (Synchronous blocking với timeout ms)
    CommandResponse ExecuteCommandSync(const std::string& action, int32_t param_int, float param_float, const std::string& param_str, uint32_t timeout_ms = 1500);

    // Xử lý frame nhận được từ Transport callback
    void OnRawBytesReceived(const uint8_t* data, size_t length);

    // Cho phép đổi scenario mock khi cần test
    std::shared_ptr<ITransportBus> GetTransport() const { return transport_; }

private:
    Stm32BridgeManager();
    ~Stm32BridgeManager();
    Stm32BridgeManager(const Stm32BridgeManager&) = delete;
    Stm32BridgeManager& operator=(const Stm32BridgeManager&) = delete;

    void BridgeTask();
    static void BridgeTaskEntry(void* arg);

    void HandleParsedFrame(uint8_t frame_type, const uint8_t* payload, size_t length);
    void ProcessHandshake(const datalogger_Handshake& hs);
    void ProcessHeartbeat(const datalogger_Heartbeat& hb);
    void ProcessTelemetry(const datalogger_SensorTelemetry& telem);
    void ProcessAck(const datalogger_CommandAck& ack);

    void SetLinkState(Stm32LinkState new_state);
    void CheckTimeoutsAndWatchdog();

    std::shared_ptr<ITransportBus> transport_{nullptr};
    StreamFramer rx_framer_;

    TaskHandle_t task_handle_{nullptr};
    QueueHandle_t rx_byte_queue_{nullptr};
    bool running_{false};

    mutable std::mutex data_mutex_;
    Stm32LinkState link_state_{Stm32LinkState::kBoot};
    datalogger_SensorTelemetry latest_telemetry_{};
    datalogger_Heartbeat latest_heartbeat_{};
    uint32_t last_heartbeat_time_ms_{0};

    uint32_t next_sequence_id_{1};

    struct PendingCommand {
        uint32_t sequence_id;
        std::promise<CommandResponse> promise;
        uint32_t sent_time_ms;
        uint32_t timeout_ms;
    };

    std::mutex pending_mutex_;
    std::map<uint32_t, std::shared_ptr<PendingCommand>> pending_commands_;
};

#endif // STM32_BRIDGE_MANAGER_H
