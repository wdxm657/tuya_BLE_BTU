#include "main.h"
#include "faqiuji_audio.h"
#include "faqiuji_record.h"
#include "task.h"

extern void xPortSysTickHandler(void);

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

void DMA1_Channel1_IRQHandler(void)
{
  faqiuji_record_dma_irq_handler();
}
