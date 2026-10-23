/**
 * @file app_charge.h
 * @brief USB and charge-state detection.
 */

#ifndef __APP_CHARGE_H__
#define __APP_CHARGE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

VOID_T app_charge_init(VOID_T);
/* Create an idle timer once on normal boot. */
VOID_T app_charge_timer_init(VOID_T);
VOID_T app_charge_process(VOID_T);
BOOL_T app_charge_is_detected(VOID_T);
BOOL_T app_charge_is_full(VOID_T);
BOOL_T app_charge_read_usb(VOID_T);

#ifdef __cplusplus
}
#endif

#endif
