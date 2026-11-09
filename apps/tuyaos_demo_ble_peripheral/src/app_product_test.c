/**
 * @file app_product_test.c
 * @brief This is app_product_test file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_sdk_test.h"
#include "tuya_ble_product_test.h"

#if defined(APP_PRODUCT_TEST) && (APP_PRODUCT_TEST == 1)

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/


/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/


/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
STATIC TIMER_ID app_product_test_timer_id = NULL;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/




STATIC VOID_T app_product_test_timeout_handler(TIMER_ID timer_id, VOID_T *arg)
{
    // Not enter production test
    if (tuya_ble_internal_production_test_with_ble_flag_get() == 0) {
        // Enter sleep or anything you need process, this is just a demo
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
        tal_sdk_test_enter_sleep_handler(0, NULL);
#endif
    }
}

VOID_T app_product_test_init(VOID_T)
{
    tal_sw_timer_create(app_product_test_timeout_handler, NULL, &app_product_test_timer_id);
    tal_sw_timer_start(app_product_test_timer_id, 500, TAL_TIMER_ONCE);
}

#endif

