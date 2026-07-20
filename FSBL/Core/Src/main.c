/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "extmem_manager.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include "stm32_extmem_conf.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

UART_HandleTypeDef hlpuart1;

XSPI_HandleTypeDef hxspi2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void PeriphCommonClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_GPDMA1_Init(void);
static void MX_XSPI2_Init(void);
static void MX_LPUART1_UART_Init(void);
static void MX_BSEC_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* Configure the peripherals common clocks */
  PeriphCommonClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_XSPI2_Init();
  MX_LPUART1_UART_Init();
  MX_BSEC_Init();
  MX_EXTMEM_MANAGER_Init();
  /* USER CODE BEGIN 2 */
  {
    const char *msg;
    char buf[80];
    static const char hex[] = "0123456789ABCDEF";

    /* HSLV OTP fuse (word 124: bit15=VDDIO3/XSPI2-NOR, bit16=VDDIO2/
     * XSPI1-PSRAM) is the hardware permission for 1.8V-range high-speed
     * pads, and SVMCR VRSEL must then select the 1.8V range.  Every ST
     * N6570-DK example programs this fuse on first boot (donor
     * app_fuseprogramming.c); without fuse+VRSEL the XSPI pads run in
     * 3.3V-range mode — fine at <=64 MHz, broken at speed (matches the
     * XSPI1 PSRAM 200 MHz corruption and the XSPI2 octal-DTR bus stall).
     * Idempotent: reads first, programs only missing bits.  Both rails
     * are hard-wired 1.8V on this board (BSP sets 1V8 unconditionally). */
    {
      BSEC_HandleTypeDef hbsec;
      const uint32_t ulHslvMask = (1UL << 15) | (1UL << 16);
      uint32_t ulFuse = 0;

      __HAL_RCC_BSEC_CLK_ENABLE();
      hbsec.Instance = BSEC;

      if (HAL_BSEC_OTP_Read(&hbsec, 124U, &ulFuse) != HAL_OK)
      {
        msg = "[FSBL] HSLV fuse read FAILED\r\n";
        HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
      }
      else if ((ulFuse & ulHslvMask) == ulHslvMask)
      {
        msg = "[FSBL] HSLV fuses already set\r\n";
        HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
      }
      else
      {
        msg = "[FSBL] programming HSLV fuses...\r\n";
        HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);

        if ((HAL_BSEC_OTP_Program(&hbsec, 124U, ulFuse | ulHslvMask,
                                  HAL_BSEC_NORMAL_PROG) == HAL_OK) &&
            (HAL_BSEC_OTP_Read(&hbsec, 124U, &ulFuse) == HAL_OK) &&
            ((ulFuse & ulHslvMask) == ulHslvMask))
        {
          msg = "[FSBL] HSLV fuses programmed OK\r\n";
        }
        else
        {
          /* Not fatal here: the app verifies the octal NOR link and falls
           * back to 1-line if the pads can't do 200 MHz. */
          msg = "[FSBL] HSLV fuse program FAILED\r\n";
        }
        HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
      }

      HAL_PWREx_ConfigVddIORange(PWR_VDDIO2, PWR_VDDIO_RANGE_1V8);
      HAL_PWREx_ConfigVddIORange(PWR_VDDIO3, PWR_VDDIO_RANGE_1V8);
    }

    msg = "\r\n[FSBL] booting app...\r\n";
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);

    extern BOOTStatus_TypeDef MapMemory(void);
    extern BOOTStatus_TypeDef CopyApplication(void);
    extern BOOTStatus_TypeDef JumpToApplication(void);

    BOOTStatus_TypeDef ret;

    ret = MapMemory();
    if (ret != BOOT_OK) {
      msg = "[FSBL] MapMemory FAIL\r\n";
      HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
      Error_Handler();
    }
    msg = "[FSBL] MapMemory OK\r\n";
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);

    ret = CopyApplication();
    if (ret != BOOT_OK) {
      msg = "[FSBL] CopyApp FAIL\r\n";
      HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
      Error_Handler();
    }
    msg = "[FSBL] CopyApp OK\r\n";
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);

    /* Dump vector table at destination */
    uint32_t vt_addr = EXTMEM_LRUN_DESTINATION_ADDRESS + EXTMEM_HEADER_OFFSET;
    uint32_t app_sp = *(volatile uint32_t *)vt_addr;
    uint32_t app_pc = *(volatile uint32_t *)(vt_addr + 4);

    /* Print SP */
    int i = 0;
    const char *lbl = "[FSBL] VT SP=0x";
    while (*lbl) buf[i++] = *lbl++;
    for (int b = 28; b >= 0; b -= 4) buf[i++] = hex[(app_sp >> b) & 0xF];
    buf[i++] = '\r'; buf[i++] = '\n';
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)buf, i, 500);

    /* Print PC */
    i = 0;
    lbl = "[FSBL] VT PC=0x";
    while (*lbl) buf[i++] = *lbl++;
    for (int b = 28; b >= 0; b -= 4) buf[i++] = hex[(app_pc >> b) & 0xF];
    buf[i++] = '\r'; buf[i++] = '\n';
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)buf, i, 500);

    /* Enable AXISRAM3-6 clocks before jumping — the app's SP (0x34300400)
       is in SRAM5.  SystemInit enables these too, but its own function
       prologue pushes to the stack before that code runs. */
    RCC->MEMENSR = RCC_MEMENSR_AXISRAM3ENS | RCC_MEMENSR_AXISRAM4ENS
                 | RCC_MEMENSR_AXISRAM5ENS | RCC_MEMENSR_AXISRAM6ENS;

    msg = "[FSBL] jumping...\r\n";
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
    HAL_Delay(20);

    ret = JumpToApplication();
    /* Should never reach here */
    msg = "[FSBL] jump returned!\r\n";
    HAL_UART_Transmit(&hlpuart1, (const uint8_t *)msg, strlen(msg), 500);
    Error_Handler();
  }
  /* USER CODE END 2 */
  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}
