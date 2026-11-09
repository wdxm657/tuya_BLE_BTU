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
#include "app_product_test.h"
#include "app_key.h"
#include "app_led.h"
#include "app_dp_parser.h"
#include "app_state.h"
#include "app_motor.h"
#include "app_battery.h"
#include "app_charge.h"
#include "app_vibration.h"

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

/* 电池电量DP定时上报（向APP周期性通知） */
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

/**
 * @brief 状态变更 → LED 刷新（适配 app_state_register_change_cb 签名）
 */
STATIC VOID_T led_on_state_change(dev_state_t old_state, dev_state_t new_state)
{
    app_led_update();
}

/**
 * @brief 临界低电 → 硬件关机（适配 app_battery_register_critical_cb 签名）
 */
STATIC VOID_T battery_critical_poweroff(VOID_T)
{
    /* 硬件关机（进入低功耗保护） */
    app_state_set_power(FALSE);
}

/***********************************************************************
 ********************* DP 定时上报 **************************************
 **********************************************************************/

/* DP定时上报：读取缓存值，仅上报有变更的DP */
STATIC VOID_T dp_report_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    app_led_update();

    if(tal_app_server_conn_handle == 0XFFFF){
        return;
    }

    static UINT8_T s_last_battery = 0xFF;
    static UINT8_T s_last_switch  = 0xFF;
    static UINT8_T s_last_mode    = 0xFF;
    static UINT8_T s_last_work_state = 0xFF;

    UINT8_T buf[DT_VALUE_LEN] = {0};

    /* 电池电量 */
    {
        UINT8_T percent = app_battery_get_percent();
        if (percent != s_last_battery) {
            s_last_battery = percent;
            /* DT_VALUE: 4字节小端 int，第4字节为有效值 */
            buf[3] = percent;
            app_dp_report(DP_ID_BATTERY, buf, DT_VALUE_LEN);
            TAL_PR_DEBUG("[dp] battery report: %d%%", percent);
        }
    }

    /* 开关状态 */
    {
        UINT8_T power_on = app_state_is_powered_on() ? 1 : 0;
        if (power_on != s_last_switch) {
            s_last_switch = power_on;
            memset(buf, 0, DT_VALUE_LEN);
            buf[0] = power_on;
            app_dp_report(DP_ID_SWITCH, buf, DT_BOOL_LEN);
            TAL_PR_DEBUG("[dp] switch report: %d", power_on);
        }
    }

    /* 工作模式 */
    {
        UINT8_T mode = (UINT8_T)app_motor_get_mode();
        if (mode != s_last_mode) {
            s_last_mode = mode;
            memset(buf, 0, DT_VALUE_LEN);
            buf[0] = mode;
            app_dp_report(DP_ID_MODE, buf, DT_ENUM_LEN);
            TAL_PR_DEBUG("[dp] mode report: %d", mode);
        }
    }

    /* 工作状态 */
    {
        UINT8_T state = app_state_get_dp_enum();
        if (state != s_last_work_state) {
            s_last_work_state = state;
            app_dp_report(DP_ID_WORK_STATE, &state, DT_ENUM_LEN);
            TAL_PR_DEBUG("[dp] work_state report: %d", state);
        }
    }
}

