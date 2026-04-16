/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32n6xx_it.c
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32n6xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */
#if (USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION == 1)
#include "FreeRTOS.h"
#include "task.h"
#endif
/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#if (USE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION == 1)
// Use SysTick as OS Tick and HAL Tick. No need to have 2 x 1ms interrupts.
#if defined(SysTick_Handler)
#undef SysTick_Handler
#endif

extern void xPortSysTickHandler(void);
#endif
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern UART_HandleTypeDef hlpuart1;
extern HASH_HandleTypeDef hhash;
#if defined(HAL_IWDG_MODULE_ENABLED)
extern IWDG_HandleTypeDef hiwdg;
#endif
extern UART_HandleTypeDef huart2;
extern PKA_HandleTypeDef hpka;
extern RNG_HandleTypeDef hrng;
extern CRYP_HandleTypeDef hcryp;
extern DMA_HandleTypeDef handle_GPDMA1_Channel1;
extern DMA_HandleTypeDef handle_GPDMA1_Channel0;
extern SPI_HandleTypeDef hspi5;
/* USER CODE BEGIN EV */
extern DCMIPP_HandleTypeDef hcamera_dcmipp;   /* defined in cmw_camera.c */
/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
   while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
/* Bounded UART putc used by the fault-handler diagnostics below.
 * Duplicated (rather than routing to it_raw_putc which is static in a
 * different section) to keep the fault path independent: a dropped
 * char on a wedged UART is fine — we're about to be IWDG-reset. */
static inline void fault_raw_putc(char c)
{
  for (uint32_t i = 0; i < 600000UL; i++) {
    if (*(volatile uint32_t *)0x56000C1CUL & (1UL << 7)) {
      *(volatile uint32_t *)0x56000C28UL = (uint32_t)c;
      return;
    }
  }
}

static void raw_hex32(uint32_t val)
{
  static const char hex[] = "0123456789ABCDEF";
  for (int i = 7; i >= 0; i--) {
    fault_raw_putc(hex[(val >> (i * 4)) & 0xF]);
  }
}

static void raw_str(const char *s)
{
  while (*s) {
    fault_raw_putc(*s++);
  }
}

/* Set to 1 to suppress HardFault logging (used during VENC init) */
volatile uint8_t g_hardfault_log_suppress = 0;

void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */
  uint32_t cfsr = *(volatile uint32_t *)0xE000ED28UL;
  uint32_t hfsr = *(volatile uint32_t *)0xE000ED2CUL;
  uint32_t bfar = *(volatile uint32_t *)0xE000ED38UL;  /* Bus Fault Address Register */
  uint32_t mmfar = *(volatile uint32_t *)0xE000ED34UL; /* MemManage Fault Address Register */

  /* Grab stacked PC from the correct exception frame.
   * EXC_RETURN bit 2: 0 = MSP was used (handler mode), 1 = PSP (thread mode).
   * We read LR via inline asm since it holds EXC_RETURN in a fault handler. */
  uint32_t exc_return;
  __asm volatile ("mov %0, lr" : "=r" (exc_return));
  uint32_t *frame = (exc_return & (1UL << 2))
                    ? (uint32_t *)__get_PSP()
                    : (uint32_t *)__get_MSP();
  uint32_t fault_pc = frame ? frame[6] : 0;
  uint32_t fault_lr = frame ? frame[5] : 0;

  /* Clear sticky fault status bits (write-1-to-clear) */
  *(volatile uint32_t *)0xE000ED28UL = cfsr;
  *(volatile uint32_t *)0xE000ED2CUL = hfsr;

  if (!g_hardfault_log_suppress)
  {
    raw_str("H CFSR=");
    raw_hex32(cfsr);
    raw_str(" HFSR=");
    raw_hex32(hfsr);
    /* Log BFAR when BusFault Address Valid (BFARVALID = CFSR bit 15) */
    if (cfsr & (1UL << 15))
    {
      raw_str(" BFAR=");
      raw_hex32(bfar);
    }
    /* Log MMFAR when MemManage Address Valid (MMARVALID = CFSR bit 7) */
    if (cfsr & (1UL << 7))
    {
      raw_str(" MMFAR=");
      raw_hex32(mmfar);
    }
    raw_str(" PC=");
    raw_hex32(fault_pc);
    raw_str(" LR=");
    raw_hex32(fault_lr);
    raw_str("\r\n");
  }
  /* Halt here — returning from HardFault re-executes the faulting
   * instruction, and if PSP is corrupted the handler itself faults,
   * causing a double-fault → Cortex-M Lockup with no UART output.   */
  while (1) {}
  /* USER CODE END HardFault_IRQn 0 */
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  fault_raw_putc('m');
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Prefetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  fault_raw_putc('B');
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  fault_raw_putc('U');
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Secure fault.
  */
