#include "audio_debugger.h"

#include "sdkconfig.h"

#if CONFIG_USE_AUDIO_DEBUGGER
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <esp_log.h>
#include <string>
#include <unistd.h>
#endif

#define TAG "AudioDebugger"

AudioDebugger::AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    udp_sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sockfd_ >= 0) {
        int broadcast_enabled = 1;
        if (setsockopt(udp_sockfd_, SOL_SOCKET, SO_BROADCAST,
                       &broadcast_enabled, sizeof(broadcast_enabled)) != 0) {
            ESP_LOGW(TAG, "Failed to enable UDP broadcast: %d", errno);
        }

        memset(&udp_server_addr_, 0, sizeof(udp_server_addr_));
        udp_server_addr_.sin_family = AF_INET;
        udp_server_addr_.sin_port = htons(8000);
        udp_server_addr_.sin_addr.s_addr = htonl(INADDR_BROADCAST);

        ESP_LOGI(TAG, "Initialized UDP broadcast address: 255.255.255.255:8000");
    } else {
        ESP_LOGW(TAG, "Failed to create UDP socket: %d", errno);
    }
#endif
}

AudioDebugger::~AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        close(udp_sockfd_);
        ESP_LOGI(TAG, "Closed UDP socket");
    }
#endif
}

void AudioDebugger::Feed(const std::vector<int16_t>& data) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        ssize_t sent = sendto(udp_sockfd_, data.data(), data.size() * sizeof(int16_t), 0,
            reinterpret_cast<struct sockaddr*>(&udp_server_addr_), sizeof(udp_server_addr_));
        if (sent < 0) {
            ESP_LOGW(TAG, "Failed to send audio broadcast: %d", errno);
        } else {
            ESP_LOGD(TAG, "Sent %d bytes audio broadcast", sent);
        }
    }
#endif
}
