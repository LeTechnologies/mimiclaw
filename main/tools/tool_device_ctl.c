#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "led_strip.h"
#include "tool_device_ctl.h"

static const char *TAG = "tool_device_ctl";

// ==================== 设备列表配置（方式2：JSON字符串） ====================

const char *DEVICE_CONFIG_JSON = 
"{"
    "\"devices\": ["
        "{"
            "\"name\": \"卧室灯\","
            "\"room\": \"卧室\","
            "\"type\": \"light\","
            "\"interface\": \"gpio\","
            "\"config\": {"
                "\"pin\": 15,"
                "\"active_low\": false"
            "},"
            "\"description\": \"卧室主灯\""
        "},"
        "{"
            "\"name\": \"客厅风扇\","
            "\"room\": \"客厅\","
            "\"type\": \"fan\","
            "\"interface\": \"pwm\","
            "\"config\": {"
                "\"pin\": 4,"
                "\"freq\": 1000,"
                "\"resolution\": 8,"
                "\"min_duty\": 0,"
                "\"max_duty\": 255"
            "},"
            "\"description\": \"客厅吊扇\""
        "},"
        "{"
            "\"name\": \"床头灯\","
            "\"room\": \"卧室\","
            "\"type\": \"light\","
            "\"interface\": \"led_strip\","
            "\"config\": {"
                "\"pin\": 5"
            "},"
            "\"description\": \"床头阅读灯（给value参数传入十六进制RGB控制颜色）\""
        "},"
        "{"
            "\"name\": \"窗帘电机\","
            "\"room\": \"客厅\","
            "\"type\": \"motor\","
            "\"interface\": \"pwm\","
            "\"config\": {"
                "\"pin\": 6,"
                "\"freq\": 50,"
                "\"resolution\": 10,"
                "\"min_duty\": 0,"
                "\"max_duty\": 1023"
            "},"
            "\"description\": \"智能窗帘\""
        "},"
        "{"
            "\"name\": \"阳台灯\","
            "\"room\": \"阳台\","
            "\"type\": \"light\","
            "\"interface\": \"gpio\","
            "\"config\": {"
                "\"pin\": 7,"
                "\"active_low\": true"
            "},"
            "\"description\": \"阳台吸顶灯（低电平有效）\""
        "}"
    "]"
"}";

// ==================== 设备信息结构体 ====================

// typedef enum {
//     DEVICE_TYPE_LIGHT = 0,      // 灯光（GPIO控制）
//     DEVICE_TYPE_FAN,            // 风扇（GPIO或PWM）
//     DEVICE_TYPE_RELAY,          // 继电器（GPIO）
//     DEVICE_TYPE_MOTOR,          // 电机（PWM）
//     DEVICE_TYPE_LED_STRIP,      // LED灯带（PWM）
//     DEVICE_TYPE_SENSOR,         // 传感器（I2C/GPIO）
//     DEVICE_TYPE_UNKNOWN
// } device_type_t;

typedef enum {
    CTRL_INTERFACE_GPIO,         // 数字GPIO
    CTRL_INTERFACE_PWM,          // PWM输出
    CTRL_INTERFACE_I2C,          // I2C设备
    CTRL_INTERFACE_UART,         // 串口设备
    CTRL_INTERFACE_LED_STRIP,      // RGB灯
    // CTRL_INTERFACE_ONEWIRE,      // 单总线设备
    INTERFACE_UNKNOWN
} interface_type_t;

