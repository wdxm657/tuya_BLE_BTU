/**
 * @file app_battery.c
 * @brief 电池电量监测实现 — 连续 ADC 采样 + 2s 周期检测 + 临界低电关机保护
 * @version 1.0
 * @date 2026-06-15
 *
 * @copyright Copyright 2026 Tuya Inc. All Rights Reserved.
 *
 * 设计原则：
 *   1. AD_BAT_SWITCH 在初始化时开启后保持常开，不每次采样开关
 *   2. 内部 2 秒定时器周期性 ADC 采样 → 均值滤波 → 更新缓存
 *   3. 外部模块一律通过 Get 接口获取缓存值，不阻塞采样
 *   4. 临界低电（< 5%）自动触发深度睡眠，防止电池过放损坏
 *   5. 60s 的 app 上报由 tuya_sdk_callback 的独立定时器负责，只读缓存
 */

#include "string.h"

#include "tal_log.h"
#include "tal_adc.h"
#include "tal_flash.h"
#include "tal_sw_timer.h"
#include "tal_gpio.h"
#include "tal_system.h"
#include "board.h"

#include "app_battery.h"
#include "app_state.h"

/***********************************************************************
 ********************* static variable *********************************
 **********************************************************************/

/* ADC 初始化标志 */
STATIC BOOL_T s_adc_inited = FALSE;

/* 监测定时器 */
STATIC TIMER_ID s_monitor_timer_id = NULL;

/* 缓存值 */
STATIC INT32_T  s_cached_voltage = 0;     /**< 最近一次有效电压 mV */
STATIC UINT8_T  s_cached_percent = 50;    /**< 最近一次有效百分比 % */

/* 电量显示变化限速：插入 USB 只升，拔出 USB 只降 */
#define BATTERY_RISE_INTERVAL_MS   10 * 1000U /**< USB 插入时每 10 秒最多上升 1% */
#define BATTERY_DROP_INTERVAL_MS    5 * 1000U /**< USB 拔出时每 5 秒最多下降 1% */

STATIC BOOL_T   s_usb_state_inited = FALSE;
STATIC BOOL_T   s_last_usb_in      = FALSE;
STATIC UINT32_T s_last_percent_tick_ms = 0;

/* 电量存储地址使用 board.h 中的 USER_BATTERY_FLASH_ADDR。 */
#define BATTERY_FLASH_SECTOR_SIZE   0x1000U
#define BATTERY_FLASH_MAGIC_0       0x42U
#define BATTERY_FLASH_MAGIC_1       0xA7U
#define BATTERY_FLASH_DATA_SIZE     4U

/* 低电量/临界标志 */
STATIC BOOL_T   s_battery_low      = FALSE;
STATIC BOOL_T   s_battery_critical = FALSE;

/* 临界低电回调（由 tuya_sdk_callback 注册） */
STATIC VOID_T (*s_critical_cb)(VOID_T) = NULL;

/***********************************************************************
 ********************* static functions *********************************
 **********************************************************************/

/**
 * @brief 18650 电池电压 → 百分比查找表（mV 映射 %）
 *
 * 基于典型 18650 锂离子电池放电曲线建立，
 * 表中电压为开路电压近似值（带载时略有下降）。
 * 输入电压在两个表项之间时采用线性插值。
 */
STATIC CONST struct {
    INT32_T  voltage_mv;
    UINT8_T  percent;
} s_battery_lut[] = {
    {4200, 100},
    {4150,  95},
    {4110,  90},
    {4080,  85},
    {4040,  80},
    {4000,  75},
    {3960,  70},
    {3920,  65},
    {3880,  60},
    {3840,  55},
    {3800,  50},
    {3760,  45},
    {3720,  40},
    {3680,  35},
    {3640,  30},
    {3600,  25},
    {3560,  20},
    {3500,  15},
    {3400,  10},
    {3250,   5},
    {3000,   0},
};
#define BATTERY_LUT_SIZE  (sizeof(s_battery_lut) / sizeof(s_battery_lut[0]))

/**
 * @brief 电压 mV → 百分比（18650 查找表 + 线性插值）
 */
