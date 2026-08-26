/**
 * @file tuya_ble_product_test.c
 * @brief This is tuya_ble_product_test file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

#include "string.h"
#include "stdio.h"

#include "board.h"
#include "tal_util.h"
#include "tal_bluetooth.h"
#include "tkl_bluetooth_def.h"
#include "tal_gpio_test.h"

#include "tuya_ble_type.h"
#include "tuya_ble_mem.h"
#include "tuya_ble_api.h"
#include "tuya_ble_port.h"
#include "tuya_ble_main.h"
#include "tuya_ble_internal_config.h"
#include "tuya_ble_data_handler.h"
#include "tuya_ble_mutli_tsf_protocol.h"
#include "tuya_ble_secure.h"
#include "tuya_ble_main.h"
#include "tuya_ble_storage.h"
#include "tuya_ble_product_test.h"
#include "tuya_ble_log.h"
#include "tuya_ble_api.h"

#if defined(TUYA_BLE_FEATURE_PRODUCT_TEST_ENABLE) && (TUYA_BLE_FEATURE_PRODUCT_TEST_ENABLE == 1)

#if defined(CUSTOMIZED_TUYA_BLE_APP_PRODUCT_TEST_HEADER_FILE)
#include CUSTOMIZED_TUYA_BLE_APP_PRODUCT_TEST_HEADER_FILE
#endif

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
#if (TUYA_BLE_DEVICE_REGISTER_FROM_BLE&&TUYA_BLE_DEVICE_AUTH_DATA_STORE)

#define MAX_RSSI_NUM                        3
#define tuya_ble_prod_beacon_scan_timeout   5000

typedef enum {
    COMMON_CFG_FSM_STATE_IDLE = 0,
    COMMON_CFG_FSM_STATE_RCV_SIZE,
    COMMON_CFG_FSM_STATE_RCV_DATA,
    COMMON_CFG_FSM_STATE_RCV_CRC32,
} ENUM_COMMON_CFG_FSM_STATE;

#define COMM_CFG_KEY_DEV_CERTIFICATE    ( "deviceCertificate" )
#define COMM_CFG_KEY_PRIVATE_KEY        ( "privateKey" )
#define COMM_CFG_KEY_SHORTURL           ( "shortUrl" )
#define COMM_CFG_KEY_CALIBRATION        ( "calibration" )

#ifndef TAL_GPIO_TEST_ERROR_SEQUENCE_MAX_LEN
#define TAL_GPIO_TEST_ERROR_SEQUENCE_MAX_LEN       80 // 8*10, ("11,10",)=8 bytes, 20 gpios, 10 groups
#endif

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/
typedef struct {
    UINT8_T is_start;
    UINT8_T periph_name_len;
    CHAR_T  periph_name[30];
    UINT8_T count;
    INT8_T  rssi[MAX_RSSI_NUM];
} TUYA_BFT_RF_TEST_T;

#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)
typedef struct {
    BOOL_T need_subpkg;         /**< whether need subpkg received. */

    UINT8_T file_type;          /**< file type. 0x09-OEM file 0x10-homekit atoken. */
    UINT32_T total_nums;        /**< subpkg total numbers. */
    UINT32_T file_crc32;        /**< file crc32 value. */
    UINT32_T rcv_file_len;      /**< currently received file length. */
    UINT8_T *p_file_data;       /**< the point of received file data. */
} write_subpkg_info_t;
#endif

/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
STATIC CONST CHAR_T true_buf[]  = "{\"ret\":true}";
STATIC CONST CHAR_T false_buf[] = "{\"ret\":false}";

STATIC UINT8_T tuya_ble_production_test_flag = 0;
STATIC UINT8_T tuya_ble_production_test_with_ble_flag = 0;

#define tuya_ble_prod_monitor_timeout_ms  60000  //60s

tuya_ble_timer_t tuya_ble_xTimer_prod_monitor;
tuya_ble_timer_t tuya_ble_prod_beacon_scan_timer;

/**@brief   Used for wirte common config file. */
volatile ENUM_COMMON_CFG_FSM_STATE tuya_ble_comm_cfg_fsm_state = COMMON_CFG_FSM_STATE_IDLE;
volatile UINT32_T comm_cfg_total_len = 0;
volatile UINT32_T comm_cfg_cur_rcv_len = 0;
UINT8_T *p_comm_cfg = NULL;

STATIC TUYA_BFT_RF_TEST_T tuya_bft_rf_test = {
    .is_start = FALSE,
    .count = 0,
};

#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)
STATIC write_subpkg_info_t write_subpkg_info = {
    .need_subpkg    = FALSE,

    .file_type      = 0,
    .total_nums     = 0,
    .file_crc32     = 0,
    .rcv_file_len   = 0,
    .p_file_data    = NULL,
};
#endif

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/
STATIC VOID_T tuya_ble_prod_beacon_scan_timer_uninit(VOID_T);

VOID_T tuya_ble_internal_production_test_with_ble_flag_clear(VOID_T)
{
    tuya_ble_production_test_with_ble_flag = 0;
}

UINT8_T tuya_ble_internal_production_test_with_ble_flag_get(VOID_T)
{
    return (tuya_ble_production_test_with_ble_flag||tuya_ble_production_test_flag);
}

STATIC VOID_T tuya_ble_vtimer_prod_monitor_callback(tuya_ble_timer_t pxTimer)
{
    tuya_ble_device_delay_ms(1000);
    tuya_ble_device_reset();
}

STATIC VOID_T tuya_ble_prod_monitor_timer_init(VOID_T)
{
    if (tuya_ble_timer_create(&tuya_ble_xTimer_prod_monitor, tuya_ble_prod_monitor_timeout_ms, TUYA_BLE_TIMER_SINGLE_SHOT, tuya_ble_vtimer_prod_monitor_callback) != TUYA_BLE_SUCCESS) {
        TUYA_BLE_LOG_ERROR("tuya_ble_xTimer_prod_monitor creat failed");
    }
}

STATIC VOID_T tuya_ble_prod_monitor_timer_start(VOID_T)
{
    if (tuya_ble_timer_start(tuya_ble_xTimer_prod_monitor) != TUYA_BLE_SUCCESS) {
        TUYA_BLE_LOG_ERROR("tuya_ble_xTimer_prod_monitor start failed");
    }
}