// 设备信息结构
typedef struct {
    char name[32];                 // 设备名称（如"卧室灯"）
    char room[20];                 // 所在房间（如"bedroom"）
    char type[20];
    // device_type_t type;         // 设备类型
    interface_type_t interface;    // 控制接口类型
    
    union {
        struct {                   // GPIO设备
            uint8_t pin;           // GPIO引脚号
            bool active_low;       // 是否低电平有效
        } gpio;
        
        struct {                   // PWM设备
            uint8_t pin;          
            uint32_t freq;         // PWM频率
            uint8_t resolution;    // 分辨率（位）
            uint32_t min_duty;     // 最小占空比（关）
            uint32_t max_duty;     // 最大占空比（开）
        } pwm;
        
        struct {                   // I2C设备
            uint8_t addr;          // I2C地址
            i2c_port_t port;       // I2C设备号
            uint8_t scl;
            uint8_t sda;
            uint8_t reg_ctrl;      // 控制寄存器
        } i2c;

        struct {
            uart_port_t port;
            uint8_t rx;
            uint8_t tx;
        } uart;

        struct {
            uint8_t pin; 
            led_strip_handle_t led_strip;
        } led_strip;
        
    } config;
    
    bool is_on;                    // 当前开关状态
    uint32_t value;                // 当前值（如亮度、速度）
    
    bool enabled;                  // 是否启用
    char description[64];          // 设备描述
} device_info_t;

#define MAX_DEVICES 20
static device_info_t s_device_list[MAX_DEVICES];
static int s_device_count = 0;

// ==================== 设备列表初始化 ====================

/**
 * @brief 从JSON字符串解析设备列表
 */
