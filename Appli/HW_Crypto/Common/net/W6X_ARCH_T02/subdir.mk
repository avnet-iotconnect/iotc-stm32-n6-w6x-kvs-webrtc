################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Common/net/W6X_ARCH_T02/dhcp_server_raw.c \
../Common/net/W6X_ARCH_T02/eth_netif.c \
../Common/net/W6X_ARCH_T02/rtl8211.c \
../Common/net/W6X_ARCH_T02/lwip.c \
../Common/net/W6X_ARCH_T02/lwip_netif.c \
../Common/net/W6X_ARCH_T02/net_diag.c \
../Common/net/W6X_ARCH_T02/st67w6x_netconn.c 

OBJS += \
./Common/net/W6X_ARCH_T02/dhcp_server_raw.o \
./Common/net/W6X_ARCH_T02/eth_netif.o \
./Common/net/W6X_ARCH_T02/rtl8211.o \
./Common/net/W6X_ARCH_T02/lwip.o \
./Common/net/W6X_ARCH_T02/lwip_netif.o \
./Common/net/W6X_ARCH_T02/net_diag.o \
./Common/net/W6X_ARCH_T02/st67w6x_netconn.o 

C_DEPS += \
./Common/net/W6X_ARCH_T02/dhcp_server_raw.d \
./Common/net/W6X_ARCH_T02/eth_netif.d \
./Common/net/W6X_ARCH_T02/rtl8211.d \
./Common/net/W6X_ARCH_T02/lwip.d \
./Common/net/W6X_ARCH_T02/lwip_netif.d \
./Common/net/W6X_ARCH_T02/net_diag.d \
./Common/net/W6X_ARCH_T02/st67w6x_netconn.d 


# Each subdirectory must supply rules for building sources it contributes
Common/net/W6X_ARCH_T02/%.o Common/net/W6X_ARCH_T02/%.su Common/net/W6X_ARCH_T02/%.cyclo: ../Common/net/W6X_ARCH_T02/%.c Common/net/W6X_ARCH_T02/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m55 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx -DST67_ARCH=W6X_ARCH_T02 -DUSE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=1 '-DMBEDTLS_CONFIG_FILE="mbedtls_config_hw.h"' '-DLFS_CONFIG=lfs_config.h' -DST67W6X_RCP -DHW_CRYPTO -DHAVE_ARPA_INET_H=1 -DENABLE_SCTP_DATA_CHANNEL=0 -DHAVE_CONFIG_H=1 -DMBEDTLS=1 -DSDP_DO_NOT_USE_CUSTOM_CONFIG -DMETRIC_PRINT_ENABLED=1 -DMBEDTLS_DTLS_DEBUG_C -DportTICK_RATE_MS=portTICK_PERIOD_MS -DLIBRARY_LOG_LEVEL=3 -DSPI_THREAD_STACK_SIZE=2048 -DENABLE_AI_DETECTION -DLL_ATON_PLATFORM=LL_ATON_PLAT_STM32N6 -DLL_ATON_OSAL=LL_ATON_OSAL_FREERTOS -DLL_ATON_RT_MODE=LL_ATON_RT_ASYNC -DLL_ATON_SW_FALLBACK -DLL_ATON_DBG_BUFFER_INFO_EXCLUDED=1 -c -O2 -ffunction-sections -fdata-sections -Wall -Wno-error=incompatible-pointer-types -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@"  -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@" @"Common/net/W6X_ARCH_T02/dhcp_server_raw.c_includes.args"

clean: clean-Common-2f-net-2f-W6X_ARCH_T02

clean-Common-2f-net-2f-W6X_ARCH_T02:
	-$(RM) ./Common/net/W6X_ARCH_T02/dhcp_server_raw.cyclo ./Common/net/W6X_ARCH_T02/dhcp_server_raw.d ./Common/net/W6X_ARCH_T02/dhcp_server_raw.o ./Common/net/W6X_ARCH_T02/dhcp_server_raw.su ./Common/net/W6X_ARCH_T02/lwip.cyclo ./Common/net/W6X_ARCH_T02/lwip.d ./Common/net/W6X_ARCH_T02/lwip.o ./Common/net/W6X_ARCH_T02/lwip.su ./Common/net/W6X_ARCH_T02/lwip_netif.cyclo ./Common/net/W6X_ARCH_T02/lwip_netif.d ./Common/net/W6X_ARCH_T02/lwip_netif.o ./Common/net/W6X_ARCH_T02/lwip_netif.su ./Common/net/W6X_ARCH_T02/net_diag.cyclo ./Common/net/W6X_ARCH_T02/net_diag.d ./Common/net/W6X_ARCH_T02/net_diag.o ./Common/net/W6X_ARCH_T02/net_diag.su ./Common/net/W6X_ARCH_T02/st67w6x_netconn.cyclo ./Common/net/W6X_ARCH_T02/st67w6x_netconn.d ./Common/net/W6X_ARCH_T02/st67w6x_netconn.o ./Common/net/W6X_ARCH_T02/st67w6x_netconn.su

.PHONY: clean-Common-2f-net-2f-W6X_ARCH_T02

