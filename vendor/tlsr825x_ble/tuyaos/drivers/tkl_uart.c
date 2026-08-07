/**
 * @file tkl_uart.c
 * @brief This is tkl_uart file
 * @version 1.0
 * @date 2021-09-10
 *
 * @copyright Copyright 2021-2023 Tuya Inc. All Rights Reserved.
 *
 */

// chip sdk
#include "drivers.h"
#include "board.h"

// tuya os sdk
#include "tkl_uart.h"
#include "tkl_gpio.h"

// app

/***********************************************************************
 ********************* constant ( macro and enum ) *********************
 **********************************************************************/
#define TKL_UART_PORT_NUM0      0
#define UART_DATA_LEN           252    // data max 252

#if FAQIUJI
#define TKL_UART_TX_PIN_ID       9     // GPIO_PB1
#define TKL_UART_RX_PIN_ID       0     // GPIO_PA0

#else
#define TKL_UART_TX_PIN_ID       9     // GPIO_PB1
#define TKL_UART_RX_PIN_ID       15    // GPIO_PB7

#endif

/***********************************************************************
 ********************* struct ******************************************
 **********************************************************************/
typedef struct {
    UINT32_T len;        // data max 252
    UINT8_T data[UART_DATA_LEN];
} uart_data_t;

typedef struct {
    UINT32_T tx;
    UINT32_T rx;
} TKL_UART_GPIO_T;

/***********************************************************************
 ********************* variable ****************************************
 **********************************************************************/
_attribute_no_ret_bss_ uart_data_t T_rxdata_buf;   // data max 252, user must copy rxdata to other Ram,but not use directly
_attribute_no_ret_bss_ UINT8_T T_txdata_buf[UART_DATA_LEN];      // not for user
UINT8_T uart_rx_true;

STATIC volatile bool_t tkl_uart_is_init = FALSE;
STATIC TUYA_UART_IRQ_CB tkl_uart_recv_cb = NULL;
STATIC TUYA_UART_IRQ_CB tkl_uart_tx_finish_cb = NULL;

STATIC TKL_UART_GPIO_T tkl_uart_gpio = {
    .tx = TKL_UART_TX_PIN_ID,
    .rx = TKL_UART_RX_PIN_ID,
};

/***********************************************************************
 ********************* function ****************************************
 **********************************************************************/
_attribute_ram_code_ OPERATE_RET hal_uart_irq_handler(VOID_T)
{
    unsigned char irqS = reg_dma_rx_rdy0;
    if (irqS & FLD_DMA_CHN_UART_RX) {
        uart_rx_true = 1;
        if (tkl_uart_recv_cb != NULL) {
            tkl_uart_recv_cb(TKL_UART_PORT_NUM0);
        }
        reg_dma_rx_rdy0 = FLD_DMA_CHN_UART_RX;
    }

    if (irqS & FLD_DMA_CHN_UART_TX) {
        reg_dma_rx_rdy0 = FLD_DMA_CHN_UART_TX;
        if (tkl_uart_tx_finish_cb != NULL) {
            tkl_uart_tx_finish_cb(TKL_UART_PORT_NUM0);
        }
    }
    return OPRT_OK;
}

