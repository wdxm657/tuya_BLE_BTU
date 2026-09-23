#include "main.h"
#include "faqiuji_protocol.h"
#include "faqiuji_launcher.h"
#include "faqiuji_device.h"
#include "gpio_config.h"
#include "FreeRTOS.h"
#include "task.h"

UART_HandleTypeDef UartHandle;
static uint8_t uart_rx_byte;
#define UART_RX_QUEUE_SIZE 128U
static volatile uint8_t uart_rx_queue[UART_RX_QUEUE_SIZE];
static volatile uint8_t uart_rx_write;
static volatile uint8_t uart_rx_read;

static void APP_UartService(void)
{
  uint8_t value;

  while (uart_rx_read != uart_rx_write) {
    value = uart_rx_queue[uart_rx_read];
    uart_rx_read = (uint8_t)((uart_rx_read + 1U) % UART_RX_QUEUE_SIZE);
    faqiuji_protocol_input(value);
  }
}

static void APP_KeyTask(void *argument)
{
  static uint8_t raw_state;
  static uint8_t stable_state;
  static uint8_t stable_count;
  static uint8_t initialized;
  uint8_t pressed;
  (void)argument;

  for (;;) {
    pressed = (HAL_GPIO_ReadPin(GPIOC, KEY) == GPIO_PIN_RESET) ? 1U : 0U;

    if (!initialized) {
      raw_state = pressed;
      stable_state = pressed;
      initialized = 1U;
      stable_count = 0U;
    } else if (pressed != raw_state) {
      raw_state = pressed;
      stable_count = 1U;
    } else if (stable_count > 0U && stable_count < 3U) {
      stable_count++;
    }

    if (stable_count >= 3U && stable_state != raw_state) {
      stable_state = raw_state;
      stable_count = 0U;
      faqiuji_protocol_key_event(stable_state);
    }

    vTaskDelay(pdMS_TO_TICKS(10U));
  }
}

static void APP_UartTask(void *argument)
{
  (void)argument;
  for (;;) {
    APP_UartService();
    vTaskDelay(pdMS_TO_TICKS(1U));
  }
}

static void APP_GpioInit(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();

  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;

  gpio.Pin = MOTOR_PWM | LED_LOW | LED_MIDDLE | LED_HIGH | IE_PWM;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = CHARGE_EN | LED_G | LED_R;
  HAL_GPIO_Init(GPIOB, &gpio);
  gpio.Pin = DCT_CON | IR_CON;
  HAL_GPIO_Init(GPIOC, &gpio);

  HAL_GPIO_WritePin(GPIOA,  LED_LOW  | LED_MIDDLE | LED_HIGH, GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOB,  LED_G , GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB,  LED_R , GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOA, MOTOR_PWM | IE_PWM , GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, CHARGE_EN ,GPIO_PIN_SET);
  HAL_GPIO_WritePin(GPIOC, DCT_CON | IR_CON, GPIO_PIN_RESET);

  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_PULLUP;
  gpio.Pin = KEY;
  HAL_GPIO_Init(GPIOC, &gpio);
  gpio.Pull = GPIO_NOPULL;
  gpio.Pin = CHARGE_STATE;
  HAL_GPIO_Init(GPIOB, &gpio);
  gpio.Pin = USB_DET;
  HAL_GPIO_Init(GPIOF, &gpio);
  gpio.Pull = GPIO_PULLUP;
  gpio.Pin = LEIDA_IN_O | IR;
  HAL_GPIO_Init(GPIOA, &gpio);

  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  gpio.Pin = AD_I_SHUNT | AD_BAT;
  HAL_GPIO_Init(GPIOA, &gpio);
  gpio.Pin = AD_NTC;
  HAL_GPIO_Init(GPIOB, &gpio);
  gpio.Pin = NTC_CON;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  HAL_GPIO_Init(GPIOB, &gpio);
  HAL_GPIO_WritePin(GPIOB, NTC_CON, GPIO_PIN_SET);
}