STATIC UINT8_T app_battery_voltage_to_percent(INT32_T vol_mv)
{
    UINT8_T i;

    /* 超出表上限 → 满电 */
    if (vol_mv >= s_battery_lut[0].voltage_mv) {
        return s_battery_lut[0].percent;
    }

    /* 超出表下限 → 亏电 */
    if (vol_mv <= s_battery_lut[BATTERY_LUT_SIZE - 1].voltage_mv) {
        return s_battery_lut[BATTERY_LUT_SIZE - 1].percent;
    }

    /* 查找所在区间，线性插值 */
    for (i = 0; i < BATTERY_LUT_SIZE - 1; i++) {
        if (vol_mv <= s_battery_lut[i].voltage_mv &&
            vol_mv >= s_battery_lut[i + 1].voltage_mv) {
            INT32_T v_high = s_battery_lut[i].voltage_mv;
            INT32_T v_low  = s_battery_lut[i + 1].voltage_mv;
            UINT8_T p_high = s_battery_lut[i].percent;
            UINT8_T p_low  = s_battery_lut[i + 1].percent;
            INT32_T v_range = v_high - v_low;
            INT32_T v_off   = vol_mv - v_low;

            if (v_range == 0) {
                return p_high;
            }
            return (UINT8_T)(p_low + (INT32_T)((INT32_T)(p_high - p_low) * v_off / v_range));
        }
    }

    return 0;
}

/**
 * @brief ADC 读取 + 均值滤波（内部使用，不做开关 AD_BAT_SWITCH）
 * @param[out] vol_mv 电压 mV
 * @return OPRT_OK 成功
 */
STATIC OPERATE_RET app_battery_sample(INT32_T *vol_mv)
{
    INT32_T  adc_raw = 0;
    INT32_T  raw_sum = 0;
    UINT8_T  valid_cnt = 0;
    UINT8_T  i;
    OPERATE_RET ret;

    if (vol_mv == NULL) {
        return OPRT_INVALID_PARM;
    }

    /* 多次 ADC 采样并累加（AD_BAT_SWITCH 已常开，无需等待稳定） */
    for (i = 0; i < BATTERY_ADC_SAMPLES; i++) {
        ret = tal_adc_read_voltage(TUYA_ADC_NUM_0, &adc_raw, 1);
        if (ret != OPRT_OK) {
            TAL_PR_WARN("[battery] sample %d failed: %d", i, ret);
            continue;
        }

        /* 丢弃负值（差分模式下异常读数） */
        if (adc_raw < 0) {
            TAL_PR_DEBUG("[battery] sample %d negative: %d, skipped", i, adc_raw);
            continue;
        }

        raw_sum += adc_raw;
        valid_cnt++;

        if (i < BATTERY_ADC_SAMPLES - 1) {
            tal_system_delay(BATTERY_SAMPLE_INTERVAL_MS);
        }
    }

    if (valid_cnt == 0) {
        TAL_PR_ERR("[battery] all %d samples invalid", BATTERY_ADC_SAMPLES);
        return OPRT_COM_ERROR;
    }

    /* 计算平均值 → 换算电压 */
    INT32_T adc_avg = raw_sum / (INT32_T)valid_cnt;
    *vol_mv = adc_avg * BATTERY_DIVIDER_RATIO;

    return OPRT_OK;
}

/**
 * @brief 从 Flash 恢复上次保存的显示电量
 * @param[out] percent 恢复出的电量百分比
 * @return OPRT_OK=记录有效，其他=无有效记录或读取失败
 */
STATIC OPERATE_RET app_battery_load_percent(UINT8_T *percent)
{
    UINT8_T data[BATTERY_FLASH_DATA_SIZE] = {0};
    OPERATE_RET ret;

    if (percent == NULL) {
        return OPRT_INVALID_PARM;
    }

    ret = tal_flash_read(USER_BATTERY_FLASH_ADDR, data, SIZEOF(data));
    if (ret != OPRT_OK) {
        TAL_PR_WARN("[battery] flash read failed: %d", ret);
        return ret;
    }

    if (data[0] != BATTERY_FLASH_MAGIC_0 ||
        data[1] != BATTERY_FLASH_MAGIC_1 ||
        data[2] > 100U ||
        data[3] != (UINT8_T)(data[2] ^ 0xFFU)) {
        return OPRT_COM_ERROR;
    }

    *percent = data[2];
    return OPRT_OK;
}

