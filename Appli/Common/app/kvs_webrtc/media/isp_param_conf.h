/**
 * isp_param_conf.h — ISP IQ parameter configuration for the KVS WebRTC project.
 *
 * Included by cmw_imx335.c and cmw_vd66gy.c to obtain per-sensor ISP IQ
 * parameter tables.  VD55G1 does not use this file.
 *
 * SPDX-License-Identifier: LicenseRef-STMicroelectronics
 */

#ifndef __ISP_PARAM_CONF__H
#define __ISP_PARAM_CONF__H

#include "media_cam_config.h"   /* SENSOR_*_FLIP macros */
#include "cmw_camera.h"         /* CMW_MIRRORFLIP_* */

#ifdef USE_IMX335_SENSOR
#include "imx335_isp_param_conf.h"
#endif

#ifdef USE_VD66GY_SENSOR
#include "vd66gy_isp_param_conf.h"
#endif

static const ISP_IQParamTypeDef * ISP_IQParamCacheInit[] = {
#ifdef USE_IMX335_SENSOR
    &ISP_IQParamCacheInit_IMX335,
#endif
#ifdef USE_VD66GY_SENSOR
    &ISP_IQParamCacheInit_VD66GY,
#endif
};

#endif /* __ISP_PARAM_CONF__H */
