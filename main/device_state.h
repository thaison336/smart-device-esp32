#ifndef _DEVICE_STATE_H_
#define _DEVICE_STATE_H_

enum DeviceState {
    kDeviceStateUnknown,
    kDeviceStateStarting,
    kDeviceStateWifiConfiguring,
    kDeviceStateIdle,
    kDeviceStateConnecting,
    kDeviceStateListening,
    kDeviceStateSpeaking,
    kDeviceStateNotifying,
    kDeviceStateUpgrading,
    kDeviceStateActivating,
    kDeviceStateAudioTesting,
    kDeviceStateFatalError,

    // Project-level states owned by the ESP32 application.
    // Existing Xiaozhi states continue to represent the voice sub-states.
    kDeviceStateNetworkConnecting,
    kDeviceStateOnlineIdle,
    kDeviceStateDataFetch,
    kDeviceStateAiInteraction,
    kDeviceStateCommandExec,
    kDeviceStateFirmwareOta,
    kDeviceStateOffline,
    kDeviceStateStm32Recovery,
    kDeviceStatePowerDown
};

#endif // _DEVICE_STATE_H_ 