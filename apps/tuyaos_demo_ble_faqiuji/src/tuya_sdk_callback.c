/**
 * @file tuya_sdk_callback.c
 * @brief This is tuya_sdk_callback file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */


#include "string.h"

#include "board.h"

#include "tkl_wakeup.h"

#include "tal_log.h"
#include "tal_sleep.h"
#include "tal_sw_timer.h"
#include "tal_rtc.h"
#include "tal_watchdog.h"
#include "tal_gpio.h"
#include "tal_flash.h"
#include "tal_i2c.h"
#include "tal_bluetooth.h"
#include "tal_oled.h"
#include "tal_sdk_test.h"

#include "tuya_ble_api.h"
#include "tuya_ble_ota.h"
#include "tuya_ble_attach_ota.h"
#include "tuya_ble_main.h"
#include "tuya_sdk_callback.h"
#include "tuya_ble_protocol_callback.h"
#include "tuya_ble_mutli_tsf_protocol.h"
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
#include "tuya_ble_bulkdata_demo.h"
#endif

#include "app_config.h"
#include "app_dp_parser.h"
#include "faqiuji_mcu_protocol.h"
#include "faqiuji_audio.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
#define  TUYA_BLE_BULKDATA_BLOCK_SIZE 512
#endif

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/


/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
UINT16_T tal_app_server_conn_handle = 0xFFFF;

/* DP定时上报（向APP周期性通知） */
STATIC TIMER_ID s_dp_report_timer_id = NULL;

/* 设备状态DP定时上报（向APP周期性通知 work_state） */
STATIC TIMER_ID s_work_state_report_timer_id = NULL;

TAL_UART_CFG_T tal_uart_cfg = {
    .rx_buffer_size = 256,
    .open_mode = O_BLOCK,
    {
        .baudrate = 9600,
        .parity = TUYA_UART_PARITY_TYPE_NONE,
        .databits = TUYA_UART_DATA_LEN_8BIT,
        .stopbits = TUYA_UART_STOP_LEN_1BIT,
        .flowctrl = TUYA_UART_FLOWCTRL_NONE,
    }
};

TAL_BLE_ADV_PARAMS_T tal_adv_param = {
    .adv_interval_min = TY_ADV_INTERVAL*8/5,
    .adv_interval_max = TY_ADV_INTERVAL*8/5,
    .adv_type = TAL_BLE_ADV_TYPE_CS_UNDIR,
};

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
STATIC VOID_T tuya_pre_sleep_cb(VOID_T);
STATIC VOID_T tuya_post_wakeup_cb(VOID_T);
STATIC TUYA_SLEEP_CB_T tal_sleep_cb = {
    .pre_sleep_cb = tuya_pre_sleep_cb,
    .post_wakeup_cb = tuya_post_wakeup_cb,
};

STATIC TUYA_BLE_BULKDATA_EXTERNAL_PARAM_T tuya_ble_external_param = {
    .type = 1,
    .flag = NEED_PARSING_BY_APP,
};
STATIC TUYA_BLE_BULKDATA_CB_T tuya_ble_bulkdata_cb = {0};
#endif/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/***********************************************************************
 ********************* static callback wrappers ************************
 **********************************************************************/

/***********************************************************************
 ********************* DP 定时上报 **************************************
 **********************************************************************/

/* DP定时上报：读取缓存值，仅上报有变更的DP */
STATIC VOID_T dp_report_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
}