STATIC VOID_T tuya_ble_evt_callback(TAL_BLE_EVT_PARAMS_T *p_event)
{
    switch (p_event->type) {
        case TAL_BLE_STACK_INIT: {
            TAL_PR_INFO("TAL_BLE_STACK_INIT_SUCCESS");
        } break;

        case TAL_BLE_EVT_PERIPHERAL_CONNECT: {
            TAL_PR_INFO("connected");
            // 清除配网标志位
            tal_flash_erase(APP_DATA_FLASH_ADDR, 0x1000);
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

            /* BLE 断开 且关机状态 → 进入低功耗 */
            if (!app_state_is_powered_on()) {
                app_state_set_power(FALSE);
            }
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

STATIC VOID_T tuya_uart_irq_rx_cb(TUYA_UART_NUM_E port_id, VOID_T *buff, UINT16_T len)
{
    if (port_id == TUYA_UART_NUM_0) {
        tuya_ble_common_uart_receive_data(buff, len);
    } else {
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
        test_cmd_send(TEST_ID_GET(TEST_GID_UART, TEST_CID_RX_UART_PORT), (VOID_T*)&port_id, SIZEOF(UINT32_T));
        test_cmd_send(TEST_ID_GET(TEST_GID_UART, TEST_CID_RX_UART_DATA), buff, len);
#endif
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
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    // TUYA_IIC_BASE_CFG_T iic_cfg = {
    //     .role = TUYA_IIC_MODE_MASTER,
    //     .speed = TUYA_IIC_BUS_SPEED_400K,
    //     .addr_width = TUYA_IIC_ADDRESS_7BIT,
    // };
    // tal_i2c_init(TUYA_I2C_NUM_0, &iic_cfg);
#endif

    // 增加按键IO按下时唤醒低功耗
    TUYA_WAKEUP_SOURCE_BASE_CFG_T wakeup_cfg = {
        .source = TUYA_WAKEUP_SOURCE_GPIO,
        .wakeup_para.gpio_param.gpio_num = BOARD_KEY_PIN,
        .wakeup_para.gpio_param.level = TUYA_GPIO_LEVEL_LOW,
    };
    tkl_wakeup_source_set(&wakeup_cfg);

    // 增加USB IO插入时唤醒低功耗
    TUYA_WAKEUP_SOURCE_BASE_CFG_T usb_wakeup_cfg = {
        .source = TUYA_WAKEUP_SOURCE_GPIO,
        .wakeup_para.gpio_param.gpio_num = USB_DET,
        .wakeup_para.gpio_param.level = TUYA_GPIO_LEVEL_HIGH,
    };
    tkl_wakeup_source_set(&usb_wakeup_cfg);

    /* ---- 模块初始化 ---- */
    /* 1. 状态机（须在其他模块之前初始化） */
    app_state_init();

    /* 2. 初始化 GPIO 外设引脚 */
    TUYA_GPIO_BASE_CFG_T gpio_out_high = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    tal_gpio_init(AD_BAT_SWITCH, &gpio_out_high);

    /* 3. M_INA/M_INB GPIO 初始化已移除 — 由 app_motor_init() 内的 PWM 接管 */

    /* 4. 按键（一直运行） */
    app_key_init();

    /* 5. LED 指示灯 */
    app_led_init();

    /* 6. 电池监测（ADC+定时器） */
    app_battery_init();

    /* 7. 充电检测（GPIO轮询） */
    app_charge_init();

    /* 8. 电机 (PWM 初始化) */
    app_motor_init();

    /* 9. 振动传感器 */
    app_vibration_init();

    return OPRT_OK;
}

VOID_T power_off_cb(VOID_T){
    //停止电机
    app_motor_stop();
    if(tal_app_server_conn_handle == 0XFFFF){
        // 蓝牙未连接则进入低功耗
        // 清除配网标志位
        tal_flash_erase(APP_DATA_FLASH_ADDR, 0x1000);
        // 关闭AD_BAT_SWITCH,把AD_BAT_SWITCH配置为输入下拉
        TUYA_GPIO_BASE_CFG_T gpio_cfg = {
            .mode   = TUYA_GPIO_PULLDOWN,
            .direct = TUYA_GPIO_INPUT,
            .level  = TUYA_GPIO_LEVEL_LOW,
        };
        /* 初始化两个 GPIO（不进中断，纯输入轮询） */
        tal_gpio_init(AD_BAT_SWITCH, &gpio_cfg);
        TAL_PR_DEBUG("AD_BAT_SWITCH TUYA_GPIO_PULLDOWN");
        tal_adc_deinit(TUYA_ADC_NUM_0);
        TAL_PR_DEBUG("INTO LOW POWER");
        tal_cpu_allow_sleep();
    }else{
        // 蓝牙连接保活，不需要操作
        TAL_PR_DEBUG("ONLY CLOSE MOTOR");
    }
}

VOID_T power_on_cb(VOID_T){
    app_motor_start();
}

OPERATE_RET tuya_init_last(VOID_T)
{
    // PB1 TX   PB7 RX
    tal_uart_init(TUYA_UART_NUM_0, &tal_uart_cfg);

    tuya_ble_protocol_init();

    tal_uart_rx_reg_irq_cb(TUYA_UART_NUM_0, tuya_uart_irq_rx_cb);

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    tal_sdk_test_init();
#endif

    tal_ble_advertising_start(&tal_adv_param);

    /* ---- DP 定时上报定时器 ---- */
    tal_sw_timer_create(dp_report_timeout_handler, NULL, &s_dp_report_timer_id);
    tal_sw_timer_start(s_dp_report_timer_id, 1000, TAL_TIMER_CYCLE);

    /* ---- 回调注册 ---- */

    /* 振动 → 互动模式触发 */
    app_vibration_register_cb(app_motor_interactive_trigger);

    /* 设备状态变更 → LED 刷新 */
    app_state_register_change_cb(led_on_state_change);

    /* 电源状态 → 电机启停（统一开关机，与物理按键/蓝牙 DP_SWITCH 一致） */
    app_state_register_power_cb(power_on_cb, power_off_cb);

    /* 临界低电 → 硬件关机保护 */
    // app_battery_register_critical_cb(battery_critical_poweroff);

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
    // 初始化结束后从FLASH中读取标志位
    // 若标志位为0x03则认为是长按3秒后的复位启动，此时不能开机
    {
        UINT8_T f_reset = 0xFF;
        tal_flash_read(APP_DATA_FLASH_ADDR, &f_reset, 1);
        if (f_reset == 0x03) {
            TAL_PR_INFO("[boot] factory-reset flag detected, staying off");
        } else {
            app_state_set_power(TRUE);
        }
    }
    
    return OPRT_OK;
}

OPERATE_RET tuya_main_loop(VOID_T)
{
#if !TUYA_BLE_USE_OS
    tuya_ble_main_tasks_exec();
#endif

    /* 状态机周期性处理（待机超时检测等） */
    app_state_process();

//    tal_watchdog_refresh();
    
    return (tuya_ble_sleep_allowed_check() == TRUE);
}

UINT16_T tuya_app_get_conn_handle(VOID_T)
{
    return tal_app_server_conn_handle;
}

