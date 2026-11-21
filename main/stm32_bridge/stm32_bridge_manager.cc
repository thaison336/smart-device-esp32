#include "stm32_bridge_manager.h"
#include "stm32_dashboard_ui.h"
#include "datalogger.pb.h"
#include "pb_encode.h"
#include "pb_decode.h"
#include "application.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>

static const char* TAG = "Stm32Bridge";

static uint32_t GetCurrentTimeMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

Stm32BridgeManager& Stm32BridgeManager::GetInstance() {
    static Stm32BridgeManager instance;
    return instance;
}

Stm32BridgeManager::Stm32BridgeManager() {
    std::memset(&latest_telemetry_, 0, sizeof(latest_telemetry_));
    std::memset(&latest_heartbeat_, 0, sizeof(latest_heartbeat_));

    rx_framer_.SetFrameHandler([this](uint8_t frame_type, const uint8_t* payload, size_t length) {
        HandleParsedFrame(frame_type, payload, length);
    });
}

Stm32BridgeManager::~Stm32BridgeManager() {
    Stop();
}

bool Stm32BridgeManager::Start(std::shared_ptr<ITransportBus> transport) {
    if (running_) {
        ESP_LOGW(TAG, "Bridge already running");
        return true;
    }

    transport_ = transport;
    if (!transport_ || !transport_->Initialize()) {
        ESP_LOGE(TAG, "Failed to initialize transport");
        return false;
    }

    // Tạo Queue nhận bytes từ transport callback (kích thước 1024 bytes)
    rx_byte_queue_ = xQueueCreate(1024, sizeof(uint8_t));
    if (!rx_byte_queue_) {
        ESP_LOGE(TAG, "Failed to create rx_byte_queue");
        return false;
    }

    transport_->SetReceiveCallback([this](const uint8_t* data, size_t length) {
        OnRawBytesReceived(data, length);
    });

    running_ = true;
    SetLinkState(Stm32LinkState::kBoot);

    BaseType_t ret = xTaskCreatePinnedToCore(
        BridgeTaskEntry,
        "stm32_bridge",
        4096,
        this,
        5,
        &task_handle_,
        1 // Core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stm32_bridge task");
        running_ = false;
        vQueueDelete(rx_byte_queue_);
        rx_byte_queue_ = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "STM32 Bridge Manager started successfully");
    return true;
}

void Stm32BridgeManager::Stop() {
    if (!running_) return;
    running_ = false;

    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }

    if (rx_byte_queue_ != nullptr) {
        vQueueDelete(rx_byte_queue_);
        rx_byte_queue_ = nullptr;
    }

    ESP_LOGI(TAG, "STM32 Bridge Manager stopped");
}

void Stm32BridgeManager::OnRawBytesReceived(const uint8_t* data, size_t length) {
    if (!rx_byte_queue_ || data == nullptr || length == 0) return;
    for (size_t i = 0; i < length; ++i) {
        // Enqueue từng byte vào queue mà không block ISR hay gọi heap
        xQueueSend(rx_byte_queue_, &data[i], 0);
    }
}

void Stm32BridgeManager::BridgeTaskEntry(void* arg) {
    static_cast<Stm32BridgeManager*>(arg)->BridgeTask();
}