OPERATE_RET tkl_uart_init(TUYA_UART_NUM_E port_id, TUYA_UART_BASE_CFG_T *cfg)
{
    OPERATE_RET ret = OPRT_OK;
    if (cfg == NULL) {
        return OPRT_INVALID_PARM;
    }

    if (port_id != TUYA_UART_NUM_0) {
        return OPRT_NOT_SUPPORTED;
    }
    uart_recbuff_init((unsigned char *)(&T_rxdata_buf), sizeof(T_rxdata_buf), T_txdata_buf);

    UINT32_T gpio_tlsr_tx = tkl_gpio_to_tlsr_gpio(tkl_uart_gpio.tx);
    UINT32_T gpio_tlsr_rx = tkl_gpio_to_tlsr_gpio(tkl_uart_gpio.rx);
    uart_gpio_set(gpio_tlsr_tx, gpio_tlsr_rx);
    uart_reset();

    UART_ParityTypeDef parity;
    UART_StopBitTypeDef stopbits;
    switch (cfg->parity) {
        case TUYA_UART_PARITY_TYPE_NONE:
            parity = PARITY_NONE;
        break;
        case TUYA_UART_PARITY_TYPE_ODD:
            parity = PARITY_ODD;
        break;
        case TUYA_UART_PARITY_TYPE_EVEN:
            parity = PARITY_EVEN;
        break;
        default:
            return OPRT_NOT_SUPPORTED;
        break;
    }
    switch (cfg->stopbits) {
        case TUYA_UART_STOP_LEN_1BIT:
            stopbits = STOP_BIT_ONE;
        break;
        case TUYA_UART_STOP_LEN_1_5BIT1:
            stopbits = STOP_BIT_ONE_DOT_FIVE;
        break;
        case TUYA_UART_STOP_LEN_2BIT:
            stopbits = STOP_BIT_TWO;
        break;
        default:
            return OPRT_NOT_SUPPORTED;
        break;
    }

    if (19200 == cfg->baudrate) {

#if (CLOCK_SYS_CLOCK_HZ == 16000000)
        uart_init(118, 13, parity, stopbits); //baud rate: 19200
#elif (CLOCK_SYS_CLOCK_HZ == 24000000)
        uart_init(124, 9, parity, stopbits); //baud rate: 19200
#elif (CLOCK_SYS_CLOCK_HZ == 32000000)
        uart_init(235, 6, parity, stopbits); //baud rate: 19200
#elif (CLOCK_SYS_CLOCK_HZ == 48000000)
        uart_init(249, 9, parity, stopbits); //baud rate: 19200
#else
        #error " CLOCK_SYS_CLOCK_HZ VALUE SET ERROR"
#endif

    }
    else if (9600 == cfg->baudrate) {

#if (CLOCK_SYS_CLOCK_HZ == 16000000)
        uart_init(118, 13, parity, stopbits); //baud rate: 9600
#elif (CLOCK_SYS_CLOCK_HZ == 24000000)
        uart_init(249, 9, parity, stopbits); //baud rate: 9600
#elif (CLOCK_SYS_CLOCK_HZ == 32000000)
        uart_init(235, 13, parity, stopbits); //baud rate: 9600
#elif (CLOCK_SYS_CLOCK_HZ == 48000000)
        uart_init(499, 9, parity, stopbits); //baud rate: 9600
#else
#error " CLOCK_SYS_CLOCK_HZ VALUE SET ERROR"
#endif

    }
    else if (115200 == cfg->baudrate) {

#if (CLOCK_SYS_CLOCK_HZ == 16000000)
        uart_init(9,  13, parity, stopbits); //baud rate: 115200
#elif (CLOCK_SYS_CLOCK_HZ == 24000000)
        uart_init(12, 15, parity, stopbits); //baud rate: 115200
#elif (CLOCK_SYS_CLOCK_HZ == 32000000)
        uart_init(17, 13, parity, stopbits); //baud rate: 115200
#elif (CLOCK_SYS_CLOCK_HZ == 48000000)
        uart_init(25, 15, parity, stopbits); //baud rate: 115200
#else
#error " CLOCK_SYS_CLOCK_HZ VALUE SET ERROR"
#endif

    }
    else {
        uart_init_baudrate(cfg->baudrate, CLOCK_SYS_CLOCK_HZ, parity, stopbits);
    }
    uart_dma_enable(1, 1);
    irq_set_mask(FLD_IRQ_DMA_EN);
    dma_chn_irq_enable(FLD_DMA_CHN_UART_RX | FLD_DMA_CHN_UART_TX, 1);
    tkl_uart_is_init = TRUE;

    return ret;
}

OPERATE_RET tkl_uart_deinit(TUYA_UART_NUM_E port_id)
{
    if(TUYA_UART_NUM_0 != port_id){
        return OPRT_NOT_SUPPORTED;
    }

    if(!tkl_uart_is_init) {
        return OPRT_NOT_SUPPORTED;
    }

    UINT32_T gpio_tlsr_tx = tkl_gpio_to_tlsr_gpio(tkl_uart_gpio.tx);
    UINT32_T gpio_tlsr_rx = tkl_gpio_to_tlsr_gpio(tkl_uart_gpio.rx);
    gpio_set_func(gpio_tlsr_tx,AS_GPIO); // set tx pin
    gpio_set_func(gpio_tlsr_rx,AS_GPIO); // set rx pin
    
    dma_chn_irq_enable(FLD_DMA_CHN_UART_RX | FLD_DMA_CHN_UART_TX, 0);
    uart_dma_enable(0, 0);
    uart_reset();
    tkl_uart_is_init = FALSE;

    return OPRT_OK;
}

