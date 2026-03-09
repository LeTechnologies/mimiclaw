#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

esp_err_t tool_device_ctl_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_list_devices_execute(const char *input_json, char *output, size_t output_size);