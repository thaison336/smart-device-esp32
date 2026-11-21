#include "stm32_mcp_tools.h"
#include "stm32_bridge_manager.h"
#include "transport/mock_bus.h"
#include <esp_log.h>

static const char* TAG = "Stm32McpTools";

void Stm32McpTools::RegisterTools(McpServer& mcp_server) {
    // 1. Tool đọc sensor telemetry
    mcp_server.AddTool(
        "stm32_read_sensors",
        "Read latest sensor values from STM32 datalogger",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto telem = Stm32BridgeManager::GetInstance().GetLatestTelemetry();
            cJSON* result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "temperature", telem.temperature);
            cJSON_AddNumberToObject(result, "humidity", telem.humidity);
            cJSON_AddNumberToObject(result, "pressure", telem.pressure);
            cJSON_AddNumberToObject(result, "battery_voltage", telem.battery_voltage);
            cJSON_AddNumberToObject(result, "current_mA", telem.current_mA);
            cJSON_AddNumberToObject(result, "digital_inputs", telem.digital_inputs);

            cJSON* analog_arr = cJSON_CreateArray();
            for (size_t i = 0; i < telem.analog_channels_count; ++i) {
                cJSON_AddItemToArray(analog_arr, cJSON_CreateNumber(telem.analog_channels[i]));
            }
            cJSON_AddItemToObject(result, "analog_channels", analog_arr);
            cJSON_AddStringToObject(result, "error_message", telem.error_message);
            return result;
        }
    );

    // 2. Tool điều khiển thiết bị (Synchronous blocking 1500ms)
    mcp_server.AddTool(
        "stm32_control_actuator",
        "Control an actuator or send a command to STM32 (synchronous, timeout 1500ms)",
        PropertyList({
            Property("action", kPropertyTypeString, std::string("set_state")),
            Property("param_int", kPropertyTypeInteger, 0, 0, 65535),
            Property("param_float", kPropertyTypeString, std::string("0.0")),
            Property("param_str", kPropertyTypeString, std::string(""))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string action = "set_state";
            int32_t param_int = 0;
            float param_float = 0.0f;
            std::string param_str = "";

            try {
                action = properties["action"].value<std::string>();
            } catch (...) {}

            try {
                param_int = properties["param_int"].value<int>();
            } catch (...) {}

            try {
                std::string flt_str = properties["param_float"].value<std::string>();
                if (!flt_str.empty()) {
                    param_float = std::strtof(flt_str.c_str(), nullptr);
                }
            } catch (...) {}

            try {
                param_str = properties["param_str"].value<std::string>();
            } catch (...) {}

            CommandResponse resp = Stm32BridgeManager::GetInstance().ExecuteCommandSync(action, param_int, param_float, param_str, 1500);

            cJSON* result = cJSON_CreateObject();
            cJSON_AddNumberToObject(result, "sequence_id", resp.sequence_id);
            cJSON_AddNumberToObject(result, "status", static_cast<int>(resp.status));
            cJSON_AddNumberToObject(result, "result_code", resp.result_code);
            cJSON_AddStringToObject(result, "result_msg", resp.result_msg.c_str());
            return result;
        }
    );

    // 3. Tool cấu hình Mock Scenario cho testing từ server
    mcp_server.AddTool(
        "stm32_set_mock_scenario",
        "Configure MockBus scenario (normal, timeout, delayed_ack, corrupt_crc, duplicate_ack, nack_busy, stm32_hang)",
        PropertyList({
            Property("scenario", kPropertyTypeString, std::string("normal"))
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto transport = Stm32BridgeManager::GetInstance().GetTransport();
            auto mock_bus = std::dynamic_pointer_cast<MockBus>(transport);
            if (!mock_bus) {
                return "MockBus is not active on current transport.";
            }

            std::string scen_str = "normal";
            try {
                scen_str = properties["scenario"].value<std::string>();
            } catch (...) {}

            if (scen_str == "normal") {
                mock_bus->SetScenario(MockBus::MockScenario::kNormal);
            } else if (scen_str == "timeout") {
                mock_bus->SetScenario(MockBus::MockScenario::kTimeout);
            } else if (scen_str == "delayed_ack") {
                mock_bus->SetScenario(MockBus::MockScenario::kDelayedAck);
            } else if (scen_str == "corrupt_crc") {
                mock_bus->SetScenario(MockBus::MockScenario::kCorruptCrc);
            } else if (scen_str == "duplicate_ack") {
                mock_bus->SetScenario(MockBus::MockScenario::kDuplicateAck);
            } else if (scen_str == "nack_busy") {
                mock_bus->SetScenario(MockBus::MockScenario::kNackBusy);
            } else if (scen_str == "stm32_hang") {
                mock_bus->SetScenario(MockBus::MockScenario::kStm32Hang);
            } else {
                return "Unknown scenario: " + scen_str;
            }

            return "Scenario set to: " + scen_str;
        }
    );

    ESP_LOGI(TAG, "Registered STM32 MCP Tools: stm32_read_sensors, stm32_control_actuator, stm32_set_mock_scenario");
}
