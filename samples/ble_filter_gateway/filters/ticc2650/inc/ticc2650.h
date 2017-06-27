// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef TI_CC2650_RESOLVER_H
#define TI_CC2650_RESOLVER_H

#include "module.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SENSOR_TYPE_TEMPERATURE "temperature"
#define SENSOR_TYPE_HUMIDITY    "humidity"
#define SENSOR_TYPE_PRESSURE    "pressure"
#define SENSOR_TYPE_MOVEMENT    "movement"
#define SENSOR_TPYE_BRIGHTNESS  "brightness"

#define TARGETS_JSON             "targets"
#define SENSOR_TAG_JSON         "sensor-tag"
#define SENSOR_TYPES_JSON       "sensor-types"
#define SENSOR_TYPE_JSON        "sensor-type"
#define SENSOR_CONFIG_JSON      "config"

typedef struct TICC2650_CONFIG_TAG
{
    int availables;
    int accRange;
    char* macAddress;
} TICC2650_CONFIG;

#ifdef __cplusplus
}
#endif

#endif /*TI_CC2650_RESOLVER_H*/
