#include "main.h"
#include "faqiuji_protocol.h"
#include "gpio_config.h"

UART_HandleTypeDef UartHandle;
static uint8_t uart_rx_byte;

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

  HAL_GPIO_WritePin(GPIOA, MOTOR_PWM | LED_LOW | LED_MIDDLE |
                    LED_HIGH | IE_PWM, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, CHARGE_EN | LED_G | LED_R,
                    GPIO_PIN_RESET);
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
  HAL_GPIO_WritePin(GPIOB, NTC_CON, GPIO_PIN_RESET);
}

static void APP_UartInit(void)
{
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

int main(void)
{
  HAL_Init();
  APP_GpioInit();
  APP_UartInit();

  while (1) {
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart == &UartHandle) {
    faqiuji_protocol_input(uart_rx_byte);
    if (HAL_UART_Receive_IT(&UartHandle, &uart_rx_byte, 1U) != HAL_OK) {
      APP_ErrorHandler();
    }
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart == &UartHandle) {
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