void Stm32BridgeManager::BridgeTask() {
    uint8_t byte = 0;
    last_heartbeat_time_ms_ = GetCurrentTimeMs();

    // Gửi Handshake probe đầu tiên
    SetLinkState(Stm32LinkState::kHandshake);

    datalogger_BridgeEnvelope hs_env = datalogger_BridgeEnvelope_init_zero;
    hs_env.protocol_version = 1;
    hs_env.msg_type = datalogger_MessageType_MSG_TYPE_HANDSHAKE;
    hs_env.sequence_id = next_sequence_id_++;
    strncpy(hs_env.source_id, "ESP32S3", sizeof(hs_env.source_id) - 1);
    hs_env.which_payload = datalogger_BridgeEnvelope_handshake_tag;
    hs_env.payload.handshake.protocol_version = 1;

    uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
    pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &hs_env)) {
        uint8_t frame_buf[StreamFramer::kMaxFrameSize];
        size_t frame_len = StreamFramer::PackFrame(0x00, enc_buf, ostream.bytes_written, frame_buf, sizeof(frame_buf));
        if (frame_len > 0 && transport_) {
            transport_->SendFrame(frame_buf, frame_len);
        }
    }

    while (running_) {
        // Chờ nhận byte tối đa 50ms để kiểm tra timeout & watchdog
        if (xQueueReceive(rx_byte_queue_, &byte, pdMS_TO_TICKS(50)) == pdPASS) {
            rx_framer_.ProcessByte(byte);
        }

        CheckTimeoutsAndWatchdog();
    }
}

void Stm32BridgeManager::CheckTimeoutsAndWatchdog() {
    uint32_t now = GetCurrentTimeMs();

    // 1. Kiểm tra Heartbeat Watchdog (quá 5000ms không có heartbeat/telemetry -> Degraded/Recovery)
    if (link_state_ == Stm32LinkState::kOnlineIdle || link_state_ == Stm32LinkState::kCommandExec) {
        if (now - last_heartbeat_time_ms_ > 5000) {
            ESP_LOGW(TAG, "Heartbeat watchdog timeout! Switching to DEGRADED");
            SetLinkState(Stm32LinkState::kDegraded);
        }
    } else if (link_state_ == Stm32LinkState::kDegraded) {
        if (now - last_heartbeat_time_ms_ > 10000) {
            ESP_LOGE(TAG, "Heartbeat lost prolonged! Switching to STM32_RECOVERY");
            SetLinkState(Stm32LinkState::kStm32Recovery);
        }
    }

    // 2. Kiểm tra Command Timeout cho các lệnh đang chờ
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto it = pending_commands_.begin(); it != pending_commands_.end();) {
        auto& cmd = it->second;
        if (now - cmd->sent_time_ms > cmd->timeout_ms) {
            ESP_LOGW(TAG, "Command seq %lu timed out", static_cast<unsigned long>(cmd->sequence_id));
            CommandResponse resp;
            resp.sequence_id = cmd->sequence_id;
            resp.status = CommandResultStatus::kErrTimeout;
            resp.result_code = -1;
            resp.result_msg = "TIMEOUT";
            cmd->promise.set_value(resp);
            it = pending_commands_.erase(it);
        } else {
            ++it;
        }
    }
}

void Stm32BridgeManager::HandleParsedFrame(uint8_t frame_type, const uint8_t* payload, size_t length) {
    datalogger_BridgeEnvelope env = datalogger_BridgeEnvelope_init_zero;
    pb_istream_t istream = pb_istream_from_buffer(payload, length);
    if (!pb_decode(&istream, datalogger_BridgeEnvelope_fields, &env)) {
        ESP_LOGE(TAG, "Failed to decode BridgeEnvelope in bridge manager");
        return;
    }

    // Cập nhật timestamp nhận bản tin thành công từ STM32
    last_heartbeat_time_ms_ = GetCurrentTimeMs();

    // Khôi phục lại OnlineIdle nếu trước đó bị Degraded/Recovery
    if (link_state_ == Stm32LinkState::kDegraded || link_state_ == Stm32LinkState::kStm32Recovery) {
        SetLinkState(Stm32LinkState::kOnlineIdle);
    }

    switch (env.msg_type) {
        case datalogger_MessageType_MSG_TYPE_HANDSHAKE:
            if (env.which_payload == datalogger_BridgeEnvelope_handshake_tag) {
                ProcessHandshake(env.payload.handshake);
            }
            break;

        case datalogger_MessageType_MSG_TYPE_HEARTBEAT:
            if (env.which_payload == datalogger_BridgeEnvelope_heartbeat_tag) {
                ProcessHeartbeat(env.payload.heartbeat);
            }
            break;

        case datalogger_MessageType_MSG_TYPE_TELEMETRY:
            if (env.which_payload == datalogger_BridgeEnvelope_telemetry_tag) {
                ProcessTelemetry(env.payload.telemetry);
            }
            break;

        case datalogger_MessageType_MSG_TYPE_ACK:
            if (env.which_payload == datalogger_BridgeEnvelope_ack_tag) {
                ProcessAck(env.payload.ack);
            }
            break;

        default:
            break;
    }
}

