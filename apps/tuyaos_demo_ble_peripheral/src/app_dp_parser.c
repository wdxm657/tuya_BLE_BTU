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
#include "tal_flash.h"
#include "tal_util.h"
#include "board.h"

#include "tuya_ble_api.h"
#include "tuya_ble_mutli_tsf_protocol.h"

#include "app_dp_parser.h"
#include "app_state.h"
#include "app_motor.h"
#include "app_led.h"

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/


/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/

#define DP_CONFIG_MAGIC             0x44504346UL /* "DPCF" */
#define DP_CONFIG_VERSION           1U
#define DP_CONFIG_FLASH_SIZE        0x1000U
#define DP_CONFIG_DEFAULT_MODE      GAME_MODE_FAST
#define DP_CONFIG_DEFAULT_MOVEMENT  0U

#pragma pack(1)
typedef struct {
    UINT32_T magic;
    UINT8_T  version;
    UINT8_T  reserved; /* 兼容旧版 Flash 布局，不再保存开关状态。 */
    UINT8_T  mode;
    UINT8_T  movement_level;
    UINT8_T  checksum;
} app_dp_config_t;
#pragma pack()


/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
demo_dp_t g_cmd = {0};
demo_dp_t g_rsp = {0};
UINT32_T  g_sn  = 0;

STATIC app_dp_config_t s_dp_config = {
    DP_CONFIG_MAGIC,
    DP_CONFIG_VERSION,
    0,
    DP_CONFIG_DEFAULT_MODE,
    DP_CONFIG_DEFAULT_MOVEMENT,
    0,
};

STATIC UINT8_T app_dp_config_checksum(CONST app_dp_config_t *config)
{
    CONST UINT8_T *data = (CONST UINT8_T *)config;
    UINT8_T checksum = 0;
    UINT8_T i;

    for (i = 0; i < (UINT8_T)(SIZEOF(app_dp_config_t) - 1U); i++) {
        checksum = (UINT8_T)(checksum + data[i]);
    }
    return (UINT8_T)(checksum ^ 0xFFU);
}

STATIC BOOL_T app_dp_config_is_valid(CONST app_dp_config_t *config)
{
    return (config->magic == DP_CONFIG_MAGIC &&
            config->version == DP_CONFIG_VERSION &&
            config->mode <= GAME_MODE_INTERACTIVE &&
            config->movement_level <= 4U &&
            config->checksum == app_dp_config_checksum(config));
}

STATIC OPERATE_RET app_dp_config_save(VOID_T)
{
    app_dp_config_t old_config;
    OPERATE_RET ret;

    s_dp_config.checksum = app_dp_config_checksum(&s_dp_config);
    if (tal_flash_read(USER_DP_CONFIG_FLASH_ADDR,
                       (UINT8_T *)&old_config,
                       SIZEOF(old_config)) == OPRT_OK &&
        memcmp(&old_config, &s_dp_config, SIZEOF(s_dp_config)) == 0) {
        return OPRT_OK;
    }

    ret = tal_flash_erase(USER_DP_CONFIG_FLASH_ADDR, DP_CONFIG_FLASH_SIZE);
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[dp_cfg] flash erase failed: %d", ret);
        return ret;
    }

    ret = tal_flash_write(USER_DP_CONFIG_FLASH_ADDR,
                          (CONST UINT8_T *)&s_dp_config,
                          SIZEOF(s_dp_config));
    if (ret != OPRT_OK) {
        TAL_PR_ERR("[dp_cfg] flash write failed: %d", ret);
        return ret;
    }

    TAL_PR_INFO("[dp_cfg] saved: mode=%d movement=%d",
                s_dp_config.mode,
                s_dp_config.movement_level);
    return OPRT_OK;
}

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
        case DP_ID_SWITCH: {
            /* 蓝牙开关控制电源（与物理按键短按相同） */
            BOOL_T sw_on = (g_cmd.dp_data[0] != 0);
            TAL_PR_INFO("DP_ID_SWITCH: %s", sw_on ? "ON" : "OFF");
            g_cmd.dp_data[0] = app_state_set_power(sw_on) ? 1U : 0U;
            /* 复位待机定时器 */
            app_state_reset_standby_timer();
        } break;

        case DP_ID_MODE: {
            /* 游戏模式设置 */
            game_mode_t mode = (game_mode_t)g_cmd.dp_data[0];
            TAL_PR_INFO("DP_ID_MODE: %d", mode);
            if (mode <= GAME_MODE_INTERACTIVE) {
                app_motor_set_mode(mode);
                s_dp_config.mode = (UINT8_T)mode;
                app_dp_config_save();
            }
            /* 复位待机定时器 */
            app_state_reset_standby_timer();
        } break;

        case DP_ID_MOVEMENT_LEVEL: {
            TAL_PR_INFO("DP_ID_MOVEMENT_LEVEL: %d", g_cmd.dp_data[0]);
            if (g_cmd.dp_data[0] <= 4U) {
                s_dp_config.movement_level = g_cmd.dp_data[0];
                app_dp_config_save();
            }
        } break;

        default: {
            TAL_PR_DEBUG("invalid dp_id: %d", g_cmd.dp_id);
        } break;
    }


    /* 对 rw DP 回复确认上报 */
    app_dp_report(g_cmd.dp_id, g_cmd.dp_data, g_cmd.dp_data_len);

    return OPRT_OK;
}

