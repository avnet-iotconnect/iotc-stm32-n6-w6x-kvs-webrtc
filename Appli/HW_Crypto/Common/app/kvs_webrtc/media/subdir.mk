################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Common/app/kvs_webrtc/media/lcd_preview.c \
../Common/app/kvs_webrtc/media/media_cam.c \
../Common/app/kvs_webrtc/media/media_enc.c \
../Common/app/kvs_webrtc/media/stm32_media_port.c \
../Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.c 

OBJS += \
./Common/app/kvs_webrtc/media/lcd_preview.o \
./Common/app/kvs_webrtc/media/media_cam.o \
./Common/app/kvs_webrtc/media/media_enc.o \
./Common/app/kvs_webrtc/media/stm32_media_port.o \
./Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.o 

C_DEPS += \
./Common/app/kvs_webrtc/media/lcd_preview.d \
./Common/app/kvs_webrtc/media/media_cam.d \
./Common/app/kvs_webrtc/media/media_enc.d \
./Common/app/kvs_webrtc/media/stm32_media_port.d \
./Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.d 


# Each subdirectory must supply rules for building sources it contributes
Common/app/kvs_webrtc/media/%.o Common/app/kvs_webrtc/media/%.su Common/app/kvs_webrtc/media/%.cyclo: ../Common/app/kvs_webrtc/media/%.c Common/app/kvs_webrtc/media/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m55 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32N657xx -DST67_ARCH=W6X_ARCH_T02 -DUSE_CUSTOM_SYSTICK_HANDLER_IMPLEMENTATION=1 '-DMBEDTLS_CONFIG_FILE="mbedtls_config_hw.h"' '-DLFS_CONFIG=lfs_config.h' -DST67W6X_RCP -DHW_CRYPTO -DHAVE_ARPA_INET_H=1 -DENABLE_SCTP_DATA_CHANNEL=0 -DHAVE_CONFIG_H=1 -DMBEDTLS=1 -DSDP_DO_NOT_USE_CUSTOM_CONFIG -DMETRIC_PRINT_ENABLED=1 -DMBEDTLS_DTLS_DEBUG_C -DportTICK_RATE_MS=portTICK_PERIOD_MS -DLIBRARY_LOG_LEVEL=3 -DSPI_THREAD_STACK_SIZE=2048 -DENABLE_AI_DETECTION -DLL_ATON_PLATFORM=LL_ATON_PLAT_STM32N6 -DLL_ATON_OSAL=LL_ATON_OSAL_FREERTOS -DLL_ATON_RT_MODE=LL_ATON_RT_ASYNC -DLL_ATON_SW_FALLBACK -DLL_ATON_DBG_BUFFER_INFO_EXCLUDED=1 -c -O2 -ffunction-sections -fdata-sections -Wall -Wno-error=incompatible-pointer-types -fstack-usage -fcyclomatic-complexity -mcmse -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@"  -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb -o "$@" @"Common/app/kvs_webrtc/media/lcd_preview.c_includes.args"

clean: clean-Common-2f-app-2f-kvs_webrtc-2f-media

clean-Common-2f-app-2f-kvs_webrtc-2f-media:
	-$(RM) ./Common/app/kvs_webrtc/media/lcd_preview.cyclo ./Common/app/kvs_webrtc/media/lcd_preview.d ./Common/app/kvs_webrtc/media/lcd_preview.o ./Common/app/kvs_webrtc/media/lcd_preview.su ./Common/app/kvs_webrtc/media/media_cam.cyclo ./Common/app/kvs_webrtc/media/media_cam.d ./Common/app/kvs_webrtc/media/media_cam.o ./Common/app/kvs_webrtc/media/media_cam.su ./Common/app/kvs_webrtc/media/media_enc.cyclo ./Common/app/kvs_webrtc/media/media_enc.d ./Common/app/kvs_webrtc/media/media_enc.o ./Common/app/kvs_webrtc/media/media_enc.su ./Common/app/kvs_webrtc/media/stm32_media_port.cyclo ./Common/app/kvs_webrtc/media/stm32_media_port.d ./Common/app/kvs_webrtc/media/stm32_media_port.o ./Common/app/kvs_webrtc/media/stm32_media_port.su ./Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.cyclo ./Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.d ./Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.o ./Common/app/kvs_webrtc/media/stm32n6570_discovery_bus_impl.su

.PHONY: clean-Common-2f-app-2f-kvs_webrtc-2f-media