void Stm32BridgeManager::ProcessHandshake(const datalogger_Handshake& hs) {
    ESP_LOGI(TAG, "Handshake received: DevSerial=%s, FW=%lu", hs.device_serial, static_cast<unsigned long>(hs.firmware_version));
    SetLinkState(Stm32LinkState::kOnlineIdle);
}

void Stm32BridgeManager::ProcessHeartbeat(const datalogger_Heartbeat& hb) {
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        latest_heartbeat_ = hb;
    }
    ESP_LOGD(TAG, "Heartbeat: Uptime=%lu, Heap=%lu", static_cast<unsigned long>(hb.uptime_sec), static_cast<unsigned long>(hb.free_heap));
}

void Stm32BridgeManager::ProcessTelemetry(const datalogger_SensorTelemetry& telem) {
    {
        std::lock_guard<std::mutex> lock(data_mutex_);
        latest_telemetry_ = telem;
    }

    ESP_LOGI(TAG, "Telemetry received: Temp=%.2f, Hum=%.2f, Press=%.2f, Vbat=%.2fV",
             telem.temperature, telem.humidity, telem.pressure, telem.battery_voltage);

    // Schedule notification sang Application main context và cập nhật UI
    Application::GetInstance().Schedule([telem]() {
        ESP_LOGD(TAG, "Notified Application of new telemetry (T=%.1f)", telem.temperature);
        Stm32DashboardUI::UpdateTelemetryUI();
    });
}

void Stm32BridgeManager::ProcessAck(const datalogger_CommandAck& ack) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_commands_.find(ack.command_seq);
    if (it == pending_commands_.end()) {
        ESP_LOGW(TAG, "Received ACK for unknown/stale/duplicate sequence: %lu", static_cast<unsigned long>(ack.command_seq));
        return;
    }

    CommandResponse resp;
    resp.sequence_id = ack.command_seq;
    resp.result_code = ack.result_code;
    resp.result_msg = ack.result_msg;

    switch (ack.status) {
        case datalogger_AckStatus_ACK_STATUS_OK:
            resp.status = CommandResultStatus::kAckOk;
            break;
        case datalogger_AckStatus_ACK_STATUS_APPLIED:
            resp.status = CommandResultStatus::kAckApplied;
            break;
        case datalogger_AckStatus_ACK_STATUS_BUSY:
            resp.status = CommandResultStatus::kNackBusy;
            break;
        case datalogger_AckStatus_ACK_STATUS_INVALID:
            resp.status = CommandResultStatus::kNackInvalid;
            break;
        case datalogger_AckStatus_ACK_STATUS_TIMEOUT:
            resp.status = CommandResultStatus::kErrTimeout;
            break;
        default:
            resp.status = CommandResultStatus::kErrInternal;
            break;
    }

    it->second->promise.set_value(resp);
    pending_commands_.erase(it);

    if (link_state_ == Stm32LinkState::kCommandExec) {
        SetLinkState(Stm32LinkState::kOnlineIdle);
    }
}

