#ifndef UART_BUS_H
#define UART_BUS_H

#include "transport_bus.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>

class UartTransportBus : public ITransportBus {
public:
    // Hỗ trợ cả USB-Serial-JTAG (Type-C gốc của S3) lẫn phần cứng UART
    UartTransportBus(bool use_usb_serial_jtag = true);
    ~UartTransportBus() override;

    bool Initialize() override;
    void SetReceiveCallback(FrameReceiveCallback callback) override;
    bool SendFrame(const uint8_t* data, size_t length) override;

private:
    void RxTask();
    static void RxTaskEntry(void* arg);

    bool use_usb_serial_jtag_;
    FrameReceiveCallback receive_callback_{nullptr};

    TaskHandle_t rx_task_handle_{nullptr};
    std::atomic<bool> running_{false};
};

#endif // UART_BUS_H