/**
 * @brief 按 USB 状态对显示电量做单向限速
 *
 * USB 插入：目标值较高时，每 10 秒上升 1%，目标值较低时保持不变。
 * USB 拔出：目标值较低时，每 5 秒下降 1%，目标值较高时保持不变。
 */
STATIC UINT8_T app_battery_limit_percent(UINT8_T target_percent)
{
    TUYA_GPIO_LEVEL_E usb_level = TUYA_GPIO_LEVEL_LOW;
    UINT32_T now_ms = tal_system_get_millisecond();
    UINT32_T interval_ms;
    BOOL_T usb_in;

    if (tal_gpio_read(USB_DET, &usb_level) != OPRT_OK) {
        TAL_PR_WARN("[battery] read USB_DET failed, keep %d%%", s_cached_percent);
        return s_cached_percent;
    }
    usb_in = (usb_level == TUYA_GPIO_LEVEL_HIGH);

    /* 首次读取或 USB 插拔状态变化后重新计时，防止电量立即跳变。 */
    if (!s_usb_state_inited || usb_in != s_last_usb_in) {
        s_usb_state_inited = TRUE;
        s_last_usb_in = usb_in;
        s_last_percent_tick_ms = now_ms;
        TAL_PR_INFO("[battery] USB %s, percent rate timer reset",
                    usb_in ? "inserted" : "removed");
        return s_cached_percent;
    }

    interval_ms = usb_in ? BATTERY_RISE_INTERVAL_MS : BATTERY_DROP_INTERVAL_MS;
    if ((UINT32_T)(now_ms - s_last_percent_tick_ms) < interval_ms) {
        return s_cached_percent;
    }

    if (usb_in && target_percent > s_cached_percent) {
        s_last_percent_tick_ms = now_ms;
        return (UINT8_T)(s_cached_percent + 1U);
    }

    if (!usb_in && target_percent < s_cached_percent) {
        s_last_percent_tick_ms = now_ms;
        return (UINT8_T)(s_cached_percent - 1U);
    }

    return s_cached_percent;
}

/**
 * @brief 监测定时器回调 — 每 1 秒执行一次
 *
 * 1. 采样电压
 * 2. 更新缓存
 * 3. 检查低电量 → 设置低电量标志
 * 4. 检查临界低电 → 触发深度睡眠保护
 */
STATIC VOID_T app_battery_monitor_handler(TIMER_ID timer_id, VOID_T *arg)
{
    INT32_T vol_mv = 0;
    UINT8_T target_percent;
    UINT8_T percent;

    /* 采样 */
    if (app_battery_sample(&vol_mv) != OPRT_OK) {
        return;  /* 采样失败，保持缓存值 */
    }

    /* 换算目标百分比，再根据 USB 状态进行单向限速。 */
    target_percent = app_battery_voltage_to_percent(vol_mv);
    percent = app_battery_limit_percent(target_percent);

    /* 更新缓存 */
    s_cached_voltage = vol_mv;
    if (s_cached_percent != percent) {
        TAL_PR_INFO("[battery] sample: %dmV, target=%d%%, display=%d%%",
                    vol_mv, target_percent, percent);
    }
    
    s_cached_percent = percent;


    /* ----- 低电量检测 ----- */
    if (percent <= BATTERY_LOW_THRESHOLD) {
        if (!s_battery_low) {
            s_battery_low = TRUE;
            TAL_PR_WARN("[battery] LOW: %d%%", percent);
        }
    } else {
        s_battery_low = FALSE;
    }

    /* ----- 临界低电保护 ----- */
    if (percent <= BATTERY_CRITICAL_THRESHOLD) {
        if (!s_battery_critical) {
            s_battery_critical = TRUE;
            TAL_PR_ERR("[battery] CRITICAL: %d%% — entering deep sleep!", percent);
        }

        /* 触发注册的临界低电回调（由 tuya_sdk_callback 注册关机逻辑） */
        if (s_critical_cb != NULL) {
            s_critical_cb();
        }
    } else {
        s_battery_critical = FALSE;
    }
}

/***********************************************************************
 ********************* public functions *********************************
 **********************************************************************/

