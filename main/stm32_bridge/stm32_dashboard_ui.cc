#include "stm32_dashboard_ui.h"
#include "stm32_bridge_manager.h"
#include "transport/mock_bus.h"
#include "transport/uart_bus.h"
#include "application.h"
#include "display.h"
#include "board.h"
#include <esp_log.h>
#include <cstdio>
#include <thread>

static const char* TAG = "Stm32DashboardUI";

static lv_obj_t* s_btn_top_toggle = nullptr;
static lv_obj_t* s_lbl_status_link = nullptr;

static lv_obj_t* s_dashboard_panel = nullptr;
static lv_obj_t* s_panel_sensors = nullptr;
static lv_obj_t* s_panel_control = nullptr;

// Labels hiển thị Sensors
static lv_obj_t* s_lbl_temp = nullptr;
static lv_obj_t* s_lbl_hum = nullptr;
static lv_obj_t* s_lbl_press = nullptr;
static lv_obj_t* s_lbl_vbat = nullptr;
static lv_obj_t* s_lbl_sd = nullptr;

// Labels & Buttons Control
static lv_obj_t* s_lbl_cmd_result = nullptr;
static lv_obj_t* s_lbl_bus_mode = nullptr;

static bool s_relay_state = false;
static bool s_pump_state = false;
static bool s_is_uart_mode = true;
static int s_active_tab = 0; // 0: Sensors, 1: Control

