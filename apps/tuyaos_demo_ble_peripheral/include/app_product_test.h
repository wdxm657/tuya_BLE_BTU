/**
 * @file app_product_test.h
 * @brief This is app_product_test file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#ifndef __APP_PRODUCT_TEST_H__
#define __APP_PRODUCT_TEST_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "tuya_cloud_types.h"

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


/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/

/**
 * @brief app_product_test_init
 *
 * @param[in] param1:
 * @param[in] param2:
 *
 * @return OPRT_OK on success. Others on error, please refer to tuya_error_code.h
 */
VOID_T app_product_test_init(VOID_T);

#endif


#ifdef __cplusplus
}
#endif

#endif /* __APP_PRODUCT_TEST_H__ */

