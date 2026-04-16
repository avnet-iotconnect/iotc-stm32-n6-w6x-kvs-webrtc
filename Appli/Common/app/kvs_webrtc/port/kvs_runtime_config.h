/*
 * STM32N6570 KVS WebRTC port — kvs_runtime_config.h
 *
 * Provides the AWS_REGION / AWS_KVS_CHANNEL_NAME / credential macros that
 * app_common.c expects, but redirects them to runtime-mutable global char*
 * variables that kvs_webrtc_task.c populates from KVStore before starting
 * the signaling controller.
 *
 * Include this header BEFORE including app_common.h / demo_config.h when
 * compiling app_common.c in this project.
 *
 * Copyright (c) 2025 STMicroelectronics / project contributors.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef KVS_RUNTIME_CONFIG_H
#define KVS_RUNTIME_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime string buffers — populated by KvsWebRtcTask_SetConfig() before
 * AppCommon_StartSignalingController() is called.
 *
 * Declared extern here; defined once in kvs_webrtc_task.c.
 */
extern const char * pcKvsAwsRegion;
extern const char * pcKvsChannelName;
extern const char * pcKvsCredentialsEndpoint;  /* IoT role-alias path */
extern const char * pcKvsIotThingName;
extern const char * pcKvsIotRoleAlias;
extern const char * pcKvsIotThingCert;         /* PEM string */
extern const char * pcKvsIotPrivateKey;        /* PEM string */

/* Redirect the macro strings used in app_common.c to runtime pointers */
#undef  AWS_REGION
#define AWS_REGION              pcKvsAwsRegion

#undef  AWS_KVS_CHANNEL_NAME
#define AWS_KVS_CHANNEL_NAME    pcKvsChannelName

/* Enable IoT role-alias credential path unconditionally so that
 * app_common.c compiles the awsIotCreds branch.  The pointers will be
 * set to empty strings ("") when access-key auth is used instead.       */
#undef  AWS_IOT_THING_ROLE_ALIAS
#define AWS_IOT_THING_ROLE_ALIAS    pcKvsIotRoleAlias

#undef  AWS_CREDENTIALS_ENDPOINT
#define AWS_CREDENTIALS_ENDPOINT    pcKvsCredentialsEndpoint

#undef  AWS_IOT_THING_NAME
#define AWS_IOT_THING_NAME          pcKvsIotThingName

#undef  AWS_IOT_THING_CERT
#define AWS_IOT_THING_CERT          pcKvsIotThingCert

#undef  AWS_IOT_THING_PRIVATE_KEY
#define AWS_IOT_THING_PRIVATE_KEY   pcKvsIotPrivateKey

#ifdef __cplusplus
}
#endif

#endif /* KVS_RUNTIME_CONFIG_H */