STATIC UINT32_T tuya_ble_uart_prod_send(UINT8_T type, UINT8_T *pdata, UINT16_T len)
{
    UINT16_T uart_send_len = 7 + len;
    UINT8_T *uart_send_buffer = NULL;

    uart_send_buffer = (UINT8_T *)tuya_ble_malloc(uart_send_len);
    if (uart_send_buffer != NULL) {
        uart_send_buffer[0] = 0x66;
        uart_send_buffer[1] = 0xAA;
        uart_send_buffer[2] = 0x00;
        uart_send_buffer[3] = type;
        uart_send_buffer[4] = (len>>8) & 0xFF;
        uart_send_buffer[5] = (len>>0) & 0xFF;
        memcpy(uart_send_buffer + 6, pdata, len);
        uart_send_buffer[6+len] = tal_util_check_sum8(uart_send_buffer, 6 + len);
        tuya_ble_common_uart_send_data(uart_send_buffer, 7 + len);
        tuya_ble_free(uart_send_buffer);
    } else {
        TUYA_BLE_LOG_ERROR("uart prod send buffer malloc failed.");
        return 1;
    }

    return 0;
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_prod_update_mac(UINT8_T *mac)
{
    return TUYA_BLE_SUCCESS;
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_prod_beacon_scan_start(VOID_T)
{
    TAL_BLE_SCAN_PARAMS_T tal_scan_param = {
        .type = TAL_BLE_SCAN_TYPE_ACTIVE,
        .scan_interval = 100*8/5,
        .scan_window = 80*8/5,
        .timeout = 0,
        .filter_dup = 0,
    };
    tal_ble_scan_start(&tal_scan_param);
    return TUYA_BLE_SUCCESS;
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_prod_beacon_scan_stop(VOID_T)
{
    tal_ble_scan_stop();
    return TUYA_BLE_SUCCESS;
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_prod_beacon_get_rssi_avg(INT8_T *rssi)
{
    INT16_T rssi_sum = 0;

    if (tuya_bft_rf_test.count < MAX_RSSI_NUM) {
        tuya_bft_rf_test.count =  0;
        tuya_bft_rf_test.is_start = FALSE;
        return TUYA_BLE_ERR_TIMEOUT;
    }

    for (uint16_t i=0; i<tuya_bft_rf_test.count; i++) {
        rssi_sum += tuya_bft_rf_test.rssi[i];
    }

    *rssi = rssi_sum/tuya_bft_rf_test.count;

    tuya_bft_rf_test.count =  0;
    tuya_bft_rf_test.is_start = FALSE;
    return TUYA_BLE_SUCCESS;
}

STATIC VOID_T tuya_ble_prod_beacon_scan_timer_callback(tuya_ble_timer_t pxTimer)
{
    INT8_T rssi = -100;
    UINT16_T length = 0;
    CHAR_T true_reply_buf[30];

    tuya_ble_prod_beacon_scan_stop();
    if (tuya_ble_prod_beacon_get_rssi_avg(&rssi) == TUYA_BLE_SUCCESS) {
        length = sprintf((char *)true_reply_buf, "{\"ret\":true,\"rssi\":\"%d\"}", rssi);
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_RSSI_TEST, (uint8_t *)true_reply_buf, length);
        TUYA_BLE_LOG_DEBUG("RSSI TEST SUCCESS! rssi is :%d", rssi);
    } else {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_RSSI_TEST, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("RSSI TEST FAILED!");
    }

    tuya_ble_prod_beacon_scan_timer_uninit();
    return;
}

STATIC VOID_T tuya_ble_prod_beacon_scan_timer_init(VOID_T)
{
    if (tuya_ble_prod_beacon_scan_timer != NULL) {
        TUYA_BLE_LOG_ERROR("tuya_ble_prod_beacon_scan_timer already exists");
        return;
    }

    if (tuya_ble_timer_create(&tuya_ble_prod_beacon_scan_timer, tuya_ble_prod_beacon_scan_timeout, TUYA_BLE_TIMER_SINGLE_SHOT, tuya_ble_prod_beacon_scan_timer_callback) != TUYA_BLE_SUCCESS) {
        TUYA_BLE_LOG_ERROR("tuya_ble_prod_beacon_scan_timer creat failed");
    }
    return;
}

STATIC VOID_T tuya_ble_prod_beacon_scan_timer_start(VOID_T)
{
    if (tuya_ble_timer_start(tuya_ble_prod_beacon_scan_timer) != TUYA_BLE_SUCCESS) {
        TUYA_BLE_LOG_ERROR("tuya_ble_prod_beacon_scan_timer start failed");
    }
    return;
}

STATIC VOID_T tuya_ble_prod_beacon_scan_timer_uninit(VOID_T)
{
    if (tuya_ble_timer_delete(tuya_ble_prod_beacon_scan_timer) != TUYA_BLE_SUCCESS) {
        TUYA_BLE_LOG_ERROR("tuya_ble_xTimer_prod_monitor delete failed");
    } else {
        tuya_ble_prod_beacon_scan_timer = NULL;
    }

    return;
}

__TUYA_BLE_WEAK VOID_T tuya_ble_prod_beacon_handler(VOID_T* buf)
{
    TAL_BLE_ADV_REPORT_T* p_adv_report = buf;
    UINT8_T *p_name = NULL;
    UINT8_T name_len = 0;
    tal_util_adv_report_parse(0x09, p_adv_report->p_data, p_adv_report->data_len, &p_name, &name_len);
    if ((name_len == tuya_bft_rf_test.periph_name_len) && (p_name != NULL) && (memcmp(p_name, tuya_bft_rf_test.periph_name, tuya_bft_rf_test.periph_name_len) == 0)) {
        tuya_bft_rf_test.rssi[tuya_bft_rf_test.count] = p_adv_report->rssi;
        tuya_bft_rf_test.count++;
    }

    if (tuya_bft_rf_test.count >= MAX_RSSI_NUM) {
        tuya_ble_prod_beacon_scan_timer_callback(tuya_ble_prod_beacon_scan_timer);
    }
    return;
}

STATIC VOID_T tuya_ble_auc_enter(UINT8_T *para, UINT16_T len)
{
    UINT8_T buf[2] = {0};

    /* if dev is binding, can't entry ftm mode */
    if (tuya_ble_current_para.sys_settings.bound_flag == 1) {
        TAL_PR_INFO("TAL_BLE_EVT_READ_RX");
        TUYA_BLE_LOG_DEBUG("AUC ENTER, BUT DEV IS BINDING");

        tuya_ble_device_delay_ms(200);
        tuya_ble_device_reset();
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC ENTER!");

#if (TUYA_BLE_SECURE_CONNECTION_TYPE == TUYA_BLE_SECURE_CONNECTION_WITH_AUTH_KEY_ADVANCED_ENCRYPTION)
    buf[0] = (TUYA_BLE_AUC_FINGERPRINT_VER<<TUYA_BLE_AUC_FW_FINGERPRINT_POS) | \
             (TUYA_BLE_AUC_WRITE_PID<<TUYA_BLE_AUC_WRITE_PID_POS) | \
             (TUYA_BLE_AUC_WRITE_DEV_CERT<<TUYA_BLE_AUC_WRITE_DEV_CERT_POS) | \
             (TUYA_BLE_AUC_NEED_PULL_GPIO_TEST_MAP<<TUYA_BLE_AUC_NEED_PULL_GPIO_TEST_MAP_POS);
#else
    buf[0] = (TUYA_BLE_AUC_FINGERPRINT_VER<<TUYA_BLE_AUC_FW_FINGERPRINT_POS) | \
             (TUYA_BLE_AUC_WRITE_PID<<TUYA_BLE_AUC_WRITE_PID_POS) | \
             (TUYA_BLE_AUC_NEED_PULL_GPIO_TEST_MAP<<TUYA_BLE_AUC_NEED_PULL_GPIO_TEST_MAP_POS) | 0x08;
#endif

#if TUYA_BLE_WRITE_BT_MAC
    buf[1] = (TUYA_BLE_WRITE_BT_MAC<<TUYA_BLE_WRITE_BT_MAC_POS);
#endif

    if (tuya_ble_production_test_flag == 1) {
#if TUYA_BLE_WRITE_BT_MAC
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_ENTER, buf, 2);
#else
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_ENTER, buf, 1);
#endif
        return;
    }

    tuya_ble_prod_monitor_timer_init();
    tuya_ble_prod_monitor_timer_start();
    tuya_ble_prod_sleep_timer_init();

    tuya_ble_production_test_flag = 1;
    tuya_ble_comm_cfg_fsm_state = COMMON_CFG_FSM_STATE_IDLE;

#if TUYA_BLE_WRITE_BT_MAC
    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_ENTER, buf, 2);
#else
    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_ENTER, buf, 1);
#endif
}

STATIC VOID_T tuya_ble_auc_query_hid(UINT8_T *para, UINT16_T len)
{
    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC QUERY HID!");
    CHAR_T buf[70] = "{\"ret\":true,\"hid\":\"\"}";

    if (tal_util_buffer_value_is_all_x(tuya_ble_current_para.auth_settings.h_id, H_ID_LEN, 0xFF)) {
        buf[19] = '\"';
        buf[20] = '}';
    } else if (tal_util_buffer_value_is_all_x(tuya_ble_current_para.auth_settings.h_id, H_ID_LEN, 0)) {
        buf[19] = '\"';
        buf[20] = '}';
    } else {
        memcpy(&buf[19], tuya_ble_current_para.auth_settings.h_id, H_ID_LEN);
        buf[38] = '\"';
        buf[39] = '}';
    }

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_QUERY_HID, (UINT8_T *)buf, strlen(buf));

    TUYA_BLE_LOG_HEXDUMP_DEBUG("AUC QUERY HID response data : ", (UINT8_T *)buf, strlen(buf));
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_prod_gpio_test(CHAR_T* error_sequence, UINT8_T *para, UINT8_T len)
{
#if (TAL_GPIO_TEST_ENABLE)
    return tal_gpio_test_handler(error_sequence, para, len);
#else
    return FALSE;
#endif
}

