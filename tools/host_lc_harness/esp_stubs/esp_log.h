#ifndef HOST_ESP_LOG_H
#define HOST_ESP_LOG_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_log_timestamp(void);
void host_esp_log_write(const char *level, const char *tag, const char *fmt, ...);

#define ESP_LOGE(TAG, FMT, ...) host_esp_log_write("E", TAG, FMT, ##__VA_ARGS__)
#define ESP_LOGW(TAG, FMT, ...) host_esp_log_write("W", TAG, FMT, ##__VA_ARGS__)
#define ESP_LOGI(TAG, FMT, ...) host_esp_log_write("I", TAG, FMT, ##__VA_ARGS__)
#define ESP_LOGD(TAG, FMT, ...) host_esp_log_write("D", TAG, FMT, ##__VA_ARGS__)
#define ESP_LOGV(TAG, FMT, ...) host_esp_log_write("V", TAG, FMT, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
