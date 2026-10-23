/********************************************************************************************************
 * @file     app.c 
 *
 * @brief    for TLSR chips
 *
 * @author	 BLE Group
 * @date     Sep. 18, 2015
 *
 * @par      Copyright (c) Telink Semiconductor (Shanghai) Co., Ltd.
 *           All rights reserved.
 *           
 *			 The information contained herein is confidential and proprietary property of Telink 
 * 		     Semiconductor (Shanghai) Co., Ltd. and is available under the terms 
 *			 of Commercial License Agreement between Telink Semiconductor (Shanghai) 
 *			 Co., Ltd. and the licensee in separate contract or the terms described here-in. 
 *           This heading MUST NOT be removed from this file.
 *
 * 			 Licensees are granted free, non-transferable use of the information in this 
 *			 file under Mutual Non-Disclosure Agreement. NO WARRENTY of ANY KIND is provided.
 *           
 *******************************************************************************************************/
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "battery_check.h"
#include "vendor/common/blt_common.h"
#include "../common/blt_soft_timer.h"

#include "board.h"

#include "tkl_bluetooth.h"

#include "tal_system.h"
#include "tal_system.h"
#include "tal_bluetooth.h"
#include "tal_sdk_test.h"
#include "tal_rtc.h"
#include "tal_uart.h"
#include "tal_gpio.h"
#include "tal_log.h"

#include "tuya_ble_type.h"
#include "tuya_ble_api.h"
#include "tuya_ble_log.h"
#include "tuya_ble_api.h"
#include "telink_app_config.h"


extern OPERATE_RET tkl_init_first(VOID_T);
extern OPERATE_RET tkl_init_second(VOID_T);
extern OPERATE_RET tkl_init_third(VOID_T);
extern OPERATE_RET tkl_init_last(VOID_T);
extern OPERATE_RET tkl_main_loop(VOID_T);
extern BOOL_T tkl_cpu_is_sleep(VOID_T);

extern SYS_TIME_T tkl_sw_timer_next_tick;
extern TUYA_SLEEP_CB_T g_tkl_cpu_sleep_callback;

_attribute_ram_code_ VOID_T app_suspend_exit (VOID_T)
{
	bls_pm_setSuspendMask(SUSPEND_DISABLE);
}

_attribute_ram_code_ STATIC OPERATE_RET app_suspend_enter (VOID_T)
{
    if(!tkl_cpu_is_sleep()) {
        goto EXIT;
    }

    if((tkl_ble_trxfifo_not_empty())) {
        bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);
        return 1;
    }

    if(tkl_board_state_working) {
        goto EXIT;
    }

    if(g_tkl_cpu_sleep_callback.pre_sleep_cb) {
        g_tkl_cpu_sleep_callback.pre_sleep_cb();
    }
	return 1;
EXIT:
    app_suspend_exit ();
    return 0;
}

STATIC UINT8_T app_user_task_excute_finished_check(VOID_T)
{
#if !TUYA_BLE_USE_OS
    tuya_ble_main_tasks_exec();
#endif

    return (tuya_ble_sleep_allowed_check() == TRUE);
}
_attribute_ram_code_ STATIC VOID_T app_power_management (VOID_T)
{
#if (BLE_MODULE_PM_ENABLE)
    bls_pm_setSuspendMask(SUSPEND_DISABLE);

    if(tkl_board_state_working) {
        return;
    }

    if(app_user_task_excute_finished_check() == 0) {
        return;
    }

    if(tkl_cpu_is_sleep()) {
#if (PM_DEEPSLEEP_RETENTION_ENABLE)
        if(tkl_ble_state == TY_BLE_STA_IDLE) {
            bls_pm_setWakeupSource(PM_WAKEUP_PAD);
            cpu_sleep_wakeup(SUSPEND_MODE, PM_WAKEUP_PAD, 0);

            // if(tkl_sw_timer_next_tick > 0){
            //     if (tkl_board_state_suspend) {
            //     // TAL_PR_DEBUG("1");
            //         cpu_sleep_wakeup(SUSPEND_MODE, PM_WAKEUP_PAD | PM_WAKEUP_TIMER, tkl_sw_timer_next_tick);
            //     } else {
            //         cpu_sleep_wakeup(DEEPSLEEP_MODE_RET_SRAM_LOW32K, PM_WAKEUP_PAD | PM_WAKEUP_TIMER, tkl_sw_timer_next_tick);
            //     }
            // }else{
            //     if (tkl_board_state_suspend) {
            //         cpu_sleep_wakeup(SUSPEND_MODE, PM_WAKEUP_PAD, 0);
            //     } else {
            //         cpu_sleep_wakeup(DEEPSLEEP_MODE_RET_SRAM_LOW32K, PM_WAKEUP_PAD, 0);
            //     }
            // }

        } else {
            if(tkl_ble_trxfifo_not_empty()) {
                bls_pm_setManualLatency(0);
                return;
            }

            if(tkl_board_state_suspend) {
                bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);
            } else {
                bls_pm_setSuspendMask (SUSPEND_ADV | DEEPSLEEP_RETENTION_ADV | SUSPEND_CONN | DEEPSLEEP_RETENTION_CONN);
            }
            bls_pm_setWakeupSource(PM_WAKEUP_PAD);
        }
#else
        bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);
        bls_pm_setWakeupSource(PM_WAKEUP_PAD);