static esp_err_t device_list_init(void)
{
    if (s_device_count > 0) {
        return ESP_OK;  // 已经初始化
    }
    
    cJSON *root = cJSON_Parse(DEVICE_CONFIG_JSON);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse device config JSON");
        return ESP_ERR_INVALID_ARG;
    }
    
    cJSON *devices = cJSON_GetObjectItem(root, "devices");
    if (!cJSON_IsArray(devices)) {
        ESP_LOGE(TAG, "Failed to parse device array");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    int count = cJSON_GetArraySize(devices);
    if (count > MAX_DEVICES) {
        ESP_LOGW(TAG, "Too many devices, truncating to %d", MAX_DEVICES);
        count = MAX_DEVICES;
    }
    
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(devices, i);
        device_info_t *dev = &s_device_list[i];
        memset(dev, 0, sizeof(device_info_t));
        
        // 解析基础信息
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *room = cJSON_GetObjectItem(item, "room");
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *interface_str = cJSON_GetObjectItem(item, "interface");
        cJSON *desc = cJSON_GetObjectItem(item, "description");
        
        if (name) strlcpy(dev->name, name->valuestring, sizeof(dev->name));
        if (room) strlcpy(dev->room, room->valuestring, sizeof(dev->room));
        if (type) strlcpy(dev->type, type->valuestring, sizeof(dev->type));
        if (desc) strlcpy(dev->description, desc->valuestring, sizeof(dev->description));
        
        // 解析设备类型
        // if (type_str) {
        //     if (strcmp(type_str->valuestring, "light") == 0) {
        //         dev->type = DEVICE_TYPE_LIGHT;
        //     } else if (strcmp(type_str->valuestring, "fan") == 0) {
        //         dev->type = DEVICE_TYPE_FAN;
        //     } else if (strcmp(type_str->valuestring, "motor") == 0) {
        //         dev->type = DEVICE_TYPE_MOTOR;
        //     } else {
        //         dev->type = DEVICE_TYPE_UNKNOWN;
        //     }
        // }

        // 解析接口类型
        if (interface_str) {
            if (strcmp(interface_str->valuestring, "gpio") == 0) {
                dev->interface = CTRL_INTERFACE_GPIO;
            } else if (strcmp(interface_str->valuestring, "pwm") == 0) {
                dev->interface = CTRL_INTERFACE_PWM;
            } else if (strcmp(interface_str->valuestring, "i2c") == 0) {
                dev->interface = CTRL_INTERFACE_I2C;
            } else if (strcmp(interface_str->valuestring, "uart") == 0) {
                dev->interface = CTRL_INTERFACE_UART;
            } else if (strcmp(interface_str->valuestring, "led_strip") == 0) {
                dev->interface = CTRL_INTERFACE_LED_STRIP;
            // } else if (strcmp(interface_str->valuestring, "ONEWIRE") == 0) {
            //     dev->interface = CTRL_INTERFACE_ONEWIRE;
            } else {
                ESP_LOGW(TAG, "Found unknow interface devices: %s", dev->name);
                dev->interface = INTERFACE_UNKNOWN;
            }
        }
        
        // 解析配置
        cJSON *config = cJSON_GetObjectItem(item, "config");
        if (config) {
            if (dev->interface == CTRL_INTERFACE_GPIO) {
                cJSON *pin = cJSON_GetObjectItem(config, "pin");
                cJSON *active_low = cJSON_GetObjectItem(config, "active_low");
                
                if (pin) dev->config.gpio.pin = pin->valueint;
                if (active_low) dev->config.gpio.active_low = active_low->valueint;

                gpio_config_t _gpio_config = {
                    .intr_type = GPIO_INTR_DISABLE,
                    .mode = GPIO_MODE_OUTPUT,
                    .pin_bit_mask = (1ULL << dev->config.gpio.pin),
                    .pull_down_en = false,
                    .pull_up_en = false
                };
                gpio_config(&_gpio_config);
                
            } else if (dev->interface == CTRL_INTERFACE_PWM) {
                cJSON *pin = cJSON_GetObjectItem(config, "pin");
                cJSON *freq = cJSON_GetObjectItem(config, "freq");
                cJSON *resolution = cJSON_GetObjectItem(config, "resolution");
                cJSON *min_duty = cJSON_GetObjectItem(config, "min_duty");
                cJSON *max_duty = cJSON_GetObjectItem(config, "max_duty");
                
                if (pin) dev->config.pwm.pin = pin->valueint;
                if (freq) dev->config.pwm.freq = freq->valueint;
                if (resolution) dev->config.pwm.resolution = resolution->valueint;
                if (min_duty) dev->config.pwm.min_duty = min_duty->valueint;
                if (max_duty) dev->config.pwm.max_duty = max_duty->valueint;

                // here to init pwm

            } else if (dev->interface == CTRL_INTERFACE_I2C) {
                cJSON *addr = cJSON_GetObjectItem(config, "addr");
                cJSON *port = cJSON_GetObjectItem(config, "port");
                cJSON *scl = cJSON_GetObjectItem(config, "pin_scl");
                cJSON *sda = cJSON_GetObjectItem(config, "pin_sda");
                // cJSON *reg_ctrl = cJSON_GetObjectItem(config, "reg_ctrl");

                if (addr) dev->config.i2c.addr = addr->valueint;
                if (port) dev->config.i2c.port = (i2c_port_t)port->valueint;
                if (scl) dev->config.i2c.scl = scl->valueint;
                if (sda) dev->config.i2c.sda = sda->valueint;

                // here to init i2c

            } else if (dev->interface == CTRL_INTERFACE_UART) {
                cJSON *port = cJSON_GetObjectItem(config, "port");
                cJSON *rx = cJSON_GetObjectItem(config, "pin_rx");
                cJSON *tx = cJSON_GetObjectItem(config, "pin_tx");

                if (port) dev->config.uart.port = (uart_port_t)port->valueint;
                if (rx) dev->config.uart.rx = rx->valueint;
                if (tx) dev->config.uart.tx = tx->valueint;

                // here to init uart

            } else if (dev->interface == CTRL_INTERFACE_LED_STRIP) {
                cJSON *pin = cJSON_GetObjectItem(config, "pin");

                if (pin) dev->config.led_strip.pin = pin->valueint;

                led_strip_config_t strip_config = {
                    .strip_gpio_num = dev->config.led_strip.pin,
                    .max_leds = 1,
                };
                led_strip_rmt_config_t rmt_config = {
                    .resolution_hz = 10 * 1000000, // 10MHz
                    .flags.with_dma = false,
                };
                ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &dev->config.led_strip.led_strip));

            // } else if (dev->interface == CTRL_INTERFACE_ONEWIRE) {

            }
        }
        
        dev->enabled = true;
        dev->is_on = false;  // 默认关闭状态
        
        s_device_count++;
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d devices", s_device_count);
    return ESP_OK;
}

