/**
 * @file app_dp_parser.c
 * @brief This is app_dp_parser file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */


#include "string.h"

#include "tal_log.h"
#include "tal_util.h"

#include "tuya_ble_api.h"
#include "tuya_ble_mutli_tsf_protocol.h"

#include "app_dp_parser.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/


/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/


/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
demo_dp_t g_cmd = {0};
demo_dp_t g_rsp = {0};
UINT32_T  g_sn  = 0;

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/




OPERATE_RET app_dp_parser(UINT8_T* buf, UINT32_T size)
{
    memcpy(&g_cmd, buf, size);
    tal_util_reverse_byte(&g_cmd.dp_data_len, SIZEOF(UINT16_T));
    memcpy(&g_rsp, &g_cmd, size);

    TAL_PR_HEXDUMP_INFO("dp_cmd", (VOID_T*)&g_cmd, (g_cmd.dp_data_len + 4));

    switch (g_cmd.dp_id) {
    }

    /* 对 rw DP 回复确认上报 */
    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);

    return OPRT_OK;
}

OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T* buf, UINT32_T size)
{
    memset(&g_rsp, 0, SIZEOF(demo_dp_t));

    g_rsp.dp_id = dp_id;

    UINT16_T rsp_len = g_rsp.dp_data_len + 4;

    tal_util_reverse_byte(&g_rsp.dp_data_len, SIZEOF(UINT16_T));

    TAL_PR_HEXDUMP_INFO("dp_rsp", (VOID_T*)&g_rsp, rsp_len);

    return tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE, DP_SEND_FOR_CLOUD_PANEL, DP_SEND_WITHOUT_RESPONSE, (VOID_T*)&g_rsp, rsp_len);
}