STATIC VOID_T tuya_ble_auc_gpio_test(UINT8_T *para, UINT16_T len)
{
    UINT8_T buf[TAL_GPIO_TEST_ERROR_SEQUENCE_MAX_LEN+23] = {0};
    CHAR_T error_sequence[TAL_GPIO_TEST_ERROR_SEQUENCE_MAX_LEN] = {0};

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC GPIO TEST!");

    if (tuya_ble_prod_gpio_test(error_sequence, para, len) == TRUE) {
        memcpy(buf, "{\"ret\":true}", 12);
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_GPIO_TEST, buf, strlen((VOID_T*)buf));
        TUYA_BLE_LOG_DEBUG("AUC GPIO TEST successed!");
    } else {
        sprintf((VOID_T*)buf, "{\"ret\":false,\"G\":[\"%s\"]}", error_sequence);
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_GPIO_TEST, buf, strlen((VOID_T*)buf));
        TUYA_BLE_LOG_ERROR("AUC GPIO TEST failed!");
    }
}

STATIC VOID_T tuya_ble_auc_write_auth_info(UINT8_T *para, UINT16_T len)
{
#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)
    UINT8_T pid_len = 0, i = 0, pid_pos = 0;
#endif
    UINT8_T bt_mac_string_len = 0;
#if TUYA_BLE_WRITE_BT_MAC
    UINT8_T bt_mac_pos = 0;
#endif
    UINT8_T mac_temp[6];
    UINT8_T mac_char[13];
    UINT8_T bt_mac_temp[6] = {0};
    UINT8_T bt_mac_char[13] = {0};

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC WRITE AUTH INFO!");

    /*
      "auzkey":"xxxx",    //"6":"32", 7 + 6+4
      "uuid":"xxxx",      //"4":"16", 7 + 6+32+6 + 4+4
      "mac":"xxxxxx",     //"3":"12",
      "prod_test":"xxxx"  //"9":"4/5"
      "pid":"abcdefgh"    //if any
    */

    if (len < 100) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error ,since Invalid length!");
        return;
    }

#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)

    if ((memcmp(&para[2], "auzkey",6) != 0) || \
        (memcmp(&para[46], "uuid",4) != 0) || \
        (memcmp(&para[72], "mac",3) != 0) || \
        ((memcmp(&para[110], "pid\":\"", 6) != 0) && (memcmp(&para[111], "pid\":\"", 6) != 0))) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error ,since Invalid paras");
        return;
    }

#if TUYA_BLE_WRITE_BT_MAC
    if ((memcmp(&para[127], "bt_mac", 6) != 0) && (memcmp(&para[128], "bt_mac", 6) !=0 )) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error, since Invalid paras");
        return;
    }

    i = ((para[135] == '\"')?136:137);
    bt_mac_pos = i;
    bt_mac_string_len = 0;
    while ((para[i] != '\"')&&(bt_mac_string_len <= 12)) {
        i++;
        bt_mac_string_len++;
    }

    if (bt_mac_string_len != 12) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error, bt mac len != 12.");
        return;
    }

    memcpy(bt_mac_char, &para[bt_mac_pos], 12);
    tal_util_str_hexstr2hexarray(bt_mac_char, 12, bt_mac_temp);
#endif

    i = ((para[115] == '\"') ? 116 : 117);
    pid_pos = i;
    pid_len = 0;

    while ((para[i] != '\"') && (pid_len <= 20)) {
        i++;
        pid_len++;
    }

    if (pid_len > 20) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error, because pid len > 20 .");
        return;
    }

    memcpy(mac_char, &para[78], 12);
    tal_util_str_hexstr2hexarray(mac_char, 12, mac_temp);

    tuya_ble_factory_id_data_t write_auth_info = {0};

    write_auth_info.pid_len = pid_len;
    memcpy(write_auth_info.pid, &para[pid_pos], pid_len);
    memcpy(write_auth_info.auth_key, &para[11], AUTH_KEY_LEN);
    memcpy(write_auth_info.device_id, &para[53], DEVICE_ID_LEN);
    memcpy(write_auth_info.mac, mac_temp, MAC_LEN);
    memcpy(write_auth_info.mac_string, mac_char, MAC_LEN*2);

    write_auth_info.bt_mac_len = bt_mac_string_len/2;
    memcpy(write_auth_info.bt_mac, bt_mac_temp, write_auth_info.bt_mac_len);
    memcpy(write_auth_info.bt_mac_string, bt_mac_char, bt_mac_string_len);

    if (tuya_ble_storage_write_auth_info(&write_auth_info) == TUYA_BLE_SUCCESS) {
        tuya_ble_prod_update_mac(mac_temp);
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)true_buf, strlen(true_buf));
        TUYA_BLE_LOG_DEBUG("AUC WRITE AUTH INFO successed!");
    } else {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO failed!");
    }

#else

    if ((memcmp(&para[2], "auzkey", 6) != 0) || \
        (memcmp(&para[46], "uuid", 4) != 0 ) || \
        (memcmp(&para[72], "mac", 3) != 0 )) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error, since Invalid paras");
        return;
    }

    memcpy(mac_char, &para[78], 12);
    tal_util_str_hexstr2hexarray(mac_char, 12, mac_temp);

#if TUYA_BLE_WRITE_BT_MAC
    if ((memcmp(&para[110], "bt_mac", 6) != 0) && (memcmp(&para[111], "bt_mac", 6) != 0)) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error ,since Invalid paras");
        return;
    }

    i = para[118] == '\"'?119:120;
    bt_mac_pos = i;
    bt_mac_string_len = 0;
    while ((para[i] != '\"') && (bt_mac_string_len <= 12)) {
        i++;
        bt_mac_string_len++;
    }
    if (bt_mac_string_len != 12) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO error, bt mac len != 12.");
        return;
    }

    memcpy(bt_mac_char, &para[bt_mac_pos], 12);
    tal_util_str_hexstr2hexarray(bt_mac_char, 12, bt_mac_temp);