BOOL_T app_dp_config_restore(VOID_T)
{
    app_dp_config_t config;

    if (tal_flash_read(USER_DP_CONFIG_FLASH_ADDR,
                       (UINT8_T *)&config,
                       SIZEOF(config)) != OPRT_OK ||
        !app_dp_config_is_valid(&config)) {
        s_dp_config.magic = DP_CONFIG_MAGIC;
        s_dp_config.version = DP_CONFIG_VERSION;
        s_dp_config.reserved = 0;
        s_dp_config.mode = DP_CONFIG_DEFAULT_MODE;
        s_dp_config.movement_level = DP_CONFIG_DEFAULT_MOVEMENT;
        s_dp_config.checksum = app_dp_config_checksum(&s_dp_config);
        TAL_PR_INFO("[dp_cfg] no valid config, use factory defaults");
        return FALSE;
    }

    s_dp_config = config;
    s_dp_config.reserved = 0;
    app_motor_set_mode((game_mode_t)s_dp_config.mode);
    TAL_PR_INFO("[dp_cfg] restored: mode=%d movement=%d",
                s_dp_config.mode,
                s_dp_config.movement_level);
    return TRUE;
}

OPERATE_RET app_dp_config_factory_reset(VOID_T)
{
    OPERATE_RET ret = tal_flash_erase(USER_DP_CONFIG_FLASH_ADDR,
                                      DP_CONFIG_FLASH_SIZE);

    s_dp_config.magic = DP_CONFIG_MAGIC;
    s_dp_config.version = DP_CONFIG_VERSION;
    s_dp_config.reserved = 0;
    s_dp_config.mode = DP_CONFIG_DEFAULT_MODE;
    s_dp_config.movement_level = DP_CONFIG_DEFAULT_MOVEMENT;
    s_dp_config.checksum = app_dp_config_checksum(&s_dp_config);

    if (ret == OPRT_OK) {
        TAL_PR_INFO("[dp_cfg] factory config cleared");
    } else {
        TAL_PR_ERR("[dp_cfg] factory clear failed: %d", ret);
    }
    return ret;
}

OPERATE_RET app_dp_report(UINT8_T dp_id, UINT8_T* buf, UINT32_T size)
{
    memset(&g_rsp, 0, SIZEOF(demo_dp_t));

    g_rsp.dp_id = dp_id;

    switch (dp_id) {
        /* bool: 开关 */
        case DP_ID_SWITCH: {
            g_rsp.dp_type = DT_BOOL;
            g_rsp.dp_data_len = DT_BOOL_LEN;
            memcpy(g_rsp.dp_data, buf, DT_BOOL_LEN);
        } break;

        /* enum: 工作模式 active/simple/mild */
        case DP_ID_MODE: {
            g_rsp.dp_type = DT_ENUM;
            g_rsp.dp_data_len = DT_ENUM_LEN;
            memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        } break;

        /* value: 电池电量 0~100% */
        case DP_ID_BATTERY: {
            g_rsp.dp_type = DT_VALUE;
            g_rsp.dp_data_len = DT_VALUE_LEN;
            memcpy(g_rsp.dp_data, buf, DT_VALUE_LEN);
        } break;

        /* bitmap: 故障告警 */
        case DP_ID_FAULT: {
            g_rsp.dp_type = DT_BITMAP;
            g_rsp.dp_data_len = DT_BITMAP_MAX;
            memcpy(g_rsp.dp_data, buf, DT_BITMAP_MAX);
        } break;

        /* enum: 行进速度 level_1~level_5 */
        case DP_ID_MOVEMENT_LEVEL: {
            g_rsp.dp_type = DT_ENUM;
            g_rsp.dp_data_len = DT_ENUM_LEN;
            memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        } break;

        /* enum: 工作状态 work/standby/charging/charge_done */
        case DP_ID_WORK_STATE: {
            g_rsp.dp_type = DT_ENUM;
            g_rsp.dp_data_len = DT_ENUM_LEN;
            memcpy(g_rsp.dp_data, buf, DT_ENUM_LEN);
        } break;

        default: {
        } break;
    }

    UINT16_T rsp_len = g_rsp.dp_data_len + 4;

    tal_util_reverse_byte(&g_rsp.dp_data_len, SIZEOF(UINT16_T));

    TAL_PR_HEXDUMP_INFO("dp_rsp", (VOID_T*)&g_rsp, rsp_len);

    return tuya_ble_dp_data_send(g_sn++, DP_SEND_TYPE_ACTIVE, DP_SEND_FOR_CLOUD_PANEL, DP_SEND_WITHOUT_RESPONSE, (VOID_T*)&g_rsp, rsp_len);
}

