#ifndef STM32_DASHBOARD_UI_H
#define STM32_DASHBOARD_UI_H

#include <lvgl.h>

class Stm32DashboardUI {
public:
    static void Setup(lv_obj_t* parent_screen);
    static void UpdateTelemetryUI();
    static void UpdateLinkStatusUI();

private:
    static void OnRelayBtnClicked(lv_event_t* e);
    static void OnPumpBtnClicked(lv_event_t* e);
    static void OnSwitchBusBtnClicked(lv_event_t* e);
};

#endif // STM32_DASHBOARD_UI_H