#endif

    tuya_ble_factory_id_data_t write_auth_info = {0};

    write_auth_info.pid_len = 0;
    memcpy(write_auth_info.auth_key, &para[11], AUTH_KEY_LEN);
    memcpy(write_auth_info.device_id, &para[53], DEVICE_ID_LEN);
    memcpy(write_auth_info.mac, mac_temp, MAC_LEN);
    memcpy(write_auth_info.mac_string, mac_char, MAC_LEN*2);

    write_auth_info.bt_mac_len = bt_mac_string_len/2;
    memcpy(write_auth_info.bt_mac, bt_mac_temp, write_auth_info.bt_mac_len);
    memcpy(write_auth_info.bt_mac_string, bt_mac_char, bt_mac_string_len);

    if (tuya_ble_storage_write_auth_info(&write_auth_info) == TUYA_BLE_SUCCESS) {
        tuya_ble_prod_update_mac(mac_temp);

        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)true_buf, strlen(true_buf));
        TUYA_BLE_LOG_DEBUG("AUC WRITE AUTH INFO successed!");
    } else {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO, (uint8_t *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC_CMD_WRITE_AUTH_INFO failed!");
    }
#endif
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_product_test_storage_common_data(tuya_ble_common_data_type type, UINT8_T *p_data, UINT32_T data_size)
{
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    tuya_ble_status_t ret = TUYA_BLE_SUCCESS;
    UINT8_T *p_buf = NULL;
    UINT32_T op_addr;

    if (type == COMMON_DATA_SHORTURL) {
        // data format: "vaule":"https://t.tuya.com/AYtKKxLV", "encode":"utf8"
        op_addr = BOARD_FLASH_SDK_TEST_START_ADDR;
    } else {
        return TUYA_BLE_ERR_INVALID_PARAM;
    }

    p_buf = (UINT8_T *)tuya_ble_malloc(8 + data_size);
    if (p_buf == NULL) {
        return TUYA_BLE_ERR_NO_MEM;
    }

    memset(p_buf, 0x00, 8);
    p_buf[0] = (UINT8_T)(data_size);
    p_buf[1] = (UINT8_T)(data_size >> 8);
    p_buf[2] = (UINT8_T)(data_size >> 16);
    p_buf[3] = (UINT8_T)(data_size >> 24);
    memcpy(&p_buf[8], p_data, data_size);

    tuya_ble_nv_erase(op_addr, TUYA_NV_ERASE_MIN_SIZE);
    tuya_ble_nv_write(op_addr, p_buf, (8+data_size));

    if (p_buf != NULL) {
        tuya_ble_free(p_buf);
    }

    return ret;
#else
    return TUYA_BLE_ERR_COMMON;
#endif
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_product_test_readback_common_data(tuya_ble_common_data_type type, UINT8_T *p_data, UINT32_T *data_size)
{
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    UINT8_T hdr[8] = {0};
    UINT32_T op_addr, common_data_size;
    UINT8_T *p_buf = NULL;
    tuya_ble_status_t ret = TUYA_BLE_SUCCESS;

    if (type == COMMON_DATA_SHORTURL) {
        op_addr = BOARD_FLASH_SDK_TEST_START_ADDR;
    } else {
        return TUYA_BLE_ERR_INVALID_PARAM;
    }

    tuya_ble_nv_read(op_addr, hdr, 8);
    common_data_size = *(UINT32_T *)hdr;
    TUYA_BLE_LOG_DEBUG("read common_data_type=%d, addr=%x, size=%d", type, op_addr, common_data_size);

    p_buf = (UINT8_T *)tuya_ble_malloc(8 + common_data_size);
    if (p_buf == NULL)
        ret = TUYA_BLE_ERR_NO_MEM;
    else {
        tuya_ble_nv_read(op_addr, p_buf, (8+common_data_size));

        memcpy(p_data, &p_buf[8], common_data_size);
        *data_size = common_data_size;

        tuya_ble_free(p_buf);
    }

    return ret;
#else
    return TUYA_BLE_ERR_COMMON;
#endif
}

__TUYA_BLE_WEAK BOOL_T tuya_ble_product_test_storage_special_data(UINT8_T type, UINT8_T *p_data, UINT32_T data_size)
{
    // chip platform related, implemented in tkl_flash.c file
    return FALSE;
}

__TUYA_BLE_WEAK BOOL_T tuya_ble_product_test_readback_special_data(UINT8_T type, UINT8_T *p_data, UINT32_T *data_size)
{
    // chip platform related, implemented in tkl_flash.c file
    return FALSE;
}

#if (TUYA_BLE_SECURE_CONNECTION_TYPE == TUYA_BLE_SECURE_CONNECTION_WITH_AUTH_KEY_ADVANCED_ENCRYPTION)

__TUYA_BLE_WEAK UINT32_T tuya_ble_get_dev_crt_len(VOID_T)
{
    return 0;
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_get_dev_crt_der(UINT8_T *p_der, UINT32_T der_len)
{
    return TUYA_BLE_ERR_COMMON;
}

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_storage_private_data(tuya_ble_private_data_type private_data_type, uint8_t *p_data, uint32_t data_size)
{
#if defined(TUYA_SDK_TEST) && (TUYA_SDK_TEST == 1)
    return TUYA_BLE_SUCCESS;
#else
    return TUYA_BLE_ERR_COMMON;
#endif
}

#endif // (TUYA_BLE_SECURE_CONNECTION_TYPE == TUYA_BLE_SECURE_CONNECTION_WITH_AUTH_KEY_ADVANCED_ENCRYPTION)

STATIC VOID_T tuya_ble_auc_write_comm_cfg(UINT8_T *para, UINT16_T len)
{
    CHAR_T reply_buf[256] = {0};
    UINT16_T reply_buf_len = 0;
    BOOL_T reply_ret = FALSE;

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC WRITE COMM CFG INFO!");

    INT32_T i;
    INT32_T type, size, offset, crc32;
    CHAR_T key[64] = {0};
    UINT8_T symbol_colon_index[32] = {0};
    UINT8_T symbol_comma_index[32] = {0};
    UINT8_T symbol_colon_cnt, symbol_comma_cnt;
    UINT16_T start_pos, end_pos, cut_len;
    UINT32_T calc_crc32;
    UINT32_T read_comm_cfg_len = 0;

    symbol_colon_cnt = tal_util_search_symbol_index((UINT8_T *)para, len, ':', symbol_colon_index);
    symbol_comma_cnt = tal_util_search_symbol_index((UINT8_T *)para, len, ',', symbol_comma_index);
    if (symbol_colon_cnt == 0 || symbol_comma_cnt == 0) {
        goto EXIT_ERR;
    }

    TUYA_BLE_LOG_DEBUG("symbol->: cnt=[%d]", symbol_colon_cnt);
    TUYA_BLE_LOG_HEXDUMP_DEBUG("symbol : list index", symbol_colon_index, symbol_colon_cnt);

    TUYA_BLE_LOG_DEBUG("symbol->, cnt=[%d]", symbol_comma_cnt);
    TUYA_BLE_LOG_HEXDUMP_DEBUG("symbol , list index", symbol_comma_index, symbol_comma_cnt);

    // type
    start_pos = symbol_colon_index[0] + 1;
    type = para[start_pos] - '0';
    TUYA_BLE_LOG_DEBUG("parse type ->[%d]", type);

    // key
    start_pos = symbol_colon_index[1] + 2;
    end_pos = (symbol_comma_cnt > 1) ? (symbol_comma_index[1] - 2) : (len - 3);
    cut_len = (end_pos - start_pos + 1);
    memcpy(key, &para[start_pos], cut_len);
    key[cut_len] = '\0';
    TUYA_BLE_LOG_DEBUG("parse key ->[%s]", key);

#if (TUYA_BLE_SECURE_CONNECTION_TYPE == TUYA_BLE_SECURE_CONNECTION_WITH_AUTH_KEY_ADVANCED_ENCRYPTION)
    if ((memcmp(key, COMM_CFG_KEY_DEV_CERTIFICATE, strlen(COMM_CFG_KEY_DEV_CERTIFICATE)) != 0) && \
        (memcmp(key, COMM_CFG_KEY_PRIVATE_KEY, strlen(COMM_CFG_KEY_PRIVATE_KEY)) != 0) && \
        (memcmp(key, COMM_CFG_KEY_SHORTURL, strlen(COMM_CFG_KEY_SHORTURL)) != 0) && \
        (memcmp(key, COMM_CFG_KEY_CALIBRATION, strlen(COMM_CFG_KEY_CALIBRATION)) != 0)) {
        TUYA_BLE_LOG_ERROR("WRITE COMM CFG, but key unknow.");
        goto EXIT_ERR;
    }
#else
    if ((memcmp(key, COMM_CFG_KEY_SHORTURL, strlen(COMM_CFG_KEY_SHORTURL)) != 0) && \
        (memcmp(key, COMM_CFG_KEY_CALIBRATION, strlen(COMM_CFG_KEY_CALIBRATION)) != 0)) {
        TUYA_BLE_LOG_ERROR("WRITE COMM CFG, but key unknow.");
        goto EXIT_ERR;
    }
#endif

    /* process different types, 1->size 2->value 3->crc32 0-write once 4-read once */
    if (type == 1) {
        /* data format
            "type":1,
            "key":"xxxx",
            "size":xxx
        */

        // size
        start_pos = symbol_colon_index[2] + 1;
        end_pos = len - 2;
        cut_len = (end_pos - start_pos + 1);
        tal_util_str_intstr2int_with_negative((CHAR_T *)&para[start_pos], cut_len, &size);
        TUYA_BLE_LOG_DEBUG("parse size ->[%d]", size);

        comm_cfg_total_len = size;
        comm_cfg_cur_rcv_len = 0;

        if (p_comm_cfg != NULL) {
            tuya_ble_free(p_comm_cfg);
        }

        p_comm_cfg = (UINT8_T *)tuya_ble_malloc(comm_cfg_total_len+16);
        if (p_comm_cfg == NULL) {
            TUYA_BLE_LOG_ERROR("WRITE COMM CFG, malloc failed.");
        } else {
            reply_ret = TRUE;
            tuya_ble_comm_cfg_fsm_state = COMMON_CFG_FSM_STATE_RCV_SIZE;
        }
    } else if (type == 2) {
        /* data format
            "type":2,
            "key":"xxxx",
            "value":"xxxx",
            "offset":xxxx
        */

        if ((tuya_ble_comm_cfg_fsm_state != COMMON_CFG_FSM_STATE_RCV_SIZE) || (p_comm_cfg == NULL)) {
            reply_ret = FALSE;
            goto EXIT_ERR;
        }

        // offset
        start_pos = symbol_colon_index[3] + 1;
        end_pos = len - 2;
        cut_len = (end_pos - start_pos + 1);
        tal_util_str_intstr2int_with_negative((CHAR_T *)&para[start_pos], cut_len, &offset);
        TUYA_BLE_LOG_DEBUG("parse offset ->[%d]", offset);

        // value
        start_pos = symbol_colon_index[2] + 2;
        end_pos = symbol_comma_index[2] - 2;
        cut_len = (end_pos - start_pos + 1);

        // replace tab character "\r"->'\r'   "\n"->'\n'
        UINT8_T *p_value = NULL;
        UINT16_T buf_i = 0;
        p_value = (UINT8_T *)tuya_ble_malloc(cut_len);

        for (i=start_pos; i<=end_pos; i++) {
            if ((para[i] == '\\') && (para[i+1] == 'r')) {
                p_value[buf_i++] = '\r';

                i += 1;
                cut_len -= 1;
                TUYA_BLE_LOG_DEBUG("find r ");
            } else if ((para[i] == '\\') && (para[i+1] == 'n')) {
                p_value[buf_i++] = '\n';

                i += 1;
                cut_len -= 1;
                TUYA_BLE_LOG_DEBUG("find n ");
            } else {
                p_value[buf_i++] = para[i];
            }
        }

        memcpy(&p_comm_cfg[offset], p_value, cut_len);
        tuya_ble_free(p_value);
        TUYA_BLE_LOG_DEBUG("WRITE COMM CFG offset=[%d] value_size=[%d]", offset, cut_len);

        comm_cfg_cur_rcv_len += cut_len;
        reply_ret = TRUE;
    } else if (type == 3) {
        /* data format
            "type":3,
            "key":"xxxx",
            "crc32":xxxx
        */

        if (comm_cfg_cur_rcv_len != comm_cfg_total_len) {
            TUYA_BLE_LOG_DEBUG("WRITE COMM CFG size err, recv=[%d] total=[%d]", comm_cfg_cur_rcv_len, comm_cfg_total_len);
            goto EXIT_ERR;
        }

        // crc32
        start_pos = symbol_colon_index[2] + 1;
        end_pos = len - 2;
        cut_len = (end_pos - start_pos + 1);
        tal_util_str_intstr2int_with_negative((CHAR_T *)&para[start_pos], cut_len, &crc32);
        TUYA_BLE_LOG_DEBUG("parse crc32 ->[%d]", crc32);

        calc_crc32 = tal_util_crc32(p_comm_cfg, comm_cfg_total_len, NULL);
        if (crc32 != calc_crc32) {
            TUYA_BLE_LOG_DEBUG("WRITE COMM CFG crc32 err, calc=[%d] crc32=[%d]", calc_crc32, crc32);
        } else {
            TUYA_BLE_LOG_DEBUG("---------WRITE COMM CFG write context success---------");
            TUYA_BLE_LOG_DEBUG("context size  = [%d]", comm_cfg_total_len);
            TUYA_BLE_LOG_DEBUG("context crc32 = [%d]", crc32);
            TUYA_BLE_LOG_HEXDUMP_DEBUG("context ", p_comm_cfg, comm_cfg_total_len);
            reply_ret = TRUE;

#if (TUYA_BLE_SECURE_CONNECTION_TYPE == TUYA_BLE_SECURE_CONNECTION_WITH_AUTH_KEY_ADVANCED_ENCRYPTION)
            /* write to security chip */
            if (memcmp(key, COMM_CFG_KEY_DEV_CERTIFICATE, strlen(COMM_CFG_KEY_DEV_CERTIFICATE)) == 0) {
                if (tuya_ble_storage_private_data(PRIVATE_DATA_DEV_CERT, p_comm_cfg, comm_cfg_total_len) != TUYA_BLE_SUCCESS) {
                    reply_ret = FALSE;
                }
            } else if (memcmp(key, COMM_CFG_KEY_PRIVATE_KEY, strlen(COMM_CFG_KEY_PRIVATE_KEY)) == 0) {
                UINT8_T private_key[64] = {0};
                UINT16_T private_key_len;

                if (tal_util_ecc_key_pem2hex((UINT8_T *)p_comm_cfg, private_key, &private_key_len) == 0) {
                    TUYA_BLE_LOG_DEBUG("tuya ble ecc key pem2hex failed");
                    reply_ret = FALSE;
                } else {
                    TUYA_BLE_LOG_HEXDUMP_DEBUG("private key raw", private_key, private_key_len);

                    if (tuya_ble_storage_private_data(PRIVATE_DATA_ECC_KEY, private_key, private_key_len) != TUYA_BLE_SUCCESS) {
                       reply_ret = FALSE;
                    }
                }
            }
#else
            reply_ret = FALSE;
#endif

            if (p_comm_cfg != NULL)
                tuya_ble_free(p_comm_cfg);
        }
    } else if (type == 0) {
        /* data format
            "type":0,
            "key":"xxxx",
            "value":xxxx
        */

        // value
        start_pos = symbol_comma_index[1] + 1;
        end_pos = len - 2;
        cut_len = (end_pos - start_pos + 1);
        TUYA_BLE_LOG_HEXDUMP_DEBUG("write once value", &para[start_pos], cut_len);
        reply_ret = FALSE;

        if (memcmp(key, COMM_CFG_KEY_SHORTURL, strlen(COMM_CFG_KEY_SHORTURL)) == 0) {
            // data format: "vaule":"https://t.tuya.com/AYtKKxLV", "encode":"utf8"
            if (tuya_ble_product_test_storage_common_data(COMMON_DATA_SHORTURL, &para[start_pos], cut_len) == TUYA_BLE_SUCCESS) {
                reply_ret = TRUE;
            }
        } else if (memcmp(key, COMM_CFG_KEY_CALIBRATION, strlen(COMM_CFG_KEY_CALIBRATION)) == 0) {
            // data format: "value":"{\"calibration_file\":\"256\"}"
            INT32_T num;

            if (sscanf((CHAR_T *)&para[start_pos], "\"value\":\"{\\\"calibration_file\\\":\\\"%d\\\"}\"", &num) != 1) {
                TUYA_BLE_LOG_DEBUG("xo calibration data format error.");
            } else if (tuya_ble_product_test_storage_special_data(SPECIAL_DATA_XO_CALIBRATION, &para[start_pos], cut_len) == TRUE) {
                reply_ret = TRUE;
            }
        }
    } else if (type == 4) {
        /* data format
            "type":4,
            "key":"xxxx",
        */

        reply_ret = FALSE;
        reply_buf_len = sprintf((char *)reply_buf, "{\"ret\":true,\"key\":\"%s\",", key);

        if (memcmp(key, COMM_CFG_KEY_SHORTURL, strlen(COMM_CFG_KEY_SHORTURL)) == 0) {
            if (tuya_ble_product_test_readback_common_data(COMMON_DATA_SHORTURL, (uint8_t *)&reply_buf[reply_buf_len], &read_comm_cfg_len) == TUYA_BLE_SUCCESS) {
                reply_ret = TRUE;
            }
        } else if (memcmp(key, COMM_CFG_KEY_CALIBRATION, strlen(COMM_CFG_KEY_CALIBRATION)) == 0) {
            if (tuya_ble_product_test_readback_special_data(SPECIAL_DATA_XO_CALIBRATION, (uint8_t *)&reply_buf[reply_buf_len], &read_comm_cfg_len) == TRUE) {
                reply_ret = TRUE;
            }
        }

        // direct reply
        if (reply_ret) {
            reply_buf_len += read_comm_cfg_len;
            reply_buf[reply_buf_len++] = '}';

            tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_COMM_CFG, (uint8_t *)reply_buf, reply_buf_len);
            TUYA_BLE_LOG_HEXDUMP_DEBUG("read once reply", (uint8_t *)reply_buf, reply_buf_len);
            TUYA_BLE_LOG_DEBUG("AUC READ COMM CFG responsed successed.");
            return;
        }
    } else {
        TUYA_BLE_LOG_ERROR("WRITE COMM CFG type err. [%d]", type);
    }

EXIT_ERR:
    if (reply_ret) {
        reply_buf_len = sprintf((CHAR_T *)reply_buf, "{\"ret\":true,\"key\":\"%s\"}", key);
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_COMM_CFG, (UINT8_T *)reply_buf, reply_buf_len);

        TUYA_BLE_LOG_DEBUG("AUC WRITE COMM CFG responsed successed.");
    } else {
        reply_buf_len = sprintf((CHAR_T *)reply_buf, "{\"ret\":false,\"key\":\"%s\"}", key);
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_COMM_CFG, (UINT8_T *)reply_buf, reply_buf_len);

        TUYA_BLE_LOG_DEBUG("AUC WRITE COMM CFG failed!");

        if (p_comm_cfg != NULL)
            tuya_ble_free(p_comm_cfg);

        tuya_ble_comm_cfg_fsm_state = COMMON_CFG_FSM_STATE_IDLE;
    }
}