static void OnToggleDashboardClicked(lv_event_t* e) {
    if (s_dashboard_panel == nullptr) return;
    if (lv_obj_has_flag(s_dashboard_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_dashboard_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_dashboard_panel);
    } else {
        lv_obj_add_flag(s_dashboard_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void OnTabSensorsClicked(lv_event_t* e) {
    s_active_tab = 0;
    if (s_panel_sensors && s_panel_control) {
        lv_obj_remove_flag(s_panel_sensors, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_panel_control, LV_OBJ_FLAG_HIDDEN);
    }
}

static void OnTabControlClicked(lv_event_t* e) {
    s_active_tab = 1;
    if (s_panel_sensors && s_panel_control) {
        lv_obj_add_flag(s_panel_sensors, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_panel_control, LV_OBJ_FLAG_HIDDEN);
    }
}

void Stm32DashboardUI::Setup(lv_obj_t* parent_screen) {
    if (s_dashboard_panel != nullptr) return;

    // Đặt nút toggle và Dashboard lên lv_layer_top() để LUÔN LUÔN nằm trên cùng mọi màn hình chat/emoji
    lv_obj_t* top_layer = lv_layer_top();

    // 1. Nút bấm viên thuốc (Pill Button) nằm ở mép trên cùng màn hình
    s_btn_top_toggle = lv_button_create(top_layer);
    lv_obj_set_size(s_btn_top_toggle, 180, 32);
    lv_obj_align(s_btn_top_toggle, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_color(s_btn_top_toggle, lv_color_hex(0x1B2A4A), 0);
    lv_obj_set_style_radius(s_btn_top_toggle, 16, 0);
    lv_obj_set_style_border_width(s_btn_top_toggle, 2, 0);
    lv_obj_set_style_border_color(s_btn_top_toggle, lv_color_hex(0x42A5F5), 0);
    lv_obj_add_event_cb(s_btn_top_toggle, OnToggleDashboardClicked, LV_EVENT_CLICKED, nullptr);

    s_lbl_status_link = lv_label_create(s_btn_top_toggle);
    lv_label_set_text(s_lbl_status_link, "STM32: CONNECTING");
    lv_obj_set_style_text_color(s_lbl_status_link, lv_color_hex(0xFFA500), 0);
    lv_obj_center(s_lbl_status_link);

    // 2. Dashboard Panel Overlay toàn màn hình (368x448) đặt trên top layer
    s_dashboard_panel = lv_obj_create(top_layer);
    lv_obj_set_size(s_dashboard_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(s_dashboard_panel, 0, 0);
    lv_obj_set_style_bg_color(s_dashboard_panel, lv_color_hex(0x0A0E14), 0);
    lv_obj_set_style_radius(s_dashboard_panel, 0, 0);
    lv_obj_set_style_border_width(s_dashboard_panel, 0, 0);
    lv_obj_set_style_pad_all(s_dashboard_panel, 10, 0);
    lv_obj_add_flag(s_dashboard_panel, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn, chạm vào nút status trên cùng để mở

    // Header của Dashboard
    auto top_header = lv_obj_create(s_dashboard_panel);
    lv_obj_set_size(top_header, LV_HOR_RES - 20, 46);
    lv_obj_set_style_bg_opa(top_header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_header, 0, 0);
    lv_obj_set_style_pad_all(top_header, 0, 0);
    lv_obj_set_flex_flow(top_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto lbl_title = lv_label_create(top_header);
    lv_label_set_text(lbl_title, "STM32 DASHBOARD");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x42A5F5), 0);

    auto btn_close = lv_button_create(top_header);
    lv_obj_set_size(btn_close, 60, 36);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xD32F2F), 0);
    lv_obj_set_style_radius(btn_close, 18, 0);
    lv_obj_add_event_cb(btn_close, OnToggleDashboardClicked, LV_EVENT_CLICKED, nullptr);
    auto lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, "DONG");
    lv_obj_center(lbl_close);

    // Thanh chuyển Tab (Sensors / Controls)
    auto tab_bar = lv_obj_create(s_dashboard_panel);
    lv_obj_set_size(tab_bar, LV_HOR_RES - 20, 46);
    lv_obj_align(tab_bar, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0x16202C), 0);
    lv_obj_set_style_radius(tab_bar, 12, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 4, 0);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto btn_tab_sensors = lv_button_create(tab_bar);
    lv_obj_set_size(btn_tab_sensors, 150, 38);
    lv_obj_set_style_bg_color(btn_tab_sensors, lv_color_hex(0x1976D2), 0);
    lv_obj_add_event_cb(btn_tab_sensors, OnTabSensorsClicked, LV_EVENT_CLICKED, nullptr);
    auto lbl_tab_s = lv_label_create(btn_tab_sensors);
    lv_label_set_text(lbl_tab_s, "TELEMETRY");
    lv_obj_center(lbl_tab_s);

    auto btn_tab_ctrl = lv_button_create(tab_bar);
    lv_obj_set_size(btn_tab_ctrl, 150, 38);
    lv_obj_set_style_bg_color(btn_tab_ctrl, lv_color_hex(0x388E3C), 0);
    lv_obj_add_event_cb(btn_tab_ctrl, OnTabControlClicked, LV_EVENT_CLICKED, nullptr);
    auto lbl_tab_c = lv_label_create(btn_tab_ctrl);
    lv_label_set_text(lbl_tab_c, "DIEU KHIEN");
    lv_obj_center(lbl_tab_c);

    // =============================================================
    // Panel 1: Sensors Telemetry
    // =============================================================
    s_panel_sensors = lv_obj_create(s_dashboard_panel);
    lv_obj_set_size(s_panel_sensors, LV_HOR_RES - 20, LV_VER_RES - 120);
    lv_obj_align(s_panel_sensors, LV_ALIGN_TOP_MID, 0, 105);
    lv_obj_set_style_bg_color(s_panel_sensors, lv_color_hex(0x121A24), 0);
    lv_obj_set_style_radius(s_panel_sensors, 14, 0);
    lv_obj_set_style_border_color(s_panel_sensors, lv_color_hex(0x243447), 0);
    lv_obj_set_style_pad_all(s_panel_sensors, 16, 0);
    lv_obj_set_flex_flow(s_panel_sensors, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel_sensors, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    s_lbl_temp = lv_label_create(s_panel_sensors);
    lv_label_set_text(s_lbl_temp, "Nhiet do: --.- C");
    lv_obj_set_style_text_color(s_lbl_temp, lv_color_white(), 0);

    s_lbl_hum = lv_label_create(s_panel_sensors);
    lv_label_set_text(s_lbl_hum, "Do am: --.- %");
    lv_obj_set_style_text_color(s_lbl_hum, lv_color_white(), 0);

    s_lbl_press = lv_label_create(s_panel_sensors);
    lv_label_set_text(s_lbl_press, "Ap suat: ----.-- hPa");
    lv_obj_set_style_text_color(s_lbl_press, lv_color_white(), 0);

    s_lbl_vbat = lv_label_create(s_panel_sensors);
    lv_label_set_text(s_lbl_vbat, "Pin: -.-- V");
    lv_obj_set_style_text_color(s_lbl_vbat, lv_color_white(), 0);

    s_lbl_sd = lv_label_create(s_panel_sensors);
    lv_label_set_text(s_lbl_sd, "The SD: DANG KET NOI");
    lv_obj_set_style_text_color(s_lbl_sd, lv_color_hex(0x81C784), 0);

    // =============================================================
    // Panel 2: Controls & Test
    // =============================================================
    s_panel_control = lv_obj_create(s_dashboard_panel);
    lv_obj_set_size(s_panel_control, LV_HOR_RES - 20, LV_VER_RES - 120);
    lv_obj_align(s_panel_control, LV_ALIGN_TOP_MID, 0, 105);
    lv_obj_set_style_bg_color(s_panel_control, lv_color_hex(0x121A24), 0);
    lv_obj_set_style_radius(s_panel_control, 14, 0);
    lv_obj_set_style_border_color(s_panel_control, lv_color_hex(0x243447), 0);
    lv_obj_set_style_pad_all(s_panel_control, 12, 0);
    lv_obj_set_flex_flow(s_panel_control, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel_control, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_panel_control, LV_OBJ_FLAG_HIDDEN); // Ẩn ban đầu

    auto btn_relay = lv_button_create(s_panel_control);
    lv_obj_set_size(btn_relay, 260, 48);
    lv_obj_set_style_bg_color(btn_relay, lv_color_hex(0x1976D2), 0);
    lv_obj_set_style_radius(btn_relay, 12, 0);
    lv_obj_add_event_cb(btn_relay, OnRelayBtnClicked, LV_EVENT_CLICKED, nullptr);
    auto lbl_btn_r = lv_label_create(btn_relay);
    lv_label_set_text(lbl_btn_r, "BAT / TAT RELAY 1");
    lv_obj_center(lbl_btn_r);

    auto btn_pump = lv_button_create(s_panel_control);
    lv_obj_set_size(btn_pump, 260, 48);
    lv_obj_set_style_bg_color(btn_pump, lv_color_hex(0x388E3C), 0);
    lv_obj_set_style_radius(btn_pump, 12, 0);
    lv_obj_add_event_cb(btn_pump, OnPumpBtnClicked, LV_EVENT_CLICKED, nullptr);
    auto lbl_btn_p = lv_label_create(btn_pump);
    lv_label_set_text(lbl_btn_p, "BAT / TAT BOM");
    lv_obj_center(lbl_btn_p);

    auto btn_bus = lv_button_create(s_panel_control);
    lv_obj_set_size(btn_bus, 260, 48);
    lv_obj_set_style_bg_color(btn_bus, lv_color_hex(0x7B1FA2), 0);
    lv_obj_set_style_radius(btn_bus, 12, 0);
    lv_obj_add_event_cb(btn_bus, OnSwitchBusBtnClicked, LV_EVENT_CLICKED, nullptr);
    s_lbl_bus_mode = lv_label_create(btn_bus);
    lv_label_set_text(s_lbl_bus_mode, "MODE: UART TYPE-C");
    lv_obj_center(s_lbl_bus_mode);

    s_lbl_cmd_result = lv_label_create(s_panel_control);
    lv_label_set_text(s_lbl_cmd_result, "Ket qua: SAN SANG");
    lv_obj_set_style_text_color(s_lbl_cmd_result, lv_color_hex(0xCCCCCC), 0);

    ESP_LOGI(TAG, "Stm32DashboardUI initialized cleanly");
}

void Stm32DashboardUI::UpdateTelemetryUI() {
    auto display = Board::GetInstance().GetDisplay();
    if (display == nullptr) return;

    auto telem = Stm32BridgeManager::GetInstance().GetLatestTelemetry();

    Application::GetInstance().Schedule([telem]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display == nullptr) return;
        DisplayLockGuard lock(display);

        if (s_lbl_temp) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Nhiet do: %.1f C", telem.temperature);
            lv_label_set_text(s_lbl_temp, buf);
        }
        if (s_lbl_hum) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Do am: %.1f %%", telem.humidity);
            lv_label_set_text(s_lbl_hum, buf);
        }
        if (s_lbl_press) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Ap suat: %.1f hPa", telem.pressure);
            lv_label_set_text(s_lbl_press, buf);
        }
        if (s_lbl_vbat) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Pin: %.2f V", telem.battery_voltage);
            lv_label_set_text(s_lbl_vbat, buf);
        }
        if (s_lbl_sd) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "The SD: %s", telem.error_message);
            lv_label_set_text(s_lbl_sd, buf);
        }
    });
}

