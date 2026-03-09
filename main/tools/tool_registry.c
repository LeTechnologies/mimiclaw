#include "tool_registry.h"
#include "mimi_config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
// #include "tools/tool_gpio_ctl.h"
#include "tools/tool_device_ctl.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 12

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " MIMI_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    // /* Register gpio_control */
    // static mimi_tool_t tool_gpio = {
    //     .name = "gpio_control",
    //     .description = "Control GPIO pins to output high or low levels. Use this to turn lights on/off, control relays, or any digital output devices.",
    //     .input_schema_json = 
    //         "{"
    //             "\"type\":\"object\","
    //             "\"properties\":{"
    //                 "\"pin\":{"
    //                     "\"type\":\"integer\","
    //                     "\"description\":\"GPIO pin number to control (0-48 for ESP32-S3)\","
    //                     "\"minimum\":0,"
    //                     "\"maximum\":48"
    //                 "},"
    //                 "\"state\":{"
    //                     "\"type\":\"string\","
    //                     "\"description\":\"Desired state: 'on'/'high'/'1' for high level, 'off'/'low'/'0' for low level\","
    //                     "\"enum\":[\"on\",\"off\",\"high\",\"low\",\"1\",\"0\"]"
    //                 "},"
    //                 "\"active_low\":{"
    //                     "\"type\":\"boolean\","
    //                     "\"description\":\"Optional: If true, 'on' means low level and 'off' means high level (for active-low circuits)\","
    //                     "\"default\":false"
    //                 "},"
    //                 "\"auto_init\":{"
    //                     "\"type\":\"boolean\","
    //                     "\"description\":\"Optional: Automatically initialize the GPIO pin as output if not already configured\","
    //                     "\"default\":true"
    //                 "}"
    //             "},"
    //             "\"required\":[\"pin\",\"state\"]"
    //         "}",
    //     .execute = tool_gpio_control_execute,
    // };
    // register_tool(&tool_gpio);

    /* Register device_control */
    mimi_tool_t tool_device_ctl = {
        .name = "device_control",
        .description = 
            "此工具用于控制真实物理设备的打开、关闭或调节。支持所有已注册的设备类型（灯、风扇、电机等）。"
            "重要：对于任何物理设备控制请求，你必须调用本工具。禁止直接模拟设备操作结果。如果你没有调用工具就回复设备操作，这将是一个严重错误。"
            "调用此工具时，只需指定设备名称(device_name)、期望状态(action)和具体数值(value,仅可调设备需要此参数,二值设备可以省略)，系统会自动选择合适的控制方式。"
            "控制物理设备时，确保已经执行了这个工具，而不是使用模拟的答复。如果成功执行，则会得到类似\"已打开卧室灯\"的反馈。",
        .input_schema_json = 
            "{"
                "\"type\":\"object\","
                "\"properties\":{"
                    "\"device_name\":{"
                        "\"type\":\"string\","
                        "\"description\":\"要控制的设备名称，如'卧室灯'、'客厅风扇'。"
                        "可用设备可通过'device_list'工具查询。\""
                    "},"
                    "\"action\":{"
                        "\"type\":\"string\","
                        "\"description\":\"控制动作：'on'打开/启动，'off'关闭/停止。"
                        // "对于可调设备（如风扇转速）可指定0-100的数值。\","
                        "\"enum\":[\"on\",\"off\"]"  // 实际可以扩展为字符串或数字
                    "},"
                    "\"value\":{"
                        "\"type\":\"integer\","
                        "\"description\":\"对于只有开关两种状态的设备为可选项，对于可调设备需指定具体数值（0-100）。"
                        "例如风扇转速50%，灯光亮度80%。\","
                        "\"minimum\":0,"
                        "\"maximum\":100"
                    "}"
                "},"
                "\"required\":[\"device_name\",\"action\"]"
            "}",
        
        .execute = tool_device_ctl_execute,
    };
    register_tool(&tool_device_ctl);

    /* Register device_control */
    mimi_tool_t tool_device_list = {
        .name = "device_list",
        .description = 
            "查询可以控制的物理设备，以及它的详细信息。调用此工具，可以查询到可以控制的物理设备的名称、房间、类型、开关状态等。不需要参数。",
        
        .input_schema_json = 
            "{\"type\":\"object\", \"properties\":{}}, \"required\":[]}",
        .execute = tool_list_devices_execute,
    };
    register_tool(&tool_device_list);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