INT32_T tkl_uart_write(TUYA_UART_NUM_E port_id, VOID_T *buff, UINT16_T len)
{
    if (TUYA_UART_NUM_0 != port_id) {
        return OPRT_NOT_SUPPORTED;
    }
    if (!tkl_uart_is_init) {
        return OPRT_NOT_FOUND;
    }
    if (len > UART_DATA_LEN) {
        return OPRT_INVALID_PARM;
    }
    if (buff == NULL) {
        return OPRT_INVALID_PARM;
    }
    if (uart_Send(buff, len)) {
        return len;
    }
    else {
        return OPRT_OK;
    }
}

VOID_T tkl_uart_rx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB rx_cb)
{
    if (!tkl_uart_is_init) {
        return;
    }

    if (port_id != TUYA_UART_NUM_0) {
        return;
    }

    tkl_uart_recv_cb = rx_cb;
}

VOID_T tkl_uart_tx_irq_cb_reg(TUYA_UART_NUM_E port_id, TUYA_UART_IRQ_CB tx_cb)
{
    if (!tkl_uart_is_init) {
        return;
    }

    if (port_id != TUYA_UART_NUM_0) {
        return;
    }

    tkl_uart_tx_finish_cb = tx_cb;
}

INT32_T tkl_uart_read(TUYA_UART_NUM_E port_id, VOID_T *buff, UINT16_T len)
{
    if (TUYA_UART_NUM_0 != port_id) {
        return 0;
    }
    if (!tkl_uart_is_init) {
        return 0;
    }
    if (buff == NULL) {
        return 0;
    }

    UINT32_T rx_len = 0;
    if (uart_rx_true) {
        uart_rx_true = 0;
        rx_len = T_rxdata_buf.len;
        if (len >= rx_len) {
            memcpy(buff, T_rxdata_buf.data, rx_len);
            return rx_len;

        } else {
            memcpy(buff, T_rxdata_buf.data, len);
            return len;

        }

    }
    return rx_len;
}

OPERATE_RET tkl_uart_set_tx_int(TUYA_UART_NUM_E port_id, BOOL_T enable)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_set_rx_flowctrl(TUYA_UART_NUM_E port_id, BOOL_T enable)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_wait_for_data(TUYA_UART_NUM_E port_id, INT32_T timeout_ms)
{
    return OPRT_NOT_SUPPORTED;
}

OPERATE_RET tkl_uart_ioctl(TUYA_UART_NUM_E port_id, UINT32_T cmd, VOID *arg)
{
    if (port_id != TUYA_UART_NUM_0) {
        return OPRT_INVALID_PARM;
    }

    if(arg == NULL) {
        return OPRT_INVALID_PARM;
    }


    if(cmd == 0) {
        TKL_UART_GPIO_T *uart_io = (TKL_UART_GPIO_T *)arg;
        //                 A2                                  B1                                  D0                                   D3                                   D7
        if((uart_io->tx != TUYA_GPIO_NUM_2) && (uart_io->tx != TUYA_GPIO_NUM_9) && (uart_io->tx != TUYA_GPIO_NUM_24) && (uart_io->tx != TUYA_GPIO_NUM_27) && (uart_io->tx != TUYA_GPIO_NUM_31)) {
            return OPRT_NOT_FOUND;
        }
        //                 A0                                  B0                                  B7                                   C3                                   C5                                   D6
        if((uart_io->rx != TUYA_GPIO_NUM_0) && (uart_io->rx != TUYA_GPIO_NUM_8) && (uart_io->rx != TUYA_GPIO_NUM_15) && (uart_io->rx != TUYA_GPIO_NUM_19) && (uart_io->rx != TUYA_GPIO_NUM_21) && (uart_io->rx != TUYA_GPIO_NUM_30)) {
            return OPRT_NOT_FOUND;
        }

        tkl_uart_gpio.tx = uart_io->tx;
        tkl_uart_gpio.rx = uart_io->rx;
    } else {
        return OPRT_INVALID_PARM;
    }

    return OPRT_OK;
}