STATIC VOID_T tuya_ble_evt_callback(TAL_BLE_EVT_PARAMS_T *p_event)
{
    switch (p_event->type) {
        case TAL_BLE_STACK_INIT: {
            TAL_PR_INFO("TAL_BLE_STACK_INIT_SUCCESS");
        } break;

        case TAL_BLE_EVT_PERIPHERAL_CONNECT: {
            TAL_PR_INFO("connected");
            tal_app_server_conn_handle = p_event->ble_event.connect.peer.conn_handle;

            tuya_ble_connected_handler();

        } break;

        case TAL_BLE_EVT_CENTRAL_CONNECT_DISCOVERY: {
            TAL_PR_INFO("TAL_BLE_EVT_CENTRAL_CONNECT_DISCOVERY");
        } break;

        case TAL_BLE_EVT_DISCONNECT: {
            TAL_PR_INFO("disconnect: 0x%02x", p_event->ble_event.disconnect.reason);

            tuya_ble_disconnected_handler();

#if defined(TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE) && (TUYA_BLE_FEATURE_ATTACH_OTA_ENABLE == 1)
            tuya_ble_attach_ota_disconn_handler();
#endif

#if defined(TUYA_BLE_FEATURE_OTA_ENABLE) && (TUYA_BLE_FEATURE_OTA_ENABLE == 1)
            if (tuya_ble_ota_disconn_handler() == 1) {
                tal_ble_advertising_start(&tal_adv_param);
            }
#else
            tal_ble_advertising_start(&tal_adv_param);
#endif
            tal_app_server_conn_handle = 0xFFFF;

        } break;

        case TAL_BLE_EVT_ADV_REPORT: {
#if defined(TUYA_BLE_FEATURE_PRODUCT_TEST_ENABLE) && (TUYA_BLE_FEATURE_PRODUCT_TEST_ENABLE == 1)
            extern VOID_T tuya_ble_prod_beacon_handler(VOID_T* buf);
            tuya_ble_prod_beacon_handler(&p_event->ble_event.adv_report);
#endif
//            TAL_PR_INFO("TAL_BLE_EVT_ADV_REPORT");
        } break;

        case TAL_BLE_EVT_CONN_PARAM_REQ: {
            TAL_PR_INFO("TAL_BLE_EVT_CONN_PARAM_REQ");
            // Accepting parameters requested by peer.
            TAL_BLE_PEER_INFO_T peer_info = {0};
            peer_info.conn_handle = p_event->ble_event.conn_param.conn_handle;
            tal_ble_conn_param_update(peer_info, &p_event->ble_event.conn_param.conn);
        } break;

        case TAL_BLE_EVT_CONN_PARAM_UPDATE: {
            TAL_PR_INFO("conn param update: min-%dms, max-%dms, latency-%d, timeout-%dms", \
                (UINT16_T)(p_event->ble_event.conn_param.conn.min_conn_interval*1.25),     \
                (UINT16_T)(p_event->ble_event.conn_param.conn.max_conn_interval*1.25),     \
                (UINT16_T)(p_event->ble_event.conn_param.conn.latency),              \
                (UINT16_T)(p_event->ble_event.conn_param.conn.conn_sup_timeout*10) );
        } break;

        case TAL_BLE_EVT_CONN_RSSI: {
            TAL_PR_INFO("TAL_BLE_EVT_CONN_RSSI");
        } break;

        case TAL_BLE_EVT_MTU_REQUEST: {
            TAL_PR_INFO("mtu is set to 0x%X(%d)", p_event->ble_event.exchange_mtu.mtu, p_event->ble_event.exchange_mtu.mtu);
        } break;

        case TAL_BLE_EVT_MTU_RSP: {
            TAL_PR_INFO("TAL_BLE_EVT_MTU_RSP");
        } break;

        case TAL_BLE_EVT_NOTIFY_TX: {
//            TAL_PR_INFO("TAL_BLE_EVT_NOTIFY_TX");
        } break;

        case TAL_BLE_EVT_WRITE_REQ: {
            tuya_ble_gatt_receive_data(p_event->ble_event.data_report.report.p_data, p_event->ble_event.data_report.report.len);

//            TAL_BLE_DATA_T tal_data = {0};
//            tal_data.len = p_event->ble_event.data_report.report.len;
//            tal_data.p_data = p_event->ble_event.data_report.report.p_data;
//            tal_ble_server_common_send(&tal_data);
//            TAL_PR_HEXDUMP_INFO("RX", p_event->ble_event.data_report.report.p_data, p_event->ble_event.data_report.report.len);
        } break;

        case TAL_BLE_EVT_READ_RX: {
            TAL_PR_INFO("TAL_BLE_EVT_READ_RX");
        } break;

        default: {
        } break;
    }

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    tal_sdk_test_ble_evt_callback(p_event);
#endif
}

#if defined(ENABLE_LOG) && (ENABLE_LOG == 1)

STATIC VOID_T tuya_log_output_cb(IN CONST CHAR_T *str)
{
    VOID_T tkl_system_log_output(CONST UINT8_T *buf, UINT32_T size);
    tkl_system_log_output((VOID_T*)str, strlen((VOID_T*)str));
}

#endif

#define SHOUQUAN 0

STATIC VOID_T tuya_uart_irq_rx_cb(TUYA_UART_NUM_E port_id, VOID_T *buff, UINT16_T len)
{
    if (port_id == TUYA_UART_NUM_0) {
        #if !SHOUQUAN
        faqiuji_mcu_protocol_input(buff, len);
        #else
        UINT8_T* TEST = (UINT8_T*)buff;
        for (size_t i = 0; i < len; i++)
        {
            tal_log_print_raw("%2x", TEST[i]);
            /* code */
        }
        tal_log_print_raw("\r\n");
        tuya_ble_common_uart_receive_data(buff, len);
        #endif
    } else {
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
        test_cmd_send(TEST_ID_GET(TEST_GID_UART, TEST_CID_RX_UART_PORT), (VOID_T*)&port_id, SIZEOF(UINT32_T));
        test_cmd_send(TEST_ID_GET(TEST_GID_UART, TEST_CID_RX_UART_DATA), buff, len);
#endif
    }
}