void SecureFault_Handler(void)
{
  /* USER CODE BEGIN SecureFault_IRQn 0 */
  fault_raw_putc('F');
  /* USER CODE END SecureFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_SecureFault_IRQn 0 */
    /* USER CODE END W1_SecureFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32N6xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32n6xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles IWDG global interrupt.
  */
void IWDG_IRQHandler(void)
{
  /* USER CODE BEGIN IWDG_IRQn 0 */
#if defined(HAL_IWDG_MODULE_ENABLED)
  /* USER CODE END IWDG_IRQn 0 */
  HAL_IWDG_IRQHandler(&hiwdg);
  /* USER CODE BEGIN IWDG_IRQn 1 */
#endif
  /* USER CODE END IWDG_IRQn 1 */
}

/**
  * @brief This function handles EXTI Line9 interrupt.
  */
void EXTI9_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI9_IRQn 0 */

  /* USER CODE END EXTI9_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(ARD_D03_Pin);
  /* USER CODE BEGIN EXTI9_IRQn 1 */

  /* USER CODE END EXTI9_IRQn 1 */
}

/**
  * @brief This function handles EXTI Line13 interrupt.
  */
void EXTI13_IRQHandler(void)
{
  /* USER CODE BEGIN EXTI13_IRQn 0 */

  /* USER CODE END EXTI13_IRQn 0 */
  HAL_GPIO_EXTI_IRQHandler(USER_BUTTON_Pin);
  /* USER CODE BEGIN EXTI13_IRQn 1 */

  /* USER CODE END EXTI13_IRQn 1 */
}

/**
  * @brief This function handles Secure AES global interrupt.
  */
void SAES_IRQHandler(void)
{
  /* USER CODE BEGIN SAES_IRQn 0 */

  /* USER CODE END SAES_IRQn 0 */
  HAL_CRYP_IRQHandler(&hcryp);
  /* USER CODE BEGIN SAES_IRQn 1 */

  /* USER CODE END SAES_IRQn 1 */
}

/**
  * @brief This function handles CRYP global interrupt.
  */
void CRYP_IRQHandler(void)
{
  /* USER CODE BEGIN CRYP_IRQn 0 */

  /* USER CODE END CRYP_IRQn 0 */
  HAL_CRYP_IRQHandler(&hcryp);
  /* USER CODE BEGIN CRYP_IRQn 1 */

  /* USER CODE END CRYP_IRQn 1 */
}

/**
  * @brief This function handles PKA global interrupt.
  */
void PKA_IRQHandler(void)
{
  /* USER CODE BEGIN PKA_IRQn 0 */

  /* USER CODE END PKA_IRQn 0 */
  HAL_PKA_IRQHandler(&hpka);
  /* USER CODE BEGIN PKA_IRQn 1 */

  /* USER CODE END PKA_IRQn 1 */
}

/**
  * @brief This function handles HASH global interrupt.
  */
void HASH_IRQHandler(void)
{
  /* USER CODE BEGIN HASH_IRQn 0 */

  /* USER CODE END HASH_IRQn 0 */
  HAL_HASH_IRQHandler(&hhash);
  /* USER CODE BEGIN HASH_IRQn 1 */

  /* USER CODE END HASH_IRQn 1 */
}

/**
  * @brief This function handles RNG global interrupt.
  */
void RNG_IRQHandler(void)
{
  /* USER CODE BEGIN RNG_IRQn 0 */

  /* USER CODE END RNG_IRQn 0 */
  HAL_RNG_IRQHandler(&hrng);
  /* USER CODE BEGIN RNG_IRQn 1 */

  /* USER CODE END RNG_IRQn 1 */
}

/**
  * @brief This function handles GPDMA1 Channel 0 global interrupt.
  */
