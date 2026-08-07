/**
 * @file board.h
 * @brief This is board file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2022 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __BOARD_H__
#define __BOARD_H__

#ifdef __cplusplus
extern "C"
{
#endif
#include "tuya_cloud_types.h"
#include "app_config.h"
#include "tal_log.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
#ifndef BOARD_ENABLE_LOG
#define BOARD_ENABLE_LOG                        (0)
#endif

#if (BOARD_ENABLE_LOG)
    #define TY_PRINTF(fmt, ...)                 TAL_PR_INFO(fmt, ##__VA_ARGS__)
    #define TY_HEXDUMP(title, buf, size)        TAL_PR_HEXDUMP_INFO(title, buf, size)
#else
    #define TY_PRINTF(fmt, ...)
    #define TY_HEXDUMP(title, buf, size)
#endif

//RAM
#ifndef BOARD_HEAP_SIZE
#define BOARD_HEAP_SIZE                         (1024*5)
#endif

#ifndef BOARD_UART_TX_BUF_SIZE
#define BOARD_UART_TX_BUF_SIZE                  (512)
#endif

// Flash
#ifndef BOARD_FLASH_SDK_TEST_START_ADDR
#define BOARD_FLASH_SDK_TEST_START_ADDR        (0x2C000)
#endif

// Tuya Auth Info: (0x40000 + BOARD_FLASH_OTA_SIZE + 0x1000)
#ifndef BOARD_FLASH_TUYA_INFO_START_ADDR
#define BOARD_FLASH_TUYA_INFO_START_ADDR        (0x6C000)
#endif

// OTA
extern unsigned int ota_program_offset;
// Used For OTA: (BOARD_FLASH_TUYA_INFO_START_ADDR + 0x4000)
#ifndef BOARD_FLASH_OTA_INFO_ADDR
#define BOARD_FLASH_OTA_INFO_ADDR               (0x70000)
#endif

#ifndef BOARD_FLASH_OTA_SIZE
#define BOARD_FLASH_OTA_SIZE                    (0x2B000)
#endif

#ifndef BOARD_FLASH_OTA_START_ADDR
#define BOARD_FLASH_OTA_START_ADDR              (ota_program_offset)
#endif

#ifndef BOARD_FLASH_OTA_END_ADDR
#define BOARD_FLASH_OTA_END_ADDR                (ota_program_offset + BOARD_FLASH_OTA_SIZE)
#endif

#if BOARD_FLASH_OTA_SIZE > 0x3F000
#error "BOARD_FLASH_OTA_SIZE must <= 0x3F000 !!!!"
#endif

// #ifndef BOARD_FLASH_MAC_START_ADDR
// #define BOARD_FLASH_MAC_START_ADDR              (0)
// #endif
#define APP_DATA_FLASH_ADDR 0x7F000
#define LASER_BUG 0
#define CAT_STRING 0
#define BLE_BALL 0
#define FAQIUJI 1

#if LASER_BUG
#ifndef BOARD_KEY_PIN
#define BOARD_KEY_PIN                           (TUYA_GPIO_NUM_28) // D4 按键

#endif

#if (BOARD_ENABLE_LOG)
#ifndef BOARD_LOG_TX_PIN
#define BOARD_LOG_TX_PIN                        (TUYA_GPIO_NUM_14) // B6 日志输出引脚

#endif
#endif


#define AD_Bat_CON      TUYA_GPIO_NUM_17 // C1 电量ADC控制开关 高电平开 低电平关
#define LED_G           TUYA_GPIO_NUM_15 // B7 状态指示灯 绿色 高电平开 低电平关
#define LED_B           TUYA_GPIO_NUM_9  // B1 状态指示灯 蓝色 高电平开 低电平关
#define LED_R           TUYA_GPIO_NUM_1  // A1 状态指示灯 红色 高电平开 低电平关
#define USB_DET         TUYA_GPIO_NUM_0  // A0 USB插入检测（低电平=USB插入）
#define PIR             TUYA_GPIO_NUM_16 // C0 PIR指示 低电平有效
#define PIR_CON         TUYA_GPIO_NUM_31 // D7 PIR开关 高电平打开 低电平关闭
#define LASER           TUYA_GPIO_NUM_27 // D3 激光指示灯 高电平打开 低电平关闭

#define AD_Bat 8   // PC4

#define MOTOR_REV_1       TUYA_PWM_NUM_5 // B5 电机反转 PWM_OUTPUT
#define MOTOR_FOR_1       TUYA_PWM_NUM_4 // B4 电机正转 PWM_OUTPUT
#define MOTOR_REV_2       TUYA_PWM_NUM_3 // D2 电机反转 PWM_OUTPUT
#define MOTOR_FOR_2       TUYA_PWM_NUM_1 // C3 电机正转 PWM_OUTPUT
#define MOTOR_3           TUYA_PWM_NUM_0 // C2 直流电机 无正反转

#endif

#if CAT_STRING
// PIN
// #ifndef BOARD_POWER_ON_PIN
// #define BOARD_POWER_ON_PIN                      (TUYA_GPIO_NUM_31) // D7 无用 测试引脚
// #endif

#ifndef BOARD_KEY_PIN
#define BOARD_KEY_PIN                           (TUYA_GPIO_NUM_16) // C0 按键

#endif

#if (BOARD_ENABLE_LOG)
#ifndef BOARD_LOG_TX_PIN
#define BOARD_LOG_TX_PIN                        (TUYA_GPIO_NUM_20) // C4 日志输出引脚

#endif
#endif


#define CHARGE_G        TUYA_GPIO_NUM_31 // D3 充电指示灯 绿色
#define CHARGE_R        TUYA_GPIO_NUM_27 // D7 充电指示灯 红色
#define USB_DET         TUYA_GPIO_NUM_14 // B6 USB插入检测（低电平=USB插入）
#define LED_B           TUYA_GPIO_NUM_1  // A0 状态指示灯 蓝色
#define LED_G           TUYA_GPIO_NUM_0  // A1 状态指示灯 绿色
#define AD_Bat_CON      TUYA_GPIO_NUM_26 // D2 电量ADC控制开关 高电平开 低电平关
// 以下两个在调试阶段暂时禁用
// #define Set_Charg_I     TUYA_GPIO_NUM_9  // B1 充电电流限流开关 充电中且当电机运行时打拉高，电机停止时拉低
// #define AD_NTC_CON      TUYA_GPIO_NUM_15 // B7 电池NTC控制开关 高电平开 低电平关
#define CHARGE_STATE    TUYA_GPIO_NUM_17 // C1 充电状态输入检测 低电平充电中高电平满电
#define CHARGE_EN       TUYA_GPIO_NUM_28 // D4 充电芯片 高电平开 低电平关

#define AD_Bat 4   // PB4
#define AD_Motor 5 // PB5
// #define AD_NTC 5  // PC4


#define MOTOR_REV       TUYA_PWM_NUM_0 // C2 电机反转 PWM_OUTPUT
#define MOTOR_FOR       TUYA_PWM_NUM_1 // C3 电机正转 PWM_OUTPUT

#if (!BOARD_ENABLE_LOG)
#define AD_NTC 8   // PC4

#endif
#endif

#if BLE_BALL
// PIN
#ifndef BOARD_POWER_ON_PIN
#define BOARD_POWER_ON_PIN                      (TUYA_GPIO_NUM_31) // D7 无用 测试引脚

#endif

#ifndef BOARD_KEY_PIN
#define BOARD_KEY_PIN                           (TUYA_GPIO_NUM_17) // C1 按键

#endif

#ifndef BOARD_LOG_TX_PIN
#define BOARD_LOG_TX_PIN                        (TUYA_GPIO_NUM_16) // C0 日志输出引脚

#endif


#define USB_DET TUYA_GPIO_NUM_27 // D3 USB插入检测（高电平=USB插入）
#define CHARGE_STATE TUYA_GPIO_NUM_1 // A1 充电状态输入（上拉，充电中=低，满电/未充电=高）
#define AD_BAT  TUYA_GPIO_NUM_14 // B6 电池电量AD采样引脚
#define AD_BAT_SWITCH TUYA_GPIO_NUM_0 //A0 电池电量AD采样开关，打开后AD_BAT引脚不能再当做普通GPIO使用
#define LED_R TUYA_GPIO_NUM_19 // C3
#define LED_G TUYA_GPIO_NUM_18 // C2
#define LED_B TUYA_GPIO_NUM_26 // D2
#define M_INA TUYA_GPIO_NUM_12 // B4
#define M_INB TUYA_GPIO_NUM_13 // B5
#define VIBRATION_SENSOR_PIN TUYA_GPIO_NUM_28 // D4

#endif

#if FAQIUJI
// PIN
// #ifndef BOARD_POWER_ON_PIN
// #define BOARD_POWER_ON_PIN                      (TUYA_GPIO_NUM_31) // D7 无用 测试引脚
// #endif

#ifndef BOARD_KEY_PIN
#define BOARD_KEY_PIN                           (TUYA_GPIO_NUM_16) // C0 按键

#endif

#if (BOARD_ENABLE_LOG)
#ifndef BOARD_LOG_TX_PIN
#define BOARD_LOG_TX_PIN                        (TUYA_GPIO_NUM_17) // C1 调试日志输出引脚

#endif
#endif

// 音频文件保存到外部FLASH
#define NOR_FLASH_CS    TUYA_GPIO_NUM_26 // PD2
#define NOR_FLASH_CCLK  TUYA_GPIO_NUM_31 // PD7
#define NOR_FLASH_SDI   TUYA_GPIO_NUM_14 // PB6
#define NOR_FLASH_SDO   TUYA_GPIO_NUM_15 // PB7

#define SLAVE_MCU_TX TUYA_GPIO_NUM_9 //PB1 与控制外设的MCU通信
#define SLAVE_MCU_RX TUYA_GPIO_NUM_0 //PA0 与控制外设的MCU通信

#define ADC_AUDIO_IN  TUYA_GPIO_NUM_20 // PC4 ADC采样音频信号
#define PWM_AUDIO_OUT TUYA_GPIO_NUM_18 // PC2 PWM输出音频信号

#endif

// IRQ NUM
#ifndef BOARD_GPIO_IRQ_NUM
#define BOARD_GPIO_IRQ_NUM                      (2)
#else
#if BOARD_GPIO_IRQ_NUM < 1
#error "BOARD_GPIO_IRQ_NUM must >= 1 !!!!"
#endif
#endif

// LOG
#ifndef TUYA_BLE_LOG_ENABLE
#define TUYA_BLE_LOG_ENABLE                     (0)
#endif

#ifndef TUYA_APP_LOG_ENABLE
#define TUYA_APP_LOG_ENABLE                     ENABLE_LOG
#endif

#ifndef BOARD_LOG_BAUD_RATE
#define BOARD_LOG_BAUD_RATE                     (115200)
#endif

#ifndef ENABLE_SCAN
#define ENABLE_SCAN (0) 
#endif
#define TKL_BLUETOOTH_SUPPORT_SCAN      ENABLE_SCAN

#ifndef TKL_BLUETOOTH_PHY_2M
#define TKL_BLUETOOTH_PHY_2M    (0)
#endif

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/
typedef enum {
    TY_BLE_STA_IDLE = 0,
    TY_BLE_STA_ADV,
    TY_BLE_STA_CONN,
} TKL_BOARD_BLE_STATUS_E;

typedef enum {
    TKL_PM_CLEAR = 0,
    TKL_PM_SET,
} TKL_PM_STATE_E;

typedef enum {
    PM_EVENT_LED = 0,
    PM_EVENT_KEY,
    PM_EVENT_BUZZER,
    PM_EVENT_UART,
    PM_EVENT_PAIRING,
    PM_EVENT_OTA,
} TKL_PM_EVENT_E;

/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
// 这个函数可以判定设备是处于连接状态还是广播状态,或者都不是,方便低功耗的设计
extern TKL_BOARD_BLE_STATUS_E tkl_ble_state;
extern UINT32_T tkl_board_state_suspend;
extern UINT32_T tkl_board_state_working;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/
/**
 * @brief connect parameters update result callback register
 *     callback fucntion with 2 input param:
 *      first: id , default 0, user don't need care
 *      second: result, result = 0x0000 means CONN_PARAM_UPDATE_ACCEPT
 *                      result = 0x0001 means CONN_PARAM_UPDATE_REJECT,
 *
 * @param[in] param: OPERATE_RET(*func)(UINT8_T, UINT16_T)
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
extern OPERATE_RET tkl_ble_conn_param_update_rsp_cb_register(OPERATE_RET(*func)(UINT8_T, UINT16_T));

/**
 * @brief telink irq handler callback. user can define private process function, such as FLD_IRQ_PWM0_IR_DMA_FIFO_DONE
 *        the function of callback must descript as _attribute_ram_code_

 * @param[in] param: VOID_T(*func)(VOID_T)
 *
 * @return VOID
 */
