#ifndef HOST_ESP_CHECK_H
#define HOST_ESP_CHECK_H

#include "esp_err.h"
#include "esp_log.h"

#define ESP_RETURN_ON_ERROR(EXPR, TAG, FMT, ...) do { \
    esp_err_t _err_rc = (EXPR); \
    if (_err_rc != ESP_OK) { \
        ESP_LOGE(TAG, FMT ": %s", ##__VA_ARGS__, esp_err_to_name(_err_rc)); \
        return _err_rc; \
    } \
} while (0)

#endif