void GPDMA1_Channel0_IRQHandler(void)
{
  /* USER CODE BEGIN GPDMA1_Channel0_IRQn 0 */

  /* USER CODE END GPDMA1_Channel0_IRQn 0 */
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel0);
  /* USER CODE BEGIN GPDMA1_Channel0_IRQn 1 */

  /* USER CODE END GPDMA1_Channel0_IRQn 1 */
}

/**
  * @brief This function handles GPDMA1 Channel 1 global interrupt.
  */
void GPDMA1_Channel1_IRQHandler(void)
{
  /* USER CODE BEGIN GPDMA1_Channel1_IRQn 0 */

  /* USER CODE END GPDMA1_Channel1_IRQn 0 */
  HAL_DMA_IRQHandler(&handle_GPDMA1_Channel1);
  /* USER CODE BEGIN GPDMA1_Channel1_IRQn 1 */

  /* USER CODE END GPDMA1_Channel1_IRQn 1 */
}

/**
  * @brief This function handles SPI5 global interrupt.
  */
void SPI5_IRQHandler(void)
{
  /* USER CODE BEGIN SPI5_IRQn 0 */

  /* USER CODE END SPI5_IRQn 0 */
  HAL_SPI_IRQHandler(&hspi5);
  /* USER CODE BEGIN SPI5_IRQn 1 */

  /* USER CODE END SPI5_IRQn 1 */
}

/**
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
  /* USER CODE BEGIN USART2_IRQn 0 */

  /* USER CODE END USART2_IRQn 0 */
  HAL_UART_IRQHandler(&huart2);
  /* USER CODE BEGIN USART2_IRQn 1 */

  /* USER CODE END USART2_IRQn 1 */
}

/**
  * @brief This function handles LPUART1 global interrupt.
  */
void LPUART1_IRQHandler(void)
{
  /* USER CODE BEGIN LPUART1_IRQn 0 */

  /* USER CODE END LPUART1_IRQn 0 */
  HAL_UART_IRQHandler(&hlpuart1);
  /* USER CODE BEGIN LPUART1_IRQn 1 */

  /* USER CODE END LPUART1_IRQn 1 */
}

/* USER CODE BEGIN 1 */
void SysTick_Handler (void)
{
#if (configUSE_TICKLESS_IDLE != 0 )
  /* Clear overflow flag */
  SysTick->CTRL;
#endif

  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    /* Call tick handler */
    xPortSysTickHandler();
  }

  HAL_IncTick();
}

/**
  * @brief Gate flag — set by MediaCam once DCMIPP pipes are configured.
  *        Until then the ISRs disable themselves to avoid hitting
  *        uninitialised pipe state inside the HAL handler.
  */
volatile uint8_t g_dcmipp_irq_ready = 0;
volatile uint32_t g_dcmipp_irq_count = 0;   /* diagnostic: total DCMIPP IRQ entries */

/* Bounded spin — see rationale in kvs_webrtc_task.c. Called from ISRs,
 * so must never block for long; 1 ms budget is the safety ceiling.        */
extern void vPetWatchdog(void);
static inline void it_raw_putc(char c)
{
  for (uint32_t i = 0; i < 600000UL; i++)
  {
    if (*(volatile uint32_t *)0x56000C1CUL & (1UL << 7))
    {
      *(volatile uint32_t *)0x56000C28UL = (uint32_t)c;
      return;
    }
  }
  vPetWatchdog();
}

/**
  * @brief This function handles DCMIPP global interrupt.
  */
void DCMIPP_IRQHandler(void)
{
  g_dcmipp_irq_count++;
  if (g_dcmipp_irq_ready) {
    /* ISR 'd' putc removed — it was consuming ~87us per interrupt in ISR
     * context and causing UART contention that made the media task fall
     * behind the camera frame rate (115200 baud can't sustain the debug
     * output volume during streaming).                                       */
    HAL_DCMIPP_IRQHandler(&hcamera_dcmipp);
  } else {
    it_raw_putc('D');
    HAL_NVIC_DisableIRQ(DCMIPP_IRQn);
  }
}

/**
  * @brief This function handles CSI global interrupt.
  */
void CSI_IRQHandler(void)
{
  if (g_dcmipp_irq_ready) {
    HAL_DCMIPP_CSI_IRQHandler(&hcamera_dcmipp);
  } else {
    it_raw_putc('C');
    HAL_NVIC_DisableIRQ(CSI_IRQn);
  }
}
/* USER CODE END 1 */