extern VOID_T tkl_board_irq_callback_register(VOID_T(*func)(VOID_T));

/**
 * @brief check rf fifo status

 * @param[in] param: VOID_T(*func)(VOID_T)
 *
 * @return VOID
 */
extern UINT32_T tkl_ble_trxfifo_not_empty(VOID_T);

/**
 * @brief To set system only support suspend mode, will not enter deep sleep. this priority is lower than tkl_board_pm_working
 *        only all the event bit be cleared, system can enter deep sleep. It usually be used operation which need cooperated hardware, like GPIO
 * @param[in] param:
 *              state: TKL_PM_CLEAR: clear the bit of event
 *                     TKL_PM_SET: set the bit of event
 *              event: event type, 0 - 4 are defined by board, others for user. event range: [0 - 31]
 * @return VOID
 */
extern VOID_T tkl_board_pm_suspend(TKL_PM_STATE_E state, TKL_PM_EVENT_E event);

/**
 * @brief To set system do not enter any type of sleep mode. this priority is higher than tkl_board_pm_suspend
 *        only all the event bit be cleared, system can enter sleep
 * @param[in] param:
 *              state: TKL_PM_CLEAR: clear the bit of event
 *                     TKL_PM_SET: set the bit of event
 *              event: event type, 0 - 4 are defined by board, others for user. event range: [0 - 31]
 * @return VOID
 */
extern VOID_T tkl_board_pm_working(TKL_PM_STATE_E state, TKL_PM_EVENT_E event);

/**
 * @brief mapping TuyaOS Pin ID To TLSR825x GPIO Pin ID
 * @param[in] pin_id:
 *              TuyaOS pin ID
 * @return TLSR825x GPIO pin ID
 */
UINT32_T tkl_gpio_to_tlsr_gpio(TUYA_GPIO_NUM_E pin_id);


#ifdef __cplusplus
}
#endif

#endif /* __BOARD_H__ */
