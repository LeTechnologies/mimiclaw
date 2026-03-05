#pragma once

#include "esp_err.h"

/**
 * @brief GPIO控制工具的执行函数
 * @param input_json 输入的JSON字符串，包含pin和state参数
 * @param output 输出缓冲区，用于返回执行结果
 * @param output_size 输出缓冲区大小
 * @return esp_err_t 执行状态
 */
esp_err_t tool_gpio_control_execute(const char *input_json, char *output, size_t output_size);

/**
 * @brief 初始化GPIO工具（如果需要提前配置）
 */
void tool_gpio_init(void);