STATIC VOID_T tuya_ble_auc_query_info(UINT8_T *para, UINT16_T len)
{
    UINT16_T i=0;
    UINT8_T *alloc_buf = NULL;

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC QUERY INFO!");

    alloc_buf = (UINT8_T *)tuya_ble_malloc(256);
    if (alloc_buf) {
        memset(alloc_buf, 0, 256);
    } else {
        TUYA_BLE_LOG_ERROR("AUC QUERY INFO alloc buf malloc failed.");
        return;
    }

    alloc_buf[i++] = '{';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "ret", 3);
    i += 3;
    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ':';
    memcpy(&alloc_buf[i], "true", 4);
    i += 4;

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "auzKey", 6);
    i += 6;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], tuya_ble_current_para.auth_settings.auth_key, AUTH_KEY_LEN);
    i += AUTH_KEY_LEN;

    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "hid", 3);
    i += 3;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], tuya_ble_current_para.auth_settings.h_id, H_ID_LEN);
    i += 19;
    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "uuid", 4);
    i += 4;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], tuya_ble_current_para.auth_settings.device_id, DEVICE_ID_LEN);
    i += DEVICE_ID_LEN;
    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "mac", 3);
    i += 3;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';

    memcpy(&alloc_buf[i], tuya_ble_current_para.auth_settings.mac_string, MAC_LEN*2);
    i += MAC_LEN*2;
    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "firmName", 8);
    i += 8;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], tal_common_info.p_firmware_name, strlen((VOID_T*)tal_common_info.p_firmware_name));
    i += strlen((VOID_T*)tal_common_info.p_firmware_name);
    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "firmVer", 7);
    i += 7;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], tal_common_info.p_firmware_version, strlen((VOID_T*)tal_common_info.p_firmware_version));
    i += strlen((VOID_T*)tal_common_info.p_firmware_version);
    alloc_buf[i++] = '\"';

    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "prod_test", 9);
    i += 9;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';

    memcpy(&alloc_buf[i], "false", 5);
    i += 5;

