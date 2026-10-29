#include "main.h"
#include "faqiuji_audio.h"
#include "task.h"

void NMI_Handler(void)
{
}

void HardFault_Handler(void)
{
  for (;;) {
  }
}

void SysTick_Handler(void)
{
  xPortSysTickHandler();
}

void TIM6_LPTIM1_DAC_IRQHandler(void)
{
  faqiuji_audio_tim6_irq_handler();
}