static void APP_UartInit(void)
{
  uart_rx_write = 0U;
  uart_rx_read = 0U;
  UartHandle.Instance = USART1;
  UartHandle.Init.BaudRate = 9600;
  UartHandle.Init.WordLength = UART_WORDLENGTH_8B;
  UartHandle.Init.StopBits = UART_STOPBITS_1;
  UartHandle.Init.Parity = UART_PARITY_NONE;
  UartHandle.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  UartHandle.Init.Mode = UART_MODE_TX_RX;
  UartHandle.Init.OverSampling = UART_OVERSAMPLING_16;
  UartHandle.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;

  if (HAL_UART_Init(&UartHandle) != HAL_OK) {
    APP_ErrorHandler();
  }

  faqiuji_protocol_init(&UartHandle);
  if (HAL_UART_Receive_IT(&UartHandle, &uart_rx_byte, 1U) != HAL_OK) {
    APP_ErrorHandler();
  }
}
static void APP_SystemClockConfig(void);

int main(void)
{
  HAL_Init();
  APP_SystemClockConfig();
  APP_GpioInit();
  APP_UartInit();
  if (faqiuji_launcher_init() != HAL_OK) {
    APP_ErrorHandler();
  }
  faqiuji_device_init();

  (void)xTaskCreate(APP_UartTask, "uart", 256, NULL, 3, NULL);
  (void)xTaskCreate(APP_KeyTask, "key", 192, NULL, 2, NULL);
  (void)xTaskCreate(faqiuji_launcher_task, "launch", 256, NULL, 2, NULL);
  (void)xTaskCreate(faqiuji_device_task, "device", 256, NULL, 2, NULL);
  vTaskStartScheduler();
  APP_ErrorHandler();
}


/**
  * @brief   System clock configuration function
  * @param   None
  * @retval  None
  */
static void APP_SystemClockConfig(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* Configure clock source: HSE/HSI/LSE/LSI */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;                                                       /* Enable HSI */
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_8MHz;                                /* Configure HSI output clock as 8MHz */
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;                                                       /* HSI not divided */
  RCC_OscInitStruct.HSEState = RCC_HSE_OFF;                                                      /* Disable HSE */
  /*RCC_OscInitStruct.HSEFreq = RCC_HSE_16_32MHz;*/
  RCC_OscInitStruct.LSIState = RCC_LSI_OFF;                                                      /* Disable LSI */
  RCC_OscInitStruct.LSEState = RCC_LSE_OFF;                                                      /* Disable LSE */
  /*RCC_OscInitStruct.LSEDriver = RCC_LSEDRIVE_MEDIUM;*/
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_OFF;                                                  /* Disable PLL */
  /*RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_NONE;*/
  /*RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL2;*/

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)                                           /* Initialize RCC oscillators */
  {
    APP_ErrorHandler();
  }

  /* Initialize CPU, AHB, and APB bus clocks */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1; /* RCC system clock types */
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSISYS;                                      /* SYSCLK source is HSI */
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;                                             /* AHB clock not divided */
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;                                              /* APB clock not divided */

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)                        /* Initialize RCC system clock */
  {
    APP_ErrorHandler();
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart == &UartHandle) {
    uint8_t next = (uint8_t)((uart_rx_write + 1U) % UART_RX_QUEUE_SIZE);
    if (next != uart_rx_read) {
      uart_rx_queue[uart_rx_write] = uart_rx_byte;
      uart_rx_write = next;
    }
    if (HAL_UART_Receive_IT(&UartHandle, &uart_rx_byte, 1U) != HAL_OK) {
      APP_ErrorHandler();
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == &UartHandle) {
    /* Keep RX armed after an overrun or framing error. */
    (void)HAL_UART_Receive_IT(&UartHandle, &uart_rx_byte, 1U);
  }
}

void APP_ErrorHandler(void)
{
  __disable_irq();
  while (1) {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  (void)file;
  (void)line;
  APP_ErrorHandler();
}
#endif