#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)
    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';
    memcpy(&alloc_buf[i], "pid", 3);
    i += 3;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';

    memcpy(&alloc_buf[i], tuya_ble_current_para.auth_settings.factory_pid, tuya_ble_current_para.auth_settings.pid_len);
    i += tuya_ble_current_para.auth_settings.pid_len;
    alloc_buf[i++] = '\"';
#endif

#if TUYA_BLE_WRITE_BT_MAC
    alloc_buf[i++] = ',';
    alloc_buf[i++] = '\"';

    memcpy(&alloc_buf[i], "bt_mac", 6);

    i += 6;
    alloc_buf[i++] = '\"';
    alloc_buf[i++] = ':';
    alloc_buf[i++] = '\"';

    memcpy(&alloc_buf[i], tuya_ble_current_para.auth_settings.bt_mac_string, MAC_LEN*2);
    i += MAC_LEN*2;
    alloc_buf[i++] = '\"';
#endif

    alloc_buf[i++] = '}';
    alloc_buf[i++] = 0;

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_QUERY_INFO, (UINT8_T *)alloc_buf, i - 1);

    TUYA_BLE_LOG_DEBUG("AUC_CMD_QUERY_INFO RESPONSE!");

    tuya_ble_free(alloc_buf);
}

STATIC VOID_T tuya_ble_auc_reset(UINT8_T *para, UINT16_T len)
{
    UINT8_T buf[1] = {0x00};

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("auc RESET!");

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_RESET, buf, SIZEOF(buf));

    tuya_ble_device_delay_ms(1000);

    tuya_ble_device_reset();
}

STATIC VOID_T tuya_ble_auc_write_hid(UINT8_T *para, UINT16_T len)
{
    UINT8_T hid[19];

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC WRITE AUTH HID!");

    if (len < 27) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_HID, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("WRITE AUTH HID para length error!");
        return;
    }

    if (memcmp(&para[2], "hid", 3) != 0) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_HID, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("WRITE AUTH HID para error!");
        return;
    }

    memcpy(hid, &para[8], H_ID_LEN);

    if (tuya_ble_storage_write_hid(hid, H_ID_LEN) == TUYA_BLE_SUCCESS) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_HID, (UINT8_T *)true_buf, strlen(true_buf));
        TUYA_BLE_LOG_DEBUG("WRITE AUTH HID successed.");
    } else {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_HID, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("WRITE AUTH HID failed.");
    }
}

STATIC VOID_T tuya_ble_auc_query_fingerprint(UINT8_T *para, UINT16_T len)
{
    INT32_T length = 0;
    UINT8_T *alloc_buf = NULL;

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC QUERY FINGERPRINT!");

    alloc_buf = (UINT8_T *)tuya_ble_malloc(256);

    if (alloc_buf) {
        memset(alloc_buf, 0, 256);
    } else {
        TUYA_BLE_LOG_ERROR("AUC QUERY INFO alloc buf malloc failed.");
        return;
    }

    length = sprintf((CHAR_T *)alloc_buf, "{\"ret\":true,\"firmName\":\"%s\",\"firmVer\":\"%s\"}", tal_common_info.p_firmware_name, tal_common_info.p_firmware_version);

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_QUERY_FINGERPRINT, alloc_buf, length);

    tuya_ble_free(alloc_buf);

    TUYA_BLE_LOG_DEBUG("AUC_CMD_QUERY_FINGERPRINT responsed.");
}

STATIC VOID_T tuya_ble_auc_rssi_test(UINT8_T *para, UINT16_T len)
{
    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    memset(tuya_bft_rf_test.periph_name, 0x00, SIZEOF(tuya_bft_rf_test.periph_name));
    if (memcmp(&para[9], "none", 4) == 0) {
        tuya_bft_rf_test.periph_name_len = SIZEOF("ty_mdev") - 1;
        memcpy(tuya_bft_rf_test.periph_name, "ty_mdev", tuya_bft_rf_test.periph_name_len);
    } else {
        UINT8_T i, j;
        for (i=0, j=9; i<SIZEOF(tuya_bft_rf_test.periph_name); i++, j++) {
            if (para[j] == '"') {
                break;
            }

            tuya_bft_rf_test.periph_name[i] = para[j];
        }
        tuya_bft_rf_test.periph_name_len = i;
    }

    if (tuya_bft_rf_test.is_start == FALSE) {
        tuya_bft_rf_test.count =  0;
        tuya_bft_rf_test.is_start = TRUE;
        tuya_ble_prod_beacon_scan_start();
        tuya_ble_prod_beacon_scan_timer_init();
        tuya_ble_prod_beacon_scan_timer_start();
    }
}

STATIC VOID_T tuya_ble_auc_read_mac(UINT8_T *para, UINT16_T len)
{
    (VOID_T)(para);
    (VOID_T)(len);

    INT32_T length = 0;
    UINT8_T buf[64];
    UINT8_T mac_addr_str[12 + 1];

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC READ MAC ADDR!");

    // MAC string format: DC 23 xx xx xx xx
    memcpy(mac_addr_str, tuya_ble_current_para.auth_settings.mac_string, MAC_LEN*2);
    mac_addr_str[12] = '\0';

    int swap_len = MAC_LEN*2;
    CHAR_T temp[2];

    for (int i=0; i<swap_len/2; i+=2) {
        temp[0] = mac_addr_str[i];
        temp[1] = mac_addr_str[i+1];

        mac_addr_str[i] = mac_addr_str[swap_len-2-i];
        mac_addr_str[i+1] = mac_addr_str[swap_len-1-i];

        mac_addr_str[swap_len-2-i] = temp[0];
        mac_addr_str[swap_len-1-i] = temp[1];
    }

    length = sprintf((VOID_T*)buf, "{\"ret\":true,\"mac\":\"%s\"}", mac_addr_str);

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_READ_MAC, buf, length);

    TUYA_BLE_LOG_DEBUG("AUC_CMD_READ_MAC responsed.");
}