void Stm32BridgeManager::SetLinkState(Stm32LinkState new_state) {
    if (link_state_ == new_state) return;
    link_state_ = new_state;
    ESP_LOGI(TAG, "STM32 Link State changed to: %d", static_cast<int>(new_state));

    Application::GetInstance().Schedule([new_state]() {
        if (new_state == Stm32LinkState::kOnlineIdle) {
            Application::GetInstance().SetDeviceState(kDeviceStateOnlineIdle);
        } else if (new_state == Stm32LinkState::kCommandExec) {
            Application::GetInstance().SetDeviceState(kDeviceStateCommandExec);
        } else if (new_state == Stm32LinkState::kStm32Recovery) {
            Application::GetInstance().SetDeviceState(kDeviceStateStm32Recovery);
        }
        Stm32DashboardUI::UpdateLinkStatusUI();
    });
}

Stm32LinkState Stm32BridgeManager::GetLinkState() const {
    return link_state_;
}

datalogger_SensorTelemetry Stm32BridgeManager::GetLatestTelemetry() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return latest_telemetry_;
}

datalogger_Heartbeat Stm32BridgeManager::GetLatestHeartbeat() {
    std::lock_guard<std::mutex> lock(data_mutex_);
    return latest_heartbeat_;
}

CommandResponse Stm32BridgeManager::ExecuteCommandSync(const std::string& action, int32_t param_int, float param_float, const std::string& param_str, uint32_t timeout_ms) {
    uint32_t seq = next_sequence_id_++;
    auto pending_cmd = std::make_shared<PendingCommand>();
    pending_cmd->sequence_id = seq;
    pending_cmd->sent_time_ms = GetCurrentTimeMs();
    pending_cmd->timeout_ms = timeout_ms;

    std::future<CommandResponse> future = pending_cmd->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_commands_[seq] = pending_cmd;
    }

    SetLinkState(Stm32LinkState::kCommandExec);

    // Đóng gói Protobuf Command
    datalogger_BridgeEnvelope env = datalogger_BridgeEnvelope_init_zero;
    env.protocol_version = 1;
    env.msg_type = datalogger_MessageType_MSG_TYPE_COMMAND;
    env.sequence_id = seq;
    strncpy(env.source_id, "ESP32S3", sizeof(env.source_id) - 1);

    env.which_payload = datalogger_BridgeEnvelope_command_tag;
    env.payload.command.command_id = seq;
    strncpy(env.payload.command.action, action.c_str(), sizeof(env.payload.command.action) - 1);
    env.payload.command.param_int = param_int;
    env.payload.command.param_float = param_float;
    strncpy(env.payload.command.param_str, param_str.c_str(), sizeof(env.payload.command.param_str) - 1);

    uint8_t enc_buf[StreamFramer::kMaxPayloadSize];
    pb_ostream_t ostream = pb_ostream_from_buffer(enc_buf, sizeof(enc_buf));
    if (!pb_encode(&ostream, datalogger_BridgeEnvelope_fields, &env)) {
        ESP_LOGE(TAG, "Failed to encode command envelope");
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_commands_.erase(seq);
        CommandResponse err;
        err.status = CommandResultStatus::kErrInternal;
        return err;
    }

    // Đóng gói Framer Frame
    uint8_t frame_buf[StreamFramer::kMaxFrameSize];
    size_t frame_len = StreamFramer::PackFrame(0x00, enc_buf, ostream.bytes_written, frame_buf, sizeof(frame_buf));
    if (frame_len == 0 || !transport_ || !transport_->SendFrame(frame_buf, frame_len)) {
        ESP_LOGE(TAG, "Failed to send command frame via transport");
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_commands_.erase(seq);
        CommandResponse err;
        err.status = CommandResultStatus::kErrInternal;
        return err;
    }

    // Chờ kết quả đồng bộ với timeout
    std::chrono::milliseconds wait_dur(timeout_ms + 100);
    if (future.wait_for(wait_dur) == std::future_status::ready) {
        return future.get();
    } else {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_commands_.erase(seq);
        CommandResponse timeout_resp;
        timeout_resp.sequence_id = seq;
        timeout_resp.status = CommandResultStatus::kErrTimeout;
        timeout_resp.result_code = -1;
        timeout_resp.result_msg = "TIMEOUT";
        return timeout_resp;
    }
}