void Stm32DashboardUI::UpdateLinkStatusUI() {
    auto state = Stm32BridgeManager::GetInstance().GetLinkState();

    Application::GetInstance().Schedule([state]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display == nullptr) return;
        DisplayLockGuard lock(display);

        if (s_lbl_status_link) {
            if (state == Stm32LinkState::kOnlineIdle || state == Stm32LinkState::kCommandExec) {
                lv_label_set_text(s_lbl_status_link, "STM32: ONLINE");
                lv_obj_set_style_text_color(s_lbl_status_link, lv_color_hex(0x4CAF50), 0);
            } else if (state == Stm32LinkState::kDegraded || state == Stm32LinkState::kStm32Recovery) {
                lv_label_set_text(s_lbl_status_link, "STM32: TIMEOUT");
                lv_obj_set_style_text_color(s_lbl_status_link, lv_color_hex(0xE53935), 0);
            } else {
                lv_label_set_text(s_lbl_status_link, "STM32: CONNECTING");
                lv_obj_set_style_text_color(s_lbl_status_link, lv_color_hex(0xFFA500), 0);
            }
        }
    });
}

void Stm32DashboardUI::OnRelayBtnClicked(lv_event_t* e) {
    s_relay_state = !s_relay_state;
    int32_t val = s_relay_state ? 1 : 0;
    ESP_LOGI(TAG, "Touch UI: Sending relay command val=%ld", static_cast<long>(val));

    std::thread([val]() {
        CommandResponse resp = Stm32BridgeManager::GetInstance().ExecuteCommandSync("relay_1", val, 0.0f, "", 1500);

        Application::GetInstance().Schedule([resp, val]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display == nullptr) return;
            DisplayLockGuard lock(display);

            if (s_lbl_cmd_result) {
                char buf[96];
                const char* st_str = (resp.status == CommandResultStatus::kAckApplied) ? "ACK_OK" : "TIMEOUT";
                std::snprintf(buf, sizeof(buf), "Relay %s -> %s", val ? "ON" : "OFF", st_str);
                lv_label_set_text(s_lbl_cmd_result, buf);
                lv_obj_set_style_text_color(s_lbl_cmd_result,
                    (resp.status == CommandResultStatus::kAckApplied) ? lv_color_hex(0x4CAF50) : lv_color_hex(0xE53935), 0);
            }
        });
    }).detach();
}