/**
 * @brief 根据设备名称查找设备
 */
static device_info_t *device_list_find_by_name(const char *name)
{
    if (!name || s_device_count == 0) {
        return NULL;
    }
    
    for (int i = 0; i < s_device_count; i++) {
        if (strcmp(s_device_list[i].name, name) == 0) {
            return &s_device_list[i];
        }
    }
    
    // 尝试模糊匹配（忽略空格和大小写）
    for (int i = 0; i < s_device_count; i++) {
        if (strcasestr(s_device_list[i].name, name) != NULL ||
            strcasestr(name, s_device_list[i].name) != NULL) {
            return &s_device_list[i];
        }
    }
    
    return NULL;
}

// ==================== 底层硬件控制函数 ====================

/**
 * @brief GPIO控制
 */
static esp_err_t gpio_control(int pin, bool state, bool active_low)
{
    // 实际电平计算
    int level = state ? 1 : 0;
    if (active_low) {
        level = !level;
    }
    
    ESP_LOGI(TAG, "GPIO控制: pin=%d, 输出=%s, 实际电平=%s", 
             pin, 
             state ? "HIGH" : "LOW",
             level ? "HIGH" : "LOW");
    
    // 调用实际的硬件驱动
    return gpio_set_level(pin, level);
}

/**
 * @brief PWM控制
 */
static esp_err_t pwm_control(int pin, int duty, int freq)
{
    ESP_LOGI(TAG, "PWM控制: pin=%d, duty=%d, freq=%d", pin, duty, freq);
    
    // TODO: 调用实际的硬件驱动
    // ledc_set_duty_and_frequency(...);
    
    return ESP_OK;
}

/**
 * @brief LED_STRIP控制
 */
static esp_err_t led_strip_control(int pin, led_strip_handle_t led_strip, bool state, int color)
{
    ESP_LOGI(TAG, "LED_STRIP控制: pin=%d, state=%d, color=%d", pin, state, color);
    if (false == state)
        led_strip_clear(led_strip);
    else {
        // hex_color == 0xRRGGBB;
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        led_strip_set_pixel(led_strip, 0, r, g, b);
    }
    
    return ESP_OK;
}

// ==================== 主要工具函数 ====================

/**
 * @brief 设备控制工具的执行函数
 * @param input_json 输入的JSON字符串，格式：{"device_name":"卧室灯","action":"on","value":50}
 * @param output 输出缓冲区，用于返回执行结果
 * @param output_size 输出缓冲区大小
 * @return esp_err_t 执行结果
 * 
 * 输入JSON格式说明：
 * {
 *   "device_name": "卧室灯",           // 必填：设备名称
 *   "action": "on",                    // 必填：on 或 off
 *   "value": 80                        // 可选：对于可调设备（如风扇转速、灯光亮度），0-100的百分比
 * }
 */