OPERATE_RET app_battery_init(VOID_T)
{
    OPERATE_RET ret;
    UINT8_T saved_percent = 0;
    BOOL_T saved_percent_valid = FALSE;

    if (app_battery_load_percent(&saved_percent) == OPRT_OK) {
        s_cached_percent = saved_percent;
        saved_percent_valid = TRUE;
        TAL_PR_INFO("[battery] restored from flash: %d%%", saved_percent);
    }

    /* ---------- ADC 初始化 ---------- */
    TUYA_ADC_BASE_CFG_T adc_cfg = {
        .ch_nums       = 1,
        .ch_list.data  = (1 << BATTERY_ADC_CH),
        .width         = 14,
        .type          = TUYA_ADC_EXTERNAL_SAMPLE_VOL,
    };

    ret = tal_adc_init(TUYA_ADC_NUM_0, &adc_cfg);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[battery] ADC init failed: %d", ret);
        return ret;
    }

    /* ---------- 使能分压电路（常开，不再开关） ---------- */
    tal_gpio_write(AD_BAT_SWITCH, TUYA_GPIO_LEVEL_HIGH);
    tal_system_delay(10);  /* 等待电压稳定 */

    /* ---------- 首次采样初始化缓存 ---------- */
    INT32_T init_mv = 0;
    if (app_battery_sample(&init_mv) == OPRT_OK) {
        s_cached_voltage = init_mv;
        if (!saved_percent_valid) {
            s_cached_percent = app_battery_voltage_to_percent(init_mv);
        }
        s_last_percent_tick_ms = tal_system_get_millisecond();
        TAL_PR_INFO("[battery] init: %dmV, display=%d%%, adc=%d%%",
                    init_mv, s_cached_percent,
                    app_battery_voltage_to_percent(init_mv));
    } else {
        TAL_PR_WARN("[battery] init sample failed, use defaults");
    }

    /* ---------- 创建监测定时器（1 秒周期） ---------- */
    tal_sw_timer_create(app_battery_monitor_handler, NULL, &s_monitor_timer_id);
    tal_sw_timer_start(s_monitor_timer_id, BATTERY_MONITOR_INTERVAL_MS, TAL_TIMER_CYCLE);

    s_adc_inited = TRUE;
    TAL_PR_INFO("[battery] monitoring started, interval=%dms", BATTERY_MONITOR_INTERVAL_MS);
    return OPRT_OK;
}

OPERATE_RET app_battery_read_voltage(INT32_T *vol_mv)
{
    if (!s_adc_inited) {
        return OPRT_COM_ERROR;
    }
    /* 返回缓存值（非阻塞，不触发 ADC 采样） */
    *vol_mv = s_cached_voltage;
    return OPRT_OK;
}

UINT8_T app_battery_get_percent(VOID_T)
{
    return s_cached_percent;
}

OPERATE_RET app_battery_save_percent(VOID_T)
{
    UINT8_T old_data[BATTERY_FLASH_DATA_SIZE] = {0};
    UINT8_T data[BATTERY_FLASH_DATA_SIZE];
    OPERATE_RET ret;

    data[0] = BATTERY_FLASH_MAGIC_0;
    data[1] = BATTERY_FLASH_MAGIC_1;
    data[2] = s_cached_percent;
    data[3] = (UINT8_T)(s_cached_percent ^ 0xFFU);

    /* 相同记录不重复擦写，降低 Flash 磨损。 */
    if (tal_flash_read(USER_BATTERY_FLASH_ADDR, old_data, SIZEOF(old_data)) == OPRT_OK &&
        memcmp(old_data, data, SIZEOF(data)) == 0) {
        return OPRT_OK;
    }

    ret = tal_flash_erase(USER_BATTERY_FLASH_ADDR, BATTERY_FLASH_SECTOR_SIZE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[battery] flash erase failed: %d", ret);
        return ret;
    }

    ret = tal_flash_write(USER_BATTERY_FLASH_ADDR, data, SIZEOF(data));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[battery] flash write failed: %d", ret);
        return ret;
    }

    TAL_PR_INFO("[battery] saved to flash: %d%%", s_cached_percent);
    return OPRT_OK;
}

INT32_T app_battery_get_voltage(VOID_T)
{
    return s_cached_voltage;
}

BOOL_T app_battery_is_low(VOID_T)
{
    return s_battery_low;
}

BOOL_T app_battery_is_critical(VOID_T)
{
    return s_battery_critical;
}

VOID_T app_battery_register_critical_cb(VOID_T (*cb)(VOID_T))
{
    s_critical_cb = cb;
}
