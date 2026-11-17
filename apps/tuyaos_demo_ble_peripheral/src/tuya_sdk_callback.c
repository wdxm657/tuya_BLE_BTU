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
#include "tal_adc.h"
#include "tal_sleep.h"
#include "tal_system.h"
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

/* BLE 每次连接后连续强制全量上报的次数 */
#define DP_CONNECT_FORCE_REPORT_COUNT  2U
STATIC UINT8_T s_dp_force_report_count = 0;

/* 每60秒计数一次，累计15次（15分钟）后进入超低功耗。 */
#define LOW_POWER_DELAY_MS             (60UL * 1000UL)
#define RETENTION_LOW_POWER_COUNT_MAX  15U
STATIC BOOL_T   s_low_power_entered = FALSE;
STATIC BOOL_T   s_undervoltage_shutdown_active = FALSE;

/* Retention sleep 持续15分钟后停止BLE活动，进入超低功耗。 */
STATIC TIMER_ID s_retention_timer_id = NULL;
STATIC volatile UINT8_T s_retention_low_power_count = 0;
STATIC BOOL_T s_retention_tracking = FALSE;
STATIC BOOL_T s_retention_ultra_low_power = FALSE;
STATIC volatile BOOL_T s_retention_check_pending = FALSE;
STATIC volatile BOOL_T s_retention_hw_wakeup_pending = FALSE;
STATIC BOOL_T s_retention_key_press_seen = FALSE;

extern BOOL_T tkl_cpu_is_sleep(VOID_T);

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

STATIC VOID_T tuya_pre_sleep_cb(VOID_T);
STATIC VOID_T tuya_post_wakeup_cb(VOID_T);
STATIC VOID_T tuya_retention_timer_handler(TIMER_ID timer_id, VOID_T *arg);
STATIC VOID_T tuya_retention_sleep_process(VOID_T);
STATIC VOID_T app_peripherals_resume_after_sleep(VOID_T);
STATIC VOID_T usb_on_removed(VOID_T);
STATIC TUYA_SLEEP_CB_T tal_sleep_cb = {
    .pre_sleep_cb = tuya_pre_sleep_cb,
    .post_wakeup_cb = tuya_post_wakeup_cb,
};

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
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
 * @brief 状态变更处理：刷新 LED，并根据 work/standby 状态启停电机
 */
STATIC VOID_T device_on_state_change(dev_state_t old_state, dev_state_t new_state)
{
    app_led_update();

    /* 任意模式工作满 5 分钟进入待机后，都必须停止电机。 */
    if (new_state == DEV_STATE_STANDBY) {
        app_motor_stop();
    } else if (old_state == DEV_STATE_STANDBY &&
               new_state == DEV_STATE_WORK &&
               app_state_is_powered_on()) {
        /* 从待机恢复工作时，按当前模式重新启动电机。 */
        app_motor_start();
    }
}

/**
 * @brief 振动处理：待机唤醒并重启5分钟工作周期；工作中触发互动模式
 */
STATIC VOID_T vibration_on_detected(VOID_T)
{
    if (!app_state_is_powered_on() || app_state_is_charging()) {
        return;
    }

    if (app_state_get() == DEV_STATE_STANDBY) {
        TAL_PR_INFO("[state] vibration wakeup -> restart 5-minute work cycle");
        app_state_reset_standby_timer();
        return;
    }

    /* 非待机状态下，保留互动模式原有的振动触发逻辑。 */
    app_motor_interactive_trigger();
}

/**
 * @brief 执行低功耗前的统一硬件处理
 */
STATIC VOID_T enter_low_power_now(VOID_T)
{
    if (s_low_power_entered) {
        return;
    }

    s_low_power_entered = TRUE;
    app_battery_save_percent();
    {
        TUYA_GPIO_BASE_CFG_T gpio_cfg = {
            .mode   = TUYA_GPIO_PULLDOWN,
            .direct = TUYA_GPIO_INPUT,
            .level  = TUYA_GPIO_LEVEL_LOW,
        };
        tal_gpio_init(AD_BAT_SWITCH, &gpio_cfg);
    }
    tal_adc_deinit(TUYA_ADC_NUM_0);
    tal_cpu_allow_sleep();
}