STATIC VOID_T tuya_ble_auc_exit(UINT8_T *para, UINT16_T len)
{
    (VOID_T)(para);
    (VOID_T)(len);

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC EXIT!");

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_EXIT, (UINT8_T *)true_buf, strlen(true_buf));

    tuya_ble_device_delay_ms(1000);
    tuya_ble_device_reset();
}

#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)

__TUYA_BLE_WEAK tuya_ble_status_t tuya_ble_prod_storage_oem_info(UINT8_T *para, UINT16_T len)
{
    return TUYA_BLE_SUCCESS;
}

STATIC VOID_T tuya_ble_auc_write_oem_info(UINT8_T *para, UINT16_T len)
{
    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC WRITE OEM INFO!");

    if (len == 0) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_OEM_INFO, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("WRITE OEM INFO para length error!");
        return;
    }

    if ((write_subpkg_info.need_subpkg == FALSE) && (write_subpkg_info.total_nums == 0)) {
        // need not subpkg
        if ((para[0] != '{') || (para[len-1] != '}')) {
            tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_OEM_INFO, (UINT8_T *)false_buf, strlen(false_buf));
            TUYA_BLE_LOG_ERROR("WRITE OEM INFO failed.");
            return;
        }

        if (tuya_ble_prod_storage_oem_info(para, len) == TUYA_BLE_SUCCESS) {
            tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_OEM_INFO, (UINT8_T *)true_buf, strlen(true_buf));
            TUYA_BLE_LOG_DEBUG("WRITE OEM INFO successed.");
        } else {
            tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_OEM_INFO, (UINT8_T *)false_buf, strlen(false_buf));
            TUYA_BLE_LOG_ERROR("WRITE OEM INFO failed.");
        }
    } else {
        UINT16_T cur_subpkg = (para[0] << 8) + para[1];

        if (cur_subpkg < write_subpkg_info.total_nums) {
            memcpy(write_subpkg_info.p_file_data + write_subpkg_info.rcv_file_len, para+2, len-2);
            write_subpkg_info.rcv_file_len += len-2;

            tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_OEM_INFO, (UINT8_T *)true_buf, strlen(true_buf));
            TUYA_BLE_LOG_DEBUG("WRITE OEM INFO subpkg successed.");
        } else {
            tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_OEM_INFO, (UINT8_T *)false_buf, strlen(false_buf));
            TUYA_BLE_LOG_ERROR("WRITE OEM INFO subpkg failed.");
        }
    }
}

STATIC VOID_T tuya_ble_auc_write_subpkg_start(UINT8_T *para, UINT16_T len)
{
    UINT8_T cmd;
    INT32_T packageNum, crc32;
    UINT8_T symbol_colon_index[8] = {0};
    UINT8_T symbol_comma_index[8] = {0};
    UINT8_T symbol_colon_cnt, symbol_comma_cnt;
    UINT16_T start_pos, end_pos, cut_len;

    symbol_colon_cnt = tal_util_search_symbol_index((VOID_T *)para, len, ':', symbol_colon_index);
    symbol_comma_cnt = tal_util_search_symbol_index((VOID_T *)para, len, ',', symbol_comma_index);

    /* check json filed */
    if ((symbol_colon_cnt == 0 || symbol_comma_cnt == 0) || \
       (memcmp(&para[2], "cmd", strlen("cmd")) != 0) || \
       (memcmp(&para[symbol_comma_index[0]+2], "packageNum", strlen("packageNum")) != 0) || \
       (memcmp(&para[symbol_comma_index[1]+2], "crc32", strlen("crc32")) != 0) ) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_SUBPKG_START, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC WRITE SUBPKG START format err failed.");
        return;
    }

    /* cmd */
    start_pos = symbol_colon_index[0] + 2;
    end_pos = symbol_comma_index[0] - 2;
    cut_len = (end_pos - start_pos + 1);
    tal_util_str_hexstr2hexarray((UINT8_T *)&para[start_pos], cut_len, &cmd);
    TUYA_BLE_LOG_DEBUG("parse cmd ->[0x%02x]", cmd);

    /* packageNum */
    start_pos = symbol_colon_index[1] + 1;
    end_pos = symbol_comma_index[1] - 1;
    cut_len = (end_pos - start_pos + 1);
    tal_util_str_intstr2int_with_negative((CHAR_T *)&para[start_pos], cut_len, &packageNum);
    TUYA_BLE_LOG_DEBUG("parse packageNum ->[%d]", packageNum);

    /* crc32 */
    start_pos = symbol_colon_index[2] + 1;
    end_pos = len - 2;
    cut_len = (end_pos - start_pos + 1);
    tal_util_str_intstr2int_with_negative((CHAR_T *)&para[start_pos], cut_len, &crc32);
    TUYA_BLE_LOG_DEBUG("parse crc32 ->[%d]", crc32);

    write_subpkg_info.need_subpkg = TRUE;
    write_subpkg_info.file_type   = cmd; //!< "09"-oem ocnfig data "10"-homekit atoken data. format: hexstring
    write_subpkg_info.total_nums  = packageNum;
    write_subpkg_info.file_crc32  = crc32;

    write_subpkg_info.p_file_data = (UINT8_T *)tuya_ble_malloc(240*write_subpkg_info.total_nums);
    if (write_subpkg_info.p_file_data == NULL) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_SUBPKG_START, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_ERROR("AUC WRITE SUBPKG START alloc buf malloc failed.");
        return;
    }

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_SUBPKG_START, (UINT8_T *)true_buf, strlen(true_buf));
    TUYA_BLE_LOG_DEBUG("AUC WRITE SUBPKG START successed.");
}

STATIC VOID_T tuya_ble_auc_write_subpkg_end(UINT8_T *para, UINT16_T len)
{
    UINT32_T calc_crc32;

    /* check received file whether correct */
    calc_crc32 = tal_util_crc32(write_subpkg_info.p_file_data, write_subpkg_info.rcv_file_len, NULL);
    if (calc_crc32 != write_subpkg_info.file_crc32) {
        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_SUBPKG_END, (UINT8_T *)false_buf, strlen(false_buf));
        TUYA_BLE_LOG_DEBUG("AUC WRITE SUBPKG END crc32 check failed.");
    } else {
        if (write_subpkg_info.file_type == 0x09) {
            /* oem config data */
            tuya_ble_prod_storage_oem_info(write_subpkg_info.p_file_data, write_subpkg_info.rcv_file_len);
        } else if (write_subpkg_info.file_type == 0x10) {
            /* homekit atoken data */
            // TODO ..
        }

        tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_SUBPKG_END, (UINT8_T *)true_buf, strlen(true_buf));
        TUYA_BLE_LOG_DEBUG("AUC WRITE SUBPKG END successed.");
    }

    /* Clear progress vars, and free p_file_data*/
    write_subpkg_info.need_subpkg   = FALSE;
    write_subpkg_info.file_crc32    = 0;
    write_subpkg_info.total_nums    = 0;
    write_subpkg_info.file_type     = 0;
    write_subpkg_info.rcv_file_len  = 0;

    tuya_ble_free(write_subpkg_info.p_file_data);
}

#endif