#endif
    }

#else
    tkl_main_loop();
#endif
}

VOID_T tuya_ble_pm_module_init(VOID_T)
{
#if (BLE_MODULE_PM_ENABLE)
	blc_ll_initPowerManagement_module();        //pm module:      	 optional
	blc_pm_setDeepsleepRetentionType(DEEPSLEEP_MODE_RET_SRAM_LOW32K);

#if (PM_DEEPSLEEP_RETENTION_ENABLE)
	bls_pm_setSuspendMask(SUSPEND_DISABLE);
    blc_pm_setDeepsleepRetentionThreshold(PM_RETENTION_THRESHOLD_ADV, PM_RETENTION_THRESHOLD_CONN);
    blc_pm_setDeepsleepRetentionEarlyWakeupTiming(PM_EARLYWAKEUP);
#endif

	bls_pm_registerFuncBeforeSuspend( &app_suspend_enter );
#else
	bls_pm_setSuspendMask(SUSPEND_DISABLE);
#endif
}

VOID_T tuya_battery_power_check(VOID_T)
{
    /*****************************************************************************************
     Note: battery check must do before any flash write/erase operation, cause flash write/erase
            under a low or unstable power supply will lead to error flash operation

            Some module initialization may involve flash write/erase, include: OTA initialization,
                SMP initialization, ..
                So these initialization must be done after  battery check
    *****************************************************************************************/
    #if (BATT_CHECK_ENABLE)  //battery check must do before OTA relative operation
        UINT8_T battery_check_returnVaule = 0;
        UINT32_T alarm_voltage_mv = 0;

        if(analog_read(USED_DEEP_ANA_REG) & LOW_BATT_FLG){
            alarm_voltage_mv = VBAT_ALRAM_THRES_MV + 200;//2.2 V
        }else{
            alarm_voltage_mv = VBAT_ALRAM_THRES_MV;
        }

        do{
            battery_check_returnVaule = app_battery_power_check(alarm_voltage_mv);  
        }while(battery_check_returnVaule);
    #endif
}

void user_init_normal(void)
{
	//random number generator must be initiated here( in the beginning of user_init_nromal)
	//when deepSleep retention wakeUp, no need initialize again
	random_generator_init();  //this is must

    tuya_battery_power_check();

    ///////////////////// USER application initialization ///////////////////
    tkl_init_first();
    
    tkl_init_second();
    
    tkl_init_third();
    
    tkl_init_last();
}

_attribute_ram_code_ void user_init_deepRetn(void)
{
#if (PM_DEEPSLEEP_RETENTION_ENABLE)
	blc_ll_initBasicMCU();   //mandatory
    tkl_ble_gap_tx_power_set(0, RF_POWER_P3p01dBm);
	blc_ll_recoverDeepRetention();

	extern int sdk_mainLoop_run_flag;
	extern u8 blt_dma_tx_rptr;
#if BATT_CHECK_ENABLE
	extern VOID battery_set_detect_enable (INT32_T en);
    battery_set_detect_enable(1);
#endif
	sdk_mainLoop_run_flag = 0;
	blt_dma_tx_rptr = 0;

	irq_enable();

    if(g_tkl_cpu_sleep_callback.post_wakeup_cb){
        g_tkl_cpu_sleep_callback.post_wakeup_cb();
    }

    tkl_init_third();
    
	bls_pm_setSuspendMask(SUSPEND_DISABLE);    
#endif
}

_attribute_ram_code_
void main_loop (void)
{
	blt_sdk_main_loop();

    #if ENABLE_RTC
    extern VOID_T tuya_rtc_loop_handler(VOID_T);
    tuya_rtc_loop_handler();
    #endif

    #if ENABLE_TIMER
    extern VOID_T tuya_etimer_loop_handler(VOID_T);
    tuya_etimer_loop_handler();
    #endif
    
	tkl_main_loop();

	app_power_management ();
}