/* USER CODE BEGIN CLK 1 */
/* USER CODE END CLK 1 */

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* Set external SMPS regulator to overdrive (GPIOF4 high) — required BEFORE
   * PWR_REGULATOR_VOLTAGE_SCALE0 so the core rail is actually boosted.
   * Matches BSP_SMPS_Init(SMPS_VOLTAGE_OVERDRIVE) from
   * x-cube-n6-ai-h264-usb-uvc/Src/main.c:179.  Without this the VENC block
   * reports FUSE_ERROR (-17) / HW_RESET (-16) on the first encode attempts
   * because the core supply is still at nominal while PLL1 runs at 1600 MHz. */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  {
    GPIO_InitTypeDef smps_gpio = {0};
    smps_gpio.Pin   = GPIO_PIN_4;
    smps_gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    smps_gpio.Pull  = GPIO_NOPULL;
    smps_gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOF, &smps_gpio);
    HAL_GPIO_WritePin(GPIOF, GPIO_PIN_4, GPIO_PIN_SET);
  }
  HAL_Delay(1);  /* voltage ramp */

  /** Configure the System Power Supply
  */
  if (HAL_PWREx_ConfigSupply(PWR_EXTERNAL_SOURCE_SUPPLY) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE0) != HAL_OK)
  {
    Error_Handler();
  }

  /* Enable HSI */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Get current CPU/System buses clocks configuration and if necessary switch
 to intermediate HSI clock to ensure target clock can be set
  */
  HAL_RCC_GetClockConfig(&RCC_ClkInitStruct);
  if ((RCC_ClkInitStruct.CPUCLKSource == RCC_CPUCLKSOURCE_IC1) ||
     (RCC_ClkInitStruct.SYSCLKSource == RCC_SYSCLKSOURCE_IC2_IC6_IC11))
  {
    RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_CPUCLK | RCC_CLOCKTYPE_SYSCLK);
    RCC_ClkInitStruct.CPUCLKSource = RCC_CPUCLKSOURCE_HSI;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK)
    {
      /* Initialization Error */
      Error_Handler();
    }
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL1.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL1.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL1.PLLM = 1;
  RCC_OscInitStruct.PLL1.PLLN = 25;
  RCC_OscInitStruct.PLL1.PLLFractional = 0;
  RCC_OscInitStruct.PLL1.PLLP1 = 1;
  RCC_OscInitStruct.PLL1.PLLP2 = 1;
  RCC_OscInitStruct.PLL2.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL3.PLLState = RCC_PLL_NONE;
  RCC_OscInitStruct.PLL4.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL4.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL4.PLLM = 1;
  RCC_OscInitStruct.PLL4.PLLN = 25;
  RCC_OscInitStruct.PLL4.PLLFractional = 0;
  RCC_OscInitStruct.PLL4.PLLP1 = 1;
  RCC_OscInitStruct.PLL4.PLLP2 = 1;

  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_CPUCLK|RCC_CLOCKTYPE_HCLK
                              |RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1
                              |RCC_CLOCKTYPE_PCLK2|RCC_CLOCKTYPE_PCLK5
                              |RCC_CLOCKTYPE_PCLK4;
  RCC_ClkInitStruct.CPUCLKSource = RCC_CPUCLKSOURCE_IC1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_IC2_IC6_IC11;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV1;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV1;
  RCC_ClkInitStruct.APB5CLKDivider = RCC_APB5_DIV1;
  RCC_ClkInitStruct.IC1Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC1Selection.ClockDivider = 2;
  RCC_ClkInitStruct.IC2Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC2Selection.ClockDivider = 4;
  /* NPU (IC6) 1600/2 = 800 MHz, NPU RAMs (IC11) 1600/4 = 400 MHz — was
   * 400/200.  The donor (x-cube-n6-ai-h264-usb-uvc) runs 1000/900 off
   * dedicated PLL2/PLL3; 800/400 off the shared PLL1 halves inference
   * compute time without adding a PLL.  VOS scale 0 + SMPS overdrive are
   * already set above, which is what these speeds require. */
  RCC_ClkInitStruct.IC6Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC6Selection.ClockDivider = 2;
  RCC_ClkInitStruct.IC11Selection.ClockSelection = RCC_ICCLKSOURCE_PLL1;
  RCC_ClkInitStruct.IC11Selection.ClockDivider = 4;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Peripherals Common Clock Configuration
  * @retval None
  */
