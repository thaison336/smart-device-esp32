#include "uart_bus.h"
#include <esp_log.h>
#include <driver/usb_serial_jtag.h>
#include <driver/uart.h>
#include <cstring>

static const char* TAG = "UartBus";

UartTransportBus::UartTransportBus(bool use_usb_serial_jtag)
    : use_usb_serial_jtag_(use_usb_serial_jtag) {}

UartTransportBus::~UartTransportBus() {
    running_.store(false);
    if (rx_task_handle_ != nullptr) {
        vTaskDelete(rx_task_handle_);
        rx_task_handle_ = nullptr;
    }
    if (use_usb_serial_jtag_) {
        usb_serial_jtag_driver_uninstall();
    } else {
        uart_driver_delete(UART_NUM_0);
    }
}

bool UartTransportBus::Initialize() {
    if (use_usb_serial_jtag_) {
        usb_serial_jtag_driver_config_t jtag_config = {
            .tx_buffer_size = 1024,
            .rx_buffer_size = 2048,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&jtag_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "UartTransportBus initialized via USB_SERIAL_JTAG (Type-C Native)");
    } else {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity    = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .rx_flow_ctrl_thresh = 122,
            .source_clk = UART_SCLK_DEFAULT,
            .flags = {
                .allow_pd = 0,
                .backup_before_sleep = 0
            }
        };
        uart_param_config(UART_NUM_0, &uart_config);
        uart_set_pin(UART_NUM_0, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_driver_install(UART_NUM_0, 2048, 1024, 0, nullptr, 0);
        ESP_LOGI(TAG, "UartTransportBus initialized via UART0 (115200 bps)");
    }

    running_.store(true);
    BaseType_t ret = xTaskCreatePinnedToCore(
        RxTaskEntry,
        "uart_rx_task",
        3072,
        this,
        6,
        &rx_task_handle_,
        1 // Core 1
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create rx task");
        running_.store(false);
        return false;
    }

    return true;
}

void UartTransportBus::SetReceiveCallback(FrameReceiveCallback callback) {
    receive_callback_ = callback;
}

bool UartTransportBus::SendFrame(const uint8_t* data, size_t length) {
    if (data == nullptr || length == 0) return false;
    if (use_usb_serial_jtag_) {
        int written = usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(100));
        return (written == static_cast<int>(length));
    } else {
        int written = uart_write_bytes(UART_NUM_0, data, length);
        return (written == static_cast<int>(length));
    }
}

void UartTransportBus::RxTaskEntry(void* arg) {
    static_cast<UartTransportBus*>(arg)->RxTask();
}

void UartTransportBus::RxTask() {
    uint8_t rx_buffer[128];
    while (running_.load()) {
        int len = 0;
        if (use_usb_serial_jtag_) {
            len = usb_serial_jtag_read_bytes(rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(50));
        } else {
            len = uart_read_bytes(UART_NUM_0, rx_buffer, sizeof(rx_buffer), pdMS_TO_TICKS(50));
        }

        if (len > 0) {
            if (receive_callback_) {
                receive_callback_(rx_buffer, static_cast<size_t>(len));
            }
        }
    }
}