esp_err_t tool_device_ctl_execute(const char *input_json, char *output, size_t output_size)
{
    // 参数检查
    if (input_json == NULL || output == NULL || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "input json: %s", input_json);
    
    // 初始化设备列表（如果还没初始化）
    esp_err_t ret = device_list_init();
    if (ret != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"Failed to initialize device list\"}");
        return ret;
    }
    
    // 解析输入JSON
    cJSON *root = cJSON_Parse(input_json);
    if (root == NULL) {
        snprintf(output, output_size, "{\"error\":\"Invalid JSON format\"}");
        return ESP_ERR_INVALID_ARG;
    }
    
    // 获取必要参数
    cJSON *device_name = cJSON_GetObjectItem(root, "device_name");
    if (!cJSON_IsString(device_name)) device_name = cJSON_GetObjectItem(root, "设备名称");
    cJSON *action = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action)) device_name = cJSON_GetObjectItem(root, "状态");
    cJSON *value = cJSON_GetObjectItem(root, "value");
    
    // 验证必填字段
    if (!cJSON_IsString(device_name) || device_name->valuestring == NULL) {
        snprintf(output, output_size, "{\"error\":\"Missing or invalid device_name\"}");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (!cJSON_IsString(action) || action->valuestring == NULL) {
        snprintf(output, output_size, "{\"error\":\"Missing or invalid action\"}");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 查找设备
    device_info_t *dev = device_list_find_by_name(device_name->valuestring);
    if (dev == NULL) {
        // 设备未找到，返回可用设备列表供LLM参考
        char available[512] = {0};
        strcat(available, "可用设备: ");
        for (int i = 0; i < s_device_count; i++) {
            if (i > 0) strcat(available, ", ");
            strcat(available, s_device_list[i].name);
        }
        
        snprintf(output, output_size, 
                "{\"error\":\"Device '%s' not found\", \"available_devices\":\"%s\"}",
                device_name->valuestring, available);
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    
    if (!dev->enabled) {
        snprintf(output, output_size, "{\"error\":\"Device '%s' is disabled\"}", dev->name);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }
    
    // 解析动作
    bool target_state = false;
    if (strcasecmp(action->valuestring, "打开") == 0 ||
        strcasecmp(action->valuestring, "open") == 0 ||
        strcasecmp(action->valuestring, "on") == 0 || 
        strcasecmp(action->valuestring, "1") == 0 ||
        strcasecmp(action->valuestring, "true") == 0 ||
        strcasecmp(action->valuestring, "high") == 0) {
        target_state = true;
    } else if (strcasecmp(action->valuestring, "关闭") == 0 ||
        strcasecmp(action->valuestring, "close") == 0 ||
        strcasecmp(action->valuestring, "off") == 0 || 
        strcasecmp(action->valuestring, "0") == 0 ||
        strcasecmp(action->valuestring, "false") == 0 ||
        strcasecmp(action->valuestring, "low") == 0) {
        target_state = false;
    } else {
        snprintf(output, output_size, "{\"error\":\"Invalid action '%s'. Use 'on' or 'off'\"}", 
                 action->valuestring);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 执行控制
    esp_err_t result = ESP_OK;
    char result_msg[128] = {0};
    
    switch (dev->interface) {
        case CTRL_INTERFACE_GPIO: {
            result = gpio_control(dev->config.gpio.pin, 
                                  target_state, 
                                  dev->config.gpio.active_low);
            if (result == ESP_OK) {
                dev->is_on = target_state;
                snprintf(result_msg, sizeof(result_msg), 
                        "已%s %s (GPIO%d)", 
                        target_state ? "打开" : "关闭",
                        dev->name,
                        dev->config.gpio.pin);
                ESP_LOGI(TAG, "已%s %s (GPIO%d)", target_state ? "打开" : "关闭", dev->name, dev->config.gpio.pin);
            } else {
                ESP_LOGW(TAG, "无法%s %s (GPIO%d)", target_state ? "打开" : "关闭", dev->name, dev->config.gpio.pin);
            }
            break;
        }
        
        case CTRL_INTERFACE_PWM: {
            int duty;
            if (value && cJSON_IsNumber(value)) {
                // 用户指定了具体数值（0-100的百分比）
                int percent = value->valueint;
                if (percent < 0) percent = 0;
                if (percent > 100) percent = 100;
                
                // 计算占空比
                duty = dev->config.pwm.min_duty + 
                       (percent * (dev->config.pwm.max_duty - dev->config.pwm.min_duty)) / 100;
                
                // 如果是关闭动作，强制占空比为最小值
                if (!target_state) {
                    duty = dev->config.pwm.min_duty;
                }
            } else {
                // 根据开关状态决定占空比
                duty = target_state ? dev->config.pwm.max_duty : dev->config.pwm.min_duty;
            }
            
            result = pwm_control(dev->config.pwm.pin, duty, dev->config.pwm.freq);
            if (result == ESP_OK) {
                dev->is_on = (duty > dev->config.pwm.min_duty);
                int percent = ((duty - dev->config.pwm.min_duty) * 100) / 
                             (dev->config.pwm.max_duty - dev->config.pwm.min_duty);
                
                snprintf(result_msg, sizeof(result_msg), 
                        "已%s %s (PWM%d, 速度/亮度: %d%%)", 
                        dev->is_on ? "打开" : "关闭",
                        dev->name,
                        dev->config.pwm.pin,
                        percent);
            }
            break;
        }
        
        case CTRL_INTERFACE_I2C: {
            
        }

        case CTRL_INTERFACE_UART: {

        }

        case CTRL_INTERFACE_LED_STRIP: {
            int color = 0;
            if (target_state && value && cJSON_IsNumber(value)) {
                color = value->valueint;
            } else if (target_state) {
                color = 0xFFFFFF;
            }

            result = led_strip_control(dev->config.led_strip.pin, dev->config.led_strip.led_strip, target_state, color);
            if (result == ESP_OK) {
                dev->is_on = target_state;
                dev->value = color;
                snprintf(result_msg, sizeof(result_msg), "已将 %s 设置为 %X (GPIO%d)", dev->name, (unsigned int)dev->value, dev->config.led_strip.pin);
                ESP_LOGI(TAG, "已将 %s 设置为 %X (GPIO%d)", dev->name, dev->value, dev->config.led_strip.pin);
            } else {
                ESP_LOGW(TAG, "无法设置 %s (GPIO%d)", dev->name, dev->config.led_strip.pin);
            }
            break;
        }

        // case CTRL_INTERFACE_ONEWIRE: {

        // }
        
        default:
            snprintf(output, output_size, 
                    "{\"error\":\"Unsupported interface type for device '%s'\"}", 
                    dev->name);
            cJSON_Delete(root);
            return ESP_ERR_NOT_SUPPORTED;
    }
    
    // 构建返回结果
    if (result == ESP_OK) {
        snprintf(output, output_size, 
                "{\"success\":true, \"message\":\"%s\", \"device\":\"%s\", \"state\":%s}",
                result_msg,
                dev->name,
                dev->is_on ? "true" : "false");
    } else {
        snprintf(output, output_size, 
                "{\"error\":\"Failed to control device '%s'\", \"device\":\"%s\"}",
                dev->name,
                dev->name);
    }
    
    cJSON_Delete(root);
    return result;
}

/**
 * @brief 列出所有设备的工具函数（辅助工具）
 */
esp_err_t tool_list_devices_execute(const char *input_json, char *output, size_t output_size)
{
    // 初始化设备列表
    esp_err_t ret = device_list_init();
    if (ret != ESP_OK) {
        snprintf(output, output_size, "{\"error\":\"Failed to initialize device list\"}");
        return ret;
    }
    
    // 构建设备列表JSON
    cJSON *root = cJSON_CreateObject();
    cJSON *devices_array = cJSON_AddArrayToObject(root, "devices");
    
    for (int i = 0; i < s_device_count; i++) {
        device_info_t *dev = &s_device_list[i];
        cJSON *dev_obj = cJSON_CreateObject();
        
        cJSON_AddStringToObject(dev_obj, "name", dev->name);
        cJSON_AddStringToObject(dev_obj, "room", dev->room);
        // cJSON_AddStringToObject(dev_obj, "type", 
        //                         dev->type == DEVICE_TYPE_LIGHT ? "light" :
        //                         dev->type == DEVICE_TYPE_FAN ? "fan" : "motor");
        cJSON_AddStringToObject(dev_obj, "type", dev->type);
        cJSON_AddBoolToObject(dev_obj, "state", dev->is_on);
        cJSON_AddStringToObject(dev_obj, "description", dev->description);
        
        cJSON_AddItemToArray(devices_array, dev_obj);
    }
    
    cJSON_AddNumberToObject(root, "count", s_device_count);
    
    char *json_str = cJSON_PrintUnformatted(root);
    strlcpy(output, json_str, output_size);
    
    free(json_str);
    cJSON_Delete(root);
    
    return ESP_OK;
}