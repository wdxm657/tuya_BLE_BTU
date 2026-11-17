/** @file app_dp_report.c
 * @brief BLE 连接后的定时 DP 上报
 */
#include "string.h"
#include "tal_log.h"
#include "tal_sw_timer.h"
#include "app_battery.h"
#include "app_state.h"
#include "app_motor.h"
#include "app_led.h"
#include "app_dp_parser.h"
#include "tuya_sdk_callback.h"
#include "tuya_ble_mutli_tsf_protocol.h"
#include "app_dp_report.h"

/* 电池电量DP定时上报（向APP周期性通知） */
STATIC TIMER_ID s_dp_report_timer_id = NULL;

/* BLE 每次连接后连续强制全量上报的次数 */
#define DP_CONNECT_FORCE_REPORT_COUNT  2U
STATIC UINT8_T s_dp_force_report_count = 0;

/* DP定时上报：状态变化时上报；BLE连接后连续5次强制全量上报 */
STATIC VOID_T dp_report_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    BOOL_T force_report;

    app_led_update();

    if (tuya_app_get_conn_handle() == 0xFFFF) {
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

VOID_T app_dp_report_init(VOID_T)
{
    /* ---- DP 定时上报定时器 ---- */
    tal_sw_timer_create(dp_report_timeout_handler, NULL, &s_dp_report_timer_id);
    tal_sw_timer_start(s_dp_report_timer_id, 1000, TAL_TIMER_CYCLE);

}

VOID_T app_dp_report_on_connected(VOID_T)
{
    s_dp_force_report_count = DP_CONNECT_FORCE_REPORT_COUNT;
    TAL_PR_INFO("[dp] schedule %d full reports after connection",
                DP_CONNECT_FORCE_REPORT_COUNT);
}

VOID_T app_dp_report_on_disconnected(VOID_T)
{
    s_dp_force_report_count = 0;
}