STATIC VOID_T faqiuji_mcu_frame_cb(CONST FAQIUJI_MCU_FRAME_T *frame)
{
    UINT8_T event_type;
    UINT8_T value;
    UINT16_T value16;
    UINT8_T work_state;

    if (frame == NULL || frame->len == 0) {
        return;
    }

    TAL_PR_INFO("MCU FRAME: cmd=0x%02x seq=%u len=%u",
                frame->cmd, frame->seq, frame->len);

    if (frame->cmd == FAQIUJI_MCU_CMD_KEY_EVENT) {
        value = frame->payload[0] ? 1U : 0U;
        TAL_PR_INFO("MCU KEY: %d", value);
        return;
    }

    if (frame->cmd != FAQIUJI_MCU_CMD_STATUS_EVENT) {
        return;
    }

    event_type = frame->payload[0];
    value = frame->len > 1U ? frame->payload[1] : 0U;
    switch (event_type) {
        case FAQIUJI_MCU_EVENT_USB:
            TAL_PR_INFO("MCU USB: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_CHARGE:
            TAL_PR_INFO("MCU CHARGE: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_BATTERY:
            TAL_PR_INFO("MCU BATTERY: %d%%", value);
            app_dp_report(DP_ID_BATTERY, &value, 1U);
            break;
        case FAQIUJI_MCU_EVENT_BALL:
            TAL_PR_INFO("MCU BALL: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_RADAR:
            TAL_PR_INFO("MCU RADAR: %d", value);
            break;
        case FAQIUJI_MCU_EVENT_TEMPERATURE:
            value16 = frame->len >= 3U ?
                      ((UINT16_T)frame->payload[1] |
                       ((UINT16_T)frame->payload[2] << 8U)) : 0U;
            TAL_PR_INFO("MCU NTC RAW: %u", value16);
            break;
        case FAQIUJI_MCU_EVENT_WORK:
            work_state = value;
            TAL_PR_INFO("MCU WORK STATE: %d", work_state);
            app_dp_report(DP_ID_WORK_STATE, &work_state, 1U);
            app_dp_set_work_state(work_state);
            break;
        case FAQIUJI_MCU_EVENT_COUNT:
            value16 = frame->len >= 3U ?
                      ((UINT16_T)frame->payload[1] |
                       ((UINT16_T)frame->payload[2] << 8U)) : 0U;
            TAL_PR_INFO("MCU LAUNCH COUNT: %u", value16);
            break;
        default:
            TAL_PR_WARN("MCU EVENT UNKNOWN: %u", event_type);
            break;
    }
}

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)

STATIC VOID_T tuya_pre_sleep_cb(VOID_T)
{
}

STATIC VOID_T tuya_post_wakeup_cb(VOID_T)
{
}

VOID_T tuya_ble_bulkdata_info_cb(TUYA_BLE_BULKDATA_INFO_T* info)
{
//    info->total_length = 0;
//    info->total_crc32 = 0;
    info->block_length = TUYA_BLE_BULKDATA_BLOCK_SIZE;
}

VOID_T tuya_ble_bulkdata_report_cb(UINT8_T* p_block_buf, UINT32_T block_length, UINT32_T block_number)
{
    UINT32_T read_addr = BOARD_FLASH_SDK_TEST_START_ADDR + block_number*TUYA_BLE_BULKDATA_BLOCK_SIZE;
    tuya_ble_nv_read(read_addr, p_block_buf, block_length);
}

#endif

STATIC VOID_T faqiuji_board_io_init(VOID_T)
{
    TUYA_GPIO_BASE_CFG_T high_cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_HIGH,
    };
    TUYA_GPIO_BASE_CFG_T low_cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };

    /* Keep the external flash writable and audio amplifier disabled at boot. */
    tal_gpio_init(NOR_FLASH_HOLD, &high_cfg);
    tal_gpio_init(NOR_FLASH_WP, &high_cfg);
    tal_gpio_init(SPK_CTRL, &high_cfg);
    tal_gpio_init(SPK_POWER_CON, &high_cfg);
    tal_gpio_init(LED_G, &low_cfg);
    // tal_gpio_init(LED_B, &low_cfg);
}