void PeriphCommonClock_Config(void)
{
  RCC_PeriphCLKInitTypeDef PeriphClkInitStruct = {0};

  /** Initializes the peripherals clock
  */
  PeriphClkInitStruct.PeriphClockSelection = RCC_PERIPHCLK_TIM;
  PeriphClkInitStruct.TIMPresSelection = RCC_TIMPRES_DIV2;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief BSEC Initialization Function
  * @param None
  * @retval None
  */
static void MX_BSEC_Init(void)
{

  /* USER CODE BEGIN BSEC_Init 0 */

  /* USER CODE END BSEC_Init 0 */

  /* USER CODE BEGIN BSEC_Init 1 */

  /* USER CODE END BSEC_Init 1 */
  /* USER CODE BEGIN BSEC_Init 2 */

  /* USER CODE END BSEC_Init 2 */

}

/**
  * @brief GPDMA1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPDMA1_Init(void)
{

  /* USER CODE BEGIN GPDMA1_Init 0 */

  /* USER CODE END GPDMA1_Init 0 */

  /* Peripheral clock enable */
  __HAL_RCC_GPDMA1_CLK_ENABLE();

  /* USER CODE BEGIN GPDMA1_Init 1 */

  /* USER CODE END GPDMA1_Init 1 */
  /* USER CODE BEGIN GPDMA1_Init 2 */

  /* USER CODE END GPDMA1_Init 2 */

}

/**
  * @brief LPUART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPUART1_UART_Init(void)
{

  /* USER CODE BEGIN LPUART1_Init 0 */

  /* USER CODE END LPUART1_Init 0 */

  /* USER CODE BEGIN LPUART1_Init 1 */

  /* USER CODE END LPUART1_Init 1 */
  hlpuart1.Instance = LPUART1;
  hlpuart1.Init.BaudRate = 115200;
  hlpuart1.Init.WordLength = UART_WORDLENGTH_8B;
  hlpuart1.Init.StopBits = UART_STOPBITS_1;
  hlpuart1.Init.Parity = UART_PARITY_NONE;
  hlpuart1.Init.Mode = UART_MODE_TX_RX;
  hlpuart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  hlpuart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  hlpuart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  hlpuart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  hlpuart1.FifoMode = UART_FIFOMODE_ENABLE;
  if (HAL_UART_Init(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&hlpuart1, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&hlpuart1, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_EnableFifoMode(&hlpuart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPUART1_Init 2 */

  /* USER CODE END LPUART1_Init 2 */

}

/**
  * @brief XSPI2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_XSPI2_Init(void)
{

  /* USER CODE BEGIN XSPI2_Init 0 */

  /* USER CODE END XSPI2_Init 0 */

  XSPIM_CfgTypeDef sXspiManagerCfg = {0};

  /* USER CODE BEGIN XSPI2_Init 1 */

  /* USER CODE END XSPI2_Init 1 */
  /* XSPI2 parameter configuration*/
  hxspi2.Instance = XSPI2;
  hxspi2.Init.FifoThresholdByte = 4;
  hxspi2.Init.MemoryMode = HAL_XSPI_SINGLE_MEM;
  hxspi2.Init.MemoryType = HAL_XSPI_MEMTYPE_MACRONIX;
  hxspi2.Init.MemorySize = HAL_XSPI_SIZE_1GB;
  hxspi2.Init.ChipSelectHighTimeCycle = 1;
  hxspi2.Init.FreeRunningClock = HAL_XSPI_FREERUNCLK_DISABLE;
  hxspi2.Init.ClockMode = HAL_XSPI_CLOCK_MODE_0;
  hxspi2.Init.WrapSize = HAL_XSPI_WRAP_NOT_SUPPORTED;
  hxspi2.Init.ClockPrescaler = 0;
  hxspi2.Init.SampleShifting = HAL_XSPI_SAMPLE_SHIFT_NONE;
  hxspi2.Init.DelayHoldQuarterCycle = HAL_XSPI_DHQC_ENABLE;
  hxspi2.Init.ChipSelectBoundary = HAL_XSPI_BONDARYOF_NONE;
  hxspi2.Init.MaxTran = 0;
  hxspi2.Init.Refresh = 0;
  hxspi2.Init.MemorySelect = HAL_XSPI_CSSEL_NCS1;
  if (HAL_XSPI_Init(&hxspi2) != HAL_OK)
  {
    Error_Handler();
  }
  sXspiManagerCfg.nCSOverride = HAL_XSPI_CSSEL_OVR_NCS1;
  sXspiManagerCfg.IOPort = HAL_XSPIM_IOPORT_2;
  sXspiManagerCfg.Req2AckTime = 1;
  if (HAL_XSPIM_Config(&hxspi2, &sXspiManagerCfg, HAL_XSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN XSPI2_Init 2 */

  /* USER CODE END XSPI2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPION_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