/**
 * @brief 临界低电 → 硬件关机（适配 app_battery_register_critical_cb 签名）
 */
STATIC VOID_T battery_critical_poweroff(VOID_T)
{
    TAL_BLE_PEER_INFO_T peer = {0};

    if (app_state_is_charging() || s_low_power_entered ||
        s_undervoltage_shutdown_active) {
        return;
    }

    s_undervoltage_shutdown_active = TRUE;

    /* 欠压时停止产品工作。 */
    app_state_set_power(FALSE);

    if (tal_app_server_conn_handle != 0xFFFF) {
        peer.conn_handle = tal_app_server_conn_handle;
        tal_ble_disconnect(peer);
    }

    /* 红灯快闪两次，完成后立即进入低功耗。 */
    app_led_red_flash_twice(enter_low_power_now);
}

/***********************************************************************
 ********************* DP 定时上报 **************************************
 **********************************************************************/

/* DP定时上报：状态变化时上报；BLE连接后连续5次强制全量上报 */
STATIC VOID_T dp_report_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    BOOL_T force_report;

    app_led_update();

    if (tal_app_server_conn_handle == 0xFFFF) {
        return;
    }

    force_report = (s_dp_force_report_count > 0U);

    static UINT8_T s_last_battery = 0xFF;
    static UINT8_T s_last_switch  = 0xFF;
    static UINT8_T s_last_mode    = 0xFF;
    static UINT8_T s_last_work_state = 0xFF;

    UINT8_T buf[DT_VALUE_LEN] = {0};

    /* 电池电量 */
    {
        UINT8_T percent = app_battery_get_percent();
        if (force_report || percent != s_last_battery) {
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
        if (force_report || power_on != s_last_switch) {
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
        if (force_report || mode != s_last_mode) {
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
        if (force_report || state != s_last_work_state) {
            s_last_work_state = state;
            app_dp_report(DP_ID_WORK_STATE, &state, DT_ENUM_LEN);
            TAL_PR_DEBUG("[dp] work_state report: %d", state);
        }
    }

    if (force_report) {
        s_dp_force_report_count--;
        TAL_PR_DEBUG("[dp] connection full report, remaining=%d",
                     s_dp_force_report_count);
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
            s_dp_force_report_count = DP_CONNECT_FORCE_REPORT_COUNT;
            TAL_PR_INFO("[dp] schedule %d full reports after connection",
                        DP_CONNECT_FORCE_REPORT_COUNT);

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
                app_battery_save_percent();
                tal_ble_advertising_start(&tal_adv_param);
            }
#else
            tal_ble_advertising_start(&tal_adv_param);
#endif
            tal_app_server_conn_handle = 0xFFFF;
            s_dp_force_report_count = 0;

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

STATIC VOID_T tuya_pre_sleep_cb(VOID_T)
{
    OPERATE_RET ret;

    app_battery_suspend();
    app_led_prepare_sleep();
    app_key_suspend();
    app_charge_suspend();
    app_vibration_suspend();

    if (s_retention_tracking || s_retention_ultra_low_power) {
        return;
    }

    if (s_retention_timer_id == NULL) {
        ret = tal_sw_timer_create(tuya_retention_timer_handler, NULL,
                                  &s_retention_timer_id);
        if (ret != OPRT_OK) {
            TAL_PR_ERR("[retention] timer create failed: %d", ret);
            return;
        }
    }

    s_retention_low_power_count = 0;
    s_retention_check_pending = FALSE;
    ret = tal_sw_timer_start(s_retention_timer_id,
                             LOW_POWER_DELAY_MS,
                             TAL_TIMER_CYCLE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[retention] timer start failed: %d", ret);
        return;
    }
    s_retention_tracking = TRUE;
}

STATIC VOID_T tuya_post_wakeup_cb(VOID_T)
{
    /* 只记录硬件唤醒；不能在此退出 sleep 或恢复轮询定时器。 */
    s_retention_hw_wakeup_pending = TRUE;
}

STATIC VOID_T tuya_retention_timer_handler(TIMER_ID timer_id, VOID_T *arg)
{
    /* 饱和计数，达到阈值后不再递增，避免长时间运行溢出。 */
    if (s_retention_low_power_count < RETENTION_LOW_POWER_COUNT_MAX) {
        s_retention_low_power_count++;
    }
    s_retention_check_pending = TRUE;
}

STATIC VOID_T tuya_retention_sleep_process(VOID_T)
{
    OPERATE_RET ret;

    if (!tkl_cpu_is_sleep()) {
        if (s_retention_tracking && s_retention_timer_id != NULL) {
            tal_sw_timer_stop(s_retention_timer_id);
        }
        s_retention_tracking = FALSE;
        s_retention_ultra_low_power = FALSE;
        s_retention_check_pending = FALSE;
        s_retention_low_power_count = 0;
        return;
    }

    if (!s_retention_tracking || !s_retention_check_pending) {
        return;
    }
    s_retention_check_pending = FALSE;

    TAL_PR_INFO("[retention] low-power count: %d/%d",
                s_retention_low_power_count,
                RETENTION_LOW_POWER_COUNT_MAX);
    if (s_retention_low_power_count < RETENTION_LOW_POWER_COUNT_MAX) {
        return;
    }

    if (tal_app_server_conn_handle != 0xFFFF) {
        tuya_ble_gap_disconnect();
        /* 等待断开事件处理完成，避免其自动重启广播后覆盖下面的停止操作。 */
        tal_system_sleep(1000);
        TAL_PR_INFO("[retention] BLE disconnected after 15 minutes");
    }

    ret = tal_ble_advertising_stop();
    if (ret != OPRT_OK) {
        TAL_PR_WARN("[retention] stop advertising failed: %d", ret);
        return;
    }

    tal_sw_timer_stop(s_retention_timer_id);
    s_retention_tracking = FALSE;
    s_retention_ultra_low_power = TRUE;
    TAL_PR_INFO("[retention] enter ultra-low-power after 15 minutes");
}

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
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

    /* third 阶段仅初始化业务 GPIO，定时器和业务状态统一放到 last。 */
    TUYA_GPIO_BASE_CFG_T gpio_out_high = {
        .mode   = TUYA_GPIO_PUSH_PULL,
        .direct = TUYA_GPIO_OUTPUT,
        .level  = TUYA_GPIO_LEVEL_LOW,
    };
    tal_gpio_init(AD_BAT_SWITCH, &gpio_out_high);
    app_battery_io_init();
    app_motor_io_init();
    app_key_io_init();
    app_led_io_init();
    app_charge_io_init();
    app_vibration_io_init();
    
    return OPRT_OK;
}

VOID_T power_off_cb(VOID_T){
    /* 这里只停止负载；低功耗由主循环在条件持续120秒后统一处理。 */
    app_motor_stop();
    // 关机则直接进入retention
    app_battery_save_percent();
    tal_cpu_allow_sleep();
    TAL_PR_DEBUG("[power] motor stopped, waiting for low-power conditions");
}

STATIC VOID_T app_peripherals_resume_after_sleep(VOID_T)
{
    app_key_resume();
    app_charge_resume();
    app_vibration_resume();
    app_battery_resume();
    app_led_resume();
}

STATIC VOID_T usb_on_removed(VOID_T)
{
    if (app_state_is_powered_on()) {
        return;
    }

    TAL_PR_INFO("[power] USB removed while powered off -> enter retention");
    app_battery_save_percent();
    tal_cpu_allow_sleep();
}

VOID_T power_on_cb(VOID_T){
    /* 明确开机后退出 sleep，并恢复业务轮询。 */
    tal_cpu_force_wakeup();
    app_peripherals_resume_after_sleep();

    /* 欠压低功耗被按键唤醒时，不允许电机启动。 */
    if (app_battery_is_critical() && !app_state_is_charging()) {
        TAL_PR_WARN("[power] power-on rejected: battery undervoltage");
        s_undervoltage_shutdown_active = FALSE;
        s_low_power_entered = FALSE;
        battery_critical_poweroff();
        return;
    }

    app_motor_start();
}

OPERATE_RET tuya_init_last(VOID_T)
{
    /* ---- 业务模块初始化（third 阶段只初始化 GPIO） ---- */
    /* 状态机须先于会读写设备状态的其他模块初始化。 */
    app_state_init();
    app_key_init();
    app_led_init();
    app_battery_init();
    app_charge_init();
    app_motor_init();
    app_vibration_init();

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

    /* 振动 → 待机唤醒；工作中用于互动模式触发 */
    app_vibration_register_cb(vibration_on_detected);

    /* 设备状态变更 → LED 刷新 + 工作/待机时电机启停 */
    app_state_register_change_cb(device_on_state_change);

    /* 电源状态 → 电机启停（统一开关机，与物理按键/蓝牙 DP_SWITCH 一致） */
    app_state_register_power_cb(power_on_cb, power_off_cb);

    /* 临界低电 → 硬件关机保护 */
    app_battery_register_critical_cb(battery_critical_poweroff);
    app_charge_register_usb_removed_cb(usb_on_removed);
    tal_cpu_sleep_callback_register(&tal_sleep_cb);

    /* 按键从欠压低功耗唤醒属于重新启动，首次采样后立即给出提示并休眠。 */
    if (app_battery_is_critical() && !app_state_is_charging()) {
        battery_critical_poweroff();
    }

#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    // if (tal_oled_init() == OPRT_OK) {
    //     tal_oled_show_string(12, 1, (VOID_T*)"TuyaOS Demo", 16);
    // }

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
        if (app_battery_is_critical() && !app_state_is_charging()) {
            TAL_PR_WARN("[boot] battery undervoltage, keep powered off");
        } else if (f_reset == 0x03) {
            TAL_PR_INFO("[boot] factory-reset flag detected, staying off");
        } else {
            // 启动默认开机
            app_state_set_power(1);
        }
    }
    
    return OPRT_OK;
}

OPERATE_RET tuya_main_loop(VOID_T)
{
    TUYA_GPIO_LEVEL_E key_level = TUYA_GPIO_LEVEL_HIGH;
    TUYA_GPIO_LEVEL_E usb_level = TUYA_GPIO_LEVEL_LOW;

#if !TUYA_BLE_USE_OS
    tuya_ble_main_tasks_exec();
#endif

    /*
     * Deep-retention 唤醒后先确认按键确实按下，再等待松开才开机。
     * 不使用软件定时器，避免在 retention 中制造周期唤醒。
     * USB 唤醒时按键始终为高，不会误触发开机。
     */
    if (s_retention_hw_wakeup_pending && tkl_cpu_is_sleep()) {
        /* USB 插入为高电平：立即退出 retention，但产品仍保持关机。 */
        if (tal_gpio_read(USB_DET, &usb_level) == OPRT_OK &&
            usb_level == TUYA_GPIO_LEVEL_HIGH) {
            s_retention_hw_wakeup_pending = FALSE;
            s_retention_key_press_seen = FALSE;
            TAL_PR_INFO("[power] USB inserted in retention -> wake up");
            tal_cpu_force_wakeup();
            app_peripherals_resume_after_sleep();
        } else if (tal_gpio_read(BOARD_KEY_PIN, &key_level) == OPRT_OK) {
            if (key_level == TUYA_GPIO_LEVEL_LOW) {
                s_retention_key_press_seen = TRUE;
            } else if (s_retention_key_press_seen) {
                s_retention_hw_wakeup_pending = FALSE;
                s_retention_key_press_seen = FALSE;
                TAL_PR_INFO("[power] retention key released -> power on");
                app_state_set_power(TRUE);
            }
        }
    }

    /* 状态机周期性处理（待机超时检测等） */
    app_state_process();

    /* Retention sleep 保持15分钟后关闭BLE广播，进入超低功耗。 */
    tuya_retention_sleep_process();

//    tal_watchdog_refresh();
    
    return (tuya_ble_sleep_allowed_check() == TRUE);
}

UINT16_T tuya_app_get_conn_handle(VOID_T)
{
    return tal_app_server_conn_handle;
}