STATIC VOID_T tuya_ble_auc_write_country_code(UINT8_T *para, UINT16_T len)
{
    UINT8_T ret = 0;
    UINT8_T data_send[16];
    UINT8_T buf_len = 0;
    UINT8_T key[] = "country";
    UINT8_T key1[] = "ble_ch";
    UINT8_T value_data[10] = {0};
    UINT16_T value_len = 0;
    UINT32_T tx_dBm = 0;
    UINT8_T country_code = 0;

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC WRITE COUNTRY CODE!");

    tal_util_get_value_by_key(para, len, key, strlen((char*)key), value_data, &value_len);

    ret = tal_util_get_value_by_key_to_int(para, len, key1, strlen((char*)key1), &tx_dBm);

    memcpy(data_send, true_buf, strlen(true_buf));
    buf_len = strlen(true_buf);

    if (0 == memcmp(value_data, "CN", value_len)) {
        country_code = COUNTRY_CODE_CN;
    } else if (0 == memcmp(value_data, "US", value_len)) {
        country_code = COUNTRY_CODE_US;
    } else if (0 == memcmp(value_data, "JP", value_len)) {
        country_code = COUNTRY_CODE_JP;
    } else if (0 == memcmp(value_data, "JP1", value_len)) {
        country_code = COUNTRY_CODE_JP1;
    } else if (0 == memcmp(value_data, "EU", value_len)) {
        country_code = COUNTRY_CODE_EU;
    } else {
        country_code = COUNTRY_CODE_CN;
        if (ret == 0) {
            memcpy(data_send, false_buf, 13);
            buf_len = strlen(false_buf);
        }
    }

    if (tuya_ble_storage_write_rf_param(country_code, tx_dBm) != TUYA_BLE_SUCCESS) {
        memcpy(data_send, false_buf, 13);
        buf_len = strlen(false_buf);
    }

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_WRITE_CONUTRY_CODE, data_send, buf_len);
}

STATIC VOID_T tuya_ble_auc_read_country_code(UINT8_T *para, UINT16_T len)
{
    UINT8_T data_send[128] = {0};
    UINT8_T code[10] = {0};
    int tx_dBm = tuya_ble_current_para.auth_settings.tx_dBm;
    UINT8_T country_code = tuya_ble_current_para.auth_settings.country_code;

    if (tuya_ble_production_test_flag != 1) {
        return;
    }

    TUYA_BLE_LOG_DEBUG("AUC READ COUNTRY CODE!");

    switch(country_code){
        case COUNTRY_CODE_CN:
            strcpy((char*)code, "CN");
        break;

        case COUNTRY_CODE_US:
            strcpy((char*)code, "US");
        break;

        case COUNTRY_CODE_JP:
            strcpy((char*)code, "JP");
        break;

        case COUNTRY_CODE_EU:
            strcpy((char*)code, "EU");
        break;

        case COUNTRY_CODE_JP1:
            strcpy((char*)code, "JP1");
        break;

        default:
        break;
    }

    if (tx_dBm == 0) {
        sprintf((char*)data_send, "{\"ret\":true,\"country\":\"%s\"}", code);
    } else {
        sprintf((char*)data_send, "{\"ret\":true,\"info\":\"{\\\"country\\\":\\\"%s\\\",\\\"mode_type\\\":%d,\\\"ble_ch\\\":%d}\"}", code, 1, tx_dBm);
    }

    tuya_ble_uart_prod_send(TUYA_BLE_AUC_CMD_READ_CONUTRY_CODE, data_send, strlen((char*)data_send));
}

__TUYA_BLE_WEAK VOID_T tuya_ble_app_sdk_test_process(UINT8_T channel, UINT8_T *p_in_data, UINT16_T in_len)
{
    UINT16_T sub_cmd = 0;
    UINT8_T *data_buffer = NULL;
    UINT16_T data_len = ((p_in_data[4]<<8) + p_in_data[5]);

    (void)data_buffer;

    if ((p_in_data[6] != 3)||(data_len<3)) {
        return;
    }

    sub_cmd = (p_in_data[7] << 8) + p_in_data[8];
    data_len -= 3;

    if (data_len > 0) {
        data_buffer = p_in_data+9;
    }

    switch (sub_cmd) {
        default:
            break;
    }
}

VOID_T tuya_ble_app_production_test_process(UINT8_T channel, UINT8_T *p_in_data, UINT16_T in_len)
{
    UINT8_T  cmd = p_in_data[3];
    UINT16_T data_len = (p_in_data[4]<<8) + p_in_data[5];
    UINT8_T* data_buffer = p_in_data+6;

//    if (tuya_ble_current_para.sys_settings.factory_test_flag == 0) {
//        TUYA_BLE_LOG_WARNING("The production interface is closed!");
//        return;
//    }

    if ((channel != 0) && (cmd != TUYA_BLE_AUC_CMD_EXTEND)) {
        TUYA_BLE_LOG_ERROR("The authorization instructions are not supported in non-serial channels!");
        return;
    }

    if ((channel == 1) && (cmd == TUYA_BLE_AUC_CMD_EXTEND)) {
        if (tuya_ble_production_test_with_ble_flag == 0) {

            tuya_ble_production_test_with_ble_flag = 1;
            if (tuya_ble_connect_status_get() != BONDING_CONN) {
                tuya_ble_connect_monitor_timer_stop();
            }
        }
    }

    switch (cmd) {
        case TUYA_BLE_AUC_CMD_EXTEND:
            tuya_ble_custom_app_production_test_process(channel, p_in_data, in_len);
            break;
        case TUYA_BLE_SDK_TEST_CMD_EXTEND:
            tuya_ble_app_sdk_test_process(channel, p_in_data, in_len);
            break;
        case TUYA_BLE_AUC_CMD_ENTER:
            tuya_ble_auc_enter(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_QUERY_HID:
            tuya_ble_auc_query_hid(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_GPIO_TEST:
            tuya_ble_auc_gpio_test(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_WRITE_AUTH_INFO:
            tuya_ble_auc_write_auth_info(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_QUERY_INFO:
            tuya_ble_auc_query_info(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_RESET:
            tuya_ble_auc_reset(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_QUERY_FINGERPRINT:
            tuya_ble_auc_query_fingerprint(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_WRITE_HID:
            tuya_ble_auc_write_hid(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_RSSI_TEST:
            tuya_ble_auc_rssi_test(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_READ_MAC:
            tuya_ble_auc_read_mac(data_buffer, data_len);
            break;

        case TUYA_BLE_AUC_CMD_EXIT:
            tuya_ble_auc_exit(data_buffer, data_len);
            break;

#if (TUYA_BLE_PROD_SUPPORT_OEM_TYPE == TUYA_BLE_PROD_OEM_TYPE_0_5)

        case TUYA_BLE_AUC_CMD_WRITE_OEM_INFO:
            tuya_ble_auc_write_oem_info(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_WRITE_SUBPKG_START:
            tuya_ble_auc_write_subpkg_start(data_buffer, data_len);
            break;
        case TUYA_BLE_AUC_CMD_WRITE_SUBPKG_END:
            tuya_ble_auc_write_subpkg_end(data_buffer, data_len);
            break;

#endif
        case TUYA_BLE_AUC_CMD_WRITE_CONUTRY_CODE:
            tuya_ble_auc_write_country_code(data_buffer, data_len);
            break;

        case TUYA_BLE_AUC_CMD_READ_CONUTRY_CODE:
            tuya_ble_auc_read_country_code(data_buffer, data_len);
            break;

        case TUYA_BLE_AUC_CMD_WRITE_COMM_CFG:
            tuya_ble_auc_write_comm_cfg(data_buffer, data_len);
            break;

        default:
            break;
    }
}

#else

VOID_T tuya_ble_app_production_test_process(UINT8_T channel, UINT8_T *p_in_data, UINT16_T in_len)
{
    UINT8_T cmd = p_in_data[3];
    UINT16_T data_len = (p_in_data[4]<<8) + p_in_data[5];
    UINT8_T *data_buffer = p_in_data+6;

    switch (cmd) {
        default:
            break;
    };
}

UINT8_T tuya_ble_internal_production_test_with_ble_flag_get(VOID_T)
{
    return 0;
}

#endif

tuya_ble_status_t tuya_ble_ecc_keypair_gen_secp256r1(UINT8_T *public_key, UINT8_T *private_key)
{
    return OPRT_NOT_SUPPORTED;
}

tuya_ble_status_t tuya_ble_ecc_shared_secret_compute_secp256r1(UINT8_T *public_key, UINT8_T *private_key, UINT8_T *secret_key)
{
    return OPRT_NOT_SUPPORTED;
}

#endif // TUYA_BLE_FEATURE_PRODUCT_TEST_ENABLE