OPERATE_RET app_config_info_set(VOID_T)
{
    tal_common_info_t tal_common_info   = {0};
    tal_common_info.p_firmware_name     = (UINT8_T*)FIRMWARE_NAME;
    tal_common_info.p_firmware_version  = (UINT8_T*)FIRMWARE_VERSION;
    tal_common_info.firmware_version    = FIRMWARE_VERSION_HEX;
    tal_common_info.p_hardware_version  = (UINT8_T*)HARDWARE_VERSION;
    tal_common_info.hardware_version    = HARDWARE_VERSION_HEX;
    tal_common_info.p_sdk_version       = (UINT8_T*)"0.2.0";
    tal_common_info.p_kernel_version    = (UINT8_T*)"0.0.1";
    return tal_common_info_init(&tal_common_info);
}

OPERATE_RET tuya_init_first(VOID_T)
{
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
// 测试上电时序，上电后先打开一个GPIO输出高电平
    TUYA_GPIO_BASE_CFG_T gpio_cfg = {
        .mode = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level = TUYA_GPIO_LEVEL_LOW,
    };
    tal_gpio_init(BOARD_POWER_ON_PIN, &gpio_cfg);
    tal_gpio_write(BOARD_POWER_ON_PIN, TUYA_GPIO_LEVEL_HIGH);
#endif

    extern VOID_T tuya_memory_init(VOID_T);
    tuya_memory_init();

    app_config_info_set();

    tal_rtc_init();

    return OPRT_OK;
}

OPERATE_RET tuya_init_second(VOID_T)
{
#if defined(ENABLE_LOG) && (ENABLE_LOG == 1)
    tal_log_create_manage_and_init(TAL_LOG_LEVEL_DEBUG, 1024, tuya_log_output_cb);
#endif

    tal_sw_timer_init();

    tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, tuya_ble_evt_callback);

    return OPRT_OK;
}

OPERATE_RET tuya_init_third(VOID_T)
{
    // GPIO 外设初始化
    faqiuji_board_io_init();
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    // TUYA_IIC_BASE_CFG_T iic_cfg = {
    //     .role = TUYA_IIC_MODE_MASTER,
    //     .speed = TUYA_IIC_BUS_SPEED_400K,
    //     .addr_width = TUYA_IIC_ADDRESS_7BIT,
    // };
    // tal_i2c_init(TUYA_I2C_NUM_0, &iic_cfg);
#endif

    OPERATE_RET ret = faqiuji_audio_init();
    if (ret != OPRT_OK)
    {
        TAL_PR_DEBUG("AUDIO INIT ERR %d",ret);
    }
    
    return ret;
}

OPERATE_RET tuya_init_last(VOID_T)
{
    // tkl_uart.c中可以指定IO
    tal_uart_init(TUYA_UART_NUM_0, &tal_uart_cfg);

    tuya_ble_protocol_init();
    faqiuji_mcu_protocol_init(faqiuji_mcu_frame_cb);

    tal_uart_rx_reg_irq_cb(TUYA_UART_NUM_0, tuya_uart_irq_rx_cb);

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    tal_sdk_test_init();
#endif

    tal_ble_advertising_start(&tal_adv_param);

    /* ---- DP 定时上报定时器 ---- */
    tal_sw_timer_create(dp_report_timeout_handler, NULL, &s_dp_report_timer_id);
    tal_sw_timer_start(s_dp_report_timer_id, 1000, TAL_TIMER_CYCLE);

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    // if (tal_oled_init() == OPRT_OK) {
    //     tal_oled_show_string(12, 1, (VOID_T*)"TuyaOS Demo", 16);
    // }

    tal_cpu_sleep_callback_register(&tal_sleep_cb);

    tuya_ble_bulkdata_cb.info_cb = tuya_ble_bulkdata_info_cb;
    tuya_ble_bulkdata_cb.report_cb = tuya_ble_bulkdata_report_cb;
    tuya_ble_bulk_data_init(&tuya_ble_external_param, &tuya_ble_bulkdata_cb);

    // app_led_timer_start();
#if defined(APP_PRODUCT_TEST) && (APP_PRODUCT_TEST == 1)
    app_product_test_init();
#endif // APP_PRODUCT_TEST
#endif
    
    return OPRT_OK;
}

OPERATE_RET tuya_main_loop(VOID_T)
{
#if !TUYA_BLE_USE_OS
    tuya_ble_main_tasks_exec();
#endif
    faqiuji_audio_task();
    app_dp_process_audio_events();
//    tal_watchdog_refresh();
    return (tuya_ble_sleep_allowed_check() == TRUE);
}

UINT16_T tuya_app_get_conn_handle(VOID_T)
{
    return tal_app_server_conn_handle;
}