void Stm32DashboardUI::OnPumpBtnClicked(lv_event_t* e) {
    s_pump_state = !s_pump_state;
    int32_t val = s_pump_state ? 1 : 0;
    ESP_LOGI(TAG, "Touch UI: Sending pump command val=%ld", static_cast<long>(val));

    std::thread([val]() {
        CommandResponse resp = Stm32BridgeManager::GetInstance().ExecuteCommandSync("pump_main", val, 0.0f, "", 1500);

        Application::GetInstance().Schedule([resp, val]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display == nullptr) return;
            DisplayLockGuard lock(display);

            if (s_lbl_cmd_result) {
                char buf[96];
                const char* st_str = (resp.status == CommandResultStatus::kAckApplied) ? "ACK_OK" : "TIMEOUT";
                std::snprintf(buf, sizeof(buf), "Pump %s -> %s", val ? "ON" : "OFF", st_str);
                lv_label_set_text(s_lbl_cmd_result, buf);
                lv_obj_set_style_text_color(s_lbl_cmd_result,
                    (resp.status == CommandResultStatus::kAckApplied) ? lv_color_hex(0x4CAF50) : lv_color_hex(0xE53935), 0);
            }
        });
    }).detach();
}

void Stm32DashboardUI::OnSwitchBusBtnClicked(lv_event_t* e) {
    s_is_uart_mode = !s_is_uart_mode;
    ESP_LOGI(TAG, "Switching transport bus mode to: %s", s_is_uart_mode ? "UART" : "MOCK");

    Stm32BridgeManager::GetInstance().Stop();

    if (s_is_uart_mode) {
        auto uart_bus = std::make_shared<UartTransportBus>(true);
        Stm32BridgeManager::GetInstance().Start(uart_bus);
        if (s_lbl_bus_mode) lv_label_set_text(s_lbl_bus_mode, "MODE: UART TYPE-C");
    } else {
        auto mock_bus = std::make_shared<MockBus>();
        mock_bus->StartPeriodicWorker();
        Stm32BridgeManager::GetInstance().Start(mock_bus);
        if (s_lbl_bus_mode) lv_label_set_text(s_lbl_bus_mode, "MODE: MOCK BUS");
    }
}
