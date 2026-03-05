#include "tool_gpio_ctl.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "tool_gpio";

// GPIO初始化标志，避免重复初始化
static bool gpio_initialized[GPIO_NUM_MAX] = {false};

void tool_gpio_init(void) {
    // 如果需要全局初始化某些GPIO，可以在这里实现
    ESP_LOGI(TAG, "GPIO tool initialized");
}

/**
 * @brief 安全初始化GPIO引脚
 * @param pin GPIO引脚号
 * @return esp_err_t 
 */
static esp_err_t ensure_gpio_initialized(gpio_num_t pin) {
    // 检查引脚号是否有效
    if (pin < 0 || pin >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid GPIO pin: %d", pin);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 如果已经初始化过，直接返回成功
    if (gpio_initialized[pin]) {
        return ESP_OK;
    }
    
    // 配置GPIO为输出模式
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,    // 禁止中断
        .mode = GPIO_MODE_OUTPUT,           // 输出模式
        .pin_bit_mask = (1ULL << pin),      // 要配置的引脚
        .pull_down_en = 0,                   // 不使能下拉
        .pull_up_en = 0,                     // 不使能上拉
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        gpio_initialized[pin] = true;
        ESP_LOGI(TAG, "GPIO %d initialized as output", pin);
    } else {
        ESP_LOGE(TAG, "Failed to configure GPIO %d: %s", pin, esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t tool_gpio_control_execute(const char *input_json, char *output, size_t output_size) {
    esp_err_t ret = ESP_OK;
    cJSON *json = NULL;
    cJSON *result = NULL;
    char *result_str = NULL;
    
    // 检查输入参数
    if (input_json == NULL || output == NULL || output_size == 0) {
        ESP_LOGE(TAG, "Invalid input parameters");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 清空输出缓冲区
    memset(output, 0, output_size);
    
    // 解析输入的JSON
    json = cJSON_Parse(input_json);
    if (json == NULL) {
        ESP_LOGE(TAG, "Failed to parse input JSON: %s", input_json);
        snprintf(output, output_size, 
            "{\"error\":\"Invalid JSON format\",\"status\":\"failed\"}");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 创建结果JSON对象
    result = cJSON_CreateObject();
    if (result == NULL) {
        ESP_LOGE(TAG, "Failed to create result JSON");
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    
    // 获取pin参数
    cJSON *pin_json = cJSON_GetObjectItem(json, "pin");
    if (pin_json == NULL || !cJSON_IsNumber(pin_json)) {
        ESP_LOGE(TAG, "Missing or invalid 'pin' parameter");
        cJSON_AddStringToObject(result, "status", "failed");
        cJSON_AddStringToObject(result, "error", "Missing or invalid 'pin' parameter");
        goto cleanup;
    }
    int pin = pin_json->valueint;
    
    // 获取state参数
    cJSON *state_json = cJSON_GetObjectItem(json, "state");
    if (state_json == NULL || !cJSON_IsString(state_json)) {
        ESP_LOGE(TAG, "Missing or invalid 'state' parameter");
        cJSON_AddStringToObject(result, "status", "failed");
        cJSON_AddStringToObject(result, "error", "Missing or invalid 'state' parameter");
        goto cleanup;
    }
    const char *state = state_json->valuestring;
    
    // 验证state值
    int level;
    if (strcmp(state, "on") == 0 || strcmp(state, "high") == 0 || strcmp(state, "1") == 0) {
        level = 1;
    } else if (strcmp(state, "off") == 0 || strcmp(state, "low") == 0 || strcmp(state, "0") == 0) {
        level = 0;
    } else {
        ESP_LOGE(TAG, "Invalid state value: %s", state);
        cJSON_AddStringToObject(result, "status", "failed");
        cJSON_AddStringToObject(result, "error", "State must be 'on'/'high'/'1' or 'off'/'low'/'0'");
        goto cleanup;
    }
    
    // 获取可选的active_low参数（低电平有效）
    cJSON *active_low_json = cJSON_GetObjectItem(json, "active_low");
    bool active_low = false;
    if (active_low_json != NULL && cJSON_IsBool(active_low_json)) {
        active_low = cJSON_IsTrue(active_low_json);
    }
    
    // 如果有active_low，反转电平逻辑
    if (active_low) {
        level = !level;
        ESP_LOGI(TAG, "Active low enabled, final level: %d", level);
    }
    
    // 获取可选的init参数（是否自动初始化引脚）
    cJSON *init_json = cJSON_GetObjectItem(json, "auto_init");
    bool auto_init = true;  // 默认自动初始化
    if (init_json != NULL && cJSON_IsBool(init_json)) {
        auto_init = cJSON_IsTrue(init_json);
    }
    
    // 如果需要，自动初始化GPIO
    if (auto_init) {
        ret = ensure_gpio_initialized(pin);
        if (ret != ESP_OK) {
            cJSON_AddStringToObject(result, "status", "failed");
            cJSON_AddStringToObject(result, "error", "Failed to initialize GPIO pin");
            cJSON_AddNumberToObject(result, "pin", pin);
            cJSON_AddStringToObject(result, "state", state);
            goto cleanup;
        }
    }
    
    // 设置GPIO电平
    ret = gpio_set_level(pin, level);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set GPIO %d level %d: %s", pin, level, esp_err_to_name(ret));
        cJSON_AddStringToObject(result, "status", "failed");
        cJSON_AddStringToObject(result, "error", "Failed to set GPIO level");
        cJSON_AddNumberToObject(result, "pin", pin);
        cJSON_AddStringToObject(result, "state", state);
        goto cleanup;
    }
    
    // 构建成功响应
    ESP_LOGI(TAG, "GPIO %d set to %s (level %d)", pin, state, level);
    cJSON_AddStringToObject(result, "status", "success");
    cJSON_AddNumberToObject(result, "pin", pin);
    cJSON_AddStringToObject(result, "state", state);
    cJSON_AddNumberToObject(result, "level", level);
    cJSON_AddBoolToObject(result, "active_low", active_low);
    
    // 如果引脚是板载LED，添加提示
    #ifdef CONFIG_LED_GPIO
    if (pin == CONFIG_LED_GPIO) {
        cJSON_AddStringToObject(result, "note", "This is the onboard LED");
    }
    #endif

cleanup:
    // 将结果转换为字符串
    result_str = cJSON_Print(result);
    if (result_str != NULL) {
        strncpy(output, result_str, output_size - 1);
        output[output_size - 1] = '\0';  // 确保字符串结束
        free(result_str);
    } else {
        snprintf(output, output_size, "{\"status\":\"failed\",\"error\":\"Internal error\"}");
    }
    
    // 清理内存
    cJSON_Delete(json);
    cJSON_Delete(result);
    
    return ESP_OK;
}