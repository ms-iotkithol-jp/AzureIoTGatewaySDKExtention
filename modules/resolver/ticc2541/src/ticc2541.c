// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include <glib.h>

#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/constmap.h"
#include "azure_c_shared_utility/constbuffer.h"

#include "module.h"
#include "message.h"
#include "broker.h"
#include "ticc2541.h"
#include "messageproperties.h"

#include <parson.h>

#define SENSOR_MASK_TEMPERATURE     0x00000002
#define SENSOR_MASK_HUMIDITY        0x00000004
#define SENSOR_MASK_MOVEMENT        0x00000008
#define SENSOR_MASK_PRESSURE        0x00000010
#define SENSOR_MASK_PRESSURE_CARIB  0x00000001

typedef struct CC2541_PRESSURE_CALIB_TAG
{
	uint16_t c1;
	uint16_t c2;
	uint16_t c3;
	uint16_t c4;
	int16_t c5;
	int16_t c6;
	int16_t c7;
	int16_t c8;
} CC2541_PRESSURE_CALIB;

typedef struct TI_CC2541_RESOLVER_HANDLE_DATA_TAG
{
    BROKER_HANDLE broker;
    double lastAmbience;
    double lastObject;
    double lastHumidity;
    double lastHumidityTemperature;
    double lastPressure;
    double lastAccelX;
    double lastAccelY;
    double lastAccelZ;
    CC2541_PRESSURE_CALIB pressureCalibBuf_cc2541;
    int flag;
    int availables;
} TI_CC2541_RESOLVER_HANDLE_DATA;

typedef void (*MESSAGE_RESOLVER)(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);

static void resolve_temperature(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_humidity(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_pressure(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_pressure_calib(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_accelerometer(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);


typedef struct DIPATCH_ENTRY_TAG
{
    const char* name;
    const char* characteristic_uuid;
    MESSAGE_RESOLVER message_resolver;
}DIPATCH_ENTRY;

// setup the message printers
DIPATCH_ENTRY g_dispatch_entries[] =
{
	{
		"Temperature",
		"F000AA01-0451-4000-B000-000000000000",
		resolve_temperature
	},
	{
		"Humidity",
		"F000AA21-0451-4000-B000-000000000000",
		resolve_humidity
	},
	{
		"Pressure Calibration",
		"F000AA43-0451-4000-B000-000000000000",
		resolve_pressure_calib
	},
	{
		"Pressure",
		"F000AA41-0451-4000-B000-000000000000",
		resolve_pressure
	},
	{
		"Accelerometer",
		"F000AA11-0451-4000-B000-000000000000",
		resolve_accelerometer
	}
};

size_t g_dispatch_entries_length = sizeof(g_dispatch_entries) / sizeof(g_dispatch_entries[0]);

/**
* Code taken from:
*    http://processors.wiki.ti.com/index.php/SensorTag_User_Guide
*/
// http://processors.wiki.ti.com/index.php/SensorTag_User_Guide
// http://processors.wiki.ti.com/images/a/a8/BLE_SensorTag_GATT_Server.pdf
static void resolve_temperature(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
	if (buffer->size == 4) {
		uint16_t* temps = (uint16_t *)buffer->buffer;
		float ambient, object;
		ambient = ((float)temps[1]) / 128;
		double vobj2 = (double)(temps[0]);
		vobj2 *= 0.00000015625;
		double tdie2 = ambient + 273.15;
		const double s0 = 6.4e-14;
		const double a1 = 1.75E-3;
		const double a2 = -1.678E-5;
		const double b0 = -2.94E-5;
		const double b1 = -5.7E-7;
		const double b2 = 4.63E-9;
		const double c2 = 13.4;
		const double tref = 298.15;
		double s = 50 * (1 + a1*(tdie2 - tref) + a2*(tdie2 - tref)*(tdie2 - tref));
		double vos = b0 + b1*(tdie2 - tref) + b2*(tdie2 - tref)*(tdie2 - tref);
		double fobj = (vobj2 - vos) + c2*(vobj2 - vos)*(vobj2 - vos);
		double tobj = pow(pow(tdie2, 4) + (fobj / s), 0.25);

		object = tobj - 273.15;

//		sprintf(resolveTempBuf, "\"ambience\":%f,\"object\":%f",
//			ambient,
//			object
//		);
        handle->lastAmbience = ambient;
        handle->lastObject = object;
        handle->flag |= SENSOR_MASK_TEMPERATURE;
	}
//	return STRING_construct(resolveTempBuf);
}

static void resolve_pressure(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
	if (buffer->size == 4 && ((handle->flag & SENSOR_MASK_PRESSURE_CARIB) != 0)) {
		uint16_t* presses = (uint16_t *)buffer->buffer;
		uint16_t rawP = presses[1];

		int64_t s, o, pres, val;
		uint16_t c3, c4;
		int16_t c5, c6, c7, c8;
		uint16_t Pr;
		int16_t Tr;

		Pr = presses[1];
		Tr = presses[0];
		c3 = handle->pressureCalibBuf_cc2541.c3;
		c4 = handle->pressureCalibBuf_cc2541.c4;
		c5 = handle->pressureCalibBuf_cc2541.c5;
		c6 = handle->pressureCalibBuf_cc2541.c6;
		c7 = handle->pressureCalibBuf_cc2541.c7;
		c8 = handle->pressureCalibBuf_cc2541.c8;

		// Sensitivity
		s = (int64_t)c3;
		val = (int64_t)c4 * Tr;
		s += (val >> 17);
		val = (int64_t)c5 * Tr * Tr;
		s += (val >> 34);

		// Offset
		o = (int64_t)c6 << 14;
		val = (int64_t)c7 * Tr;
		o += (val >> 3);
		val = (int64_t)c8 * Tr * Tr;
		o += (val >> 19);

		// Pressure (Pa)
		pres = ((int64_t)(s * Pr) + o) >> 14;
		double pressure = ((double)pres) / 100.0;
//		sprintf(resolveTempBuf, "\"pressure\",:%f",
//			pressure,
//		);
        handle->lastPressure = pressure;
        handle->flag |= SENSOR_MASK_PRESSURE;
	}
//	return STRING_construct(resolveTempBuf);
}

static void resolve_pressure_calib(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
	if (buffer->size == 16) {
		size_t i = 0;
		uint16_t* ptr = (uint16_t*)buffer->buffer;
		handle->pressureCalibBuf_cc2541.c1 = ptr[i++];
		handle->pressureCalibBuf_cc2541.c2 = ptr[i++];
		handle->pressureCalibBuf_cc2541.c3 = ptr[i++];
		handle->pressureCalibBuf_cc2541.c4 = ptr[i++];
		handle->pressureCalibBuf_cc2541.c5 = (int16_t)ptr[i++];
		handle->pressureCalibBuf_cc2541.c6 = (int16_t)ptr[i++];
		handle->pressureCalibBuf_cc2541.c7 = (int16_t)ptr[i++];
		handle->pressureCalibBuf_cc2541.c8 = (int16_t)ptr[i++];
//		sprintf(resolveTempBuf, "\"pressure-calib\":\"%s\"",
//			STRING_c_str(rawData)
//		);
        handle->flag |= SENSOR_MASK_PRESSURE_CARIB;
		LogInfo("Pressure Calibration:%04x-%04x-%04x-%04x-%04x-%04x-%04x-%04x", ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7]);
	}
//	return STRING_construct(resolveTempBuf);
}

static void resolve_humidity(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
	if (buffer->size == 4) {
		uint16_t* hums = (uint16_t *)buffer->buffer;
		float htemp, humidity;
		htemp = -46.85 + 175.72 / 65536 * (double)(int16_t)hums[0];
		uint16_t rawH = hums[1];
		rawH &= ~0x0003;
		humidity = -6.0 + 125.0 / 65536 * (double)rawH;
//		sprintf(resolveTempBuf, "\"humidityTemperature\":%f,\"humidity\":%f",
//			htemp,
//			humidity
//		);
        handle->lastHumidity = humidity;
        handle->lastHumidityTemperature = htemp;
        handle->flag |= SENSOR_MASK_HUMIDITY;
	}
//	return STRING_construct(resolveTempBuf);
}

static void resolve_accelerometer(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
	if (buffer->size == 3) {
		int8_t* accel = (int8_t*)buffer->buffer;
		float accelx = ((float)accel[0]) / 64;
		float accely = ((float)accel[1]) / 64;
		float accelz = ((float)accel[2]) / 64;
//		sprintf(resolveTempBuf, "\"accelx:%f,\"accely\":%f,\"accelz\":%f",
//			accelx,
//			accely,
//			accelz);
        handle->lastAccelX = accelx;
        handle->lastAccelY = accely;
        handle->lastAccelZ = accelz;
        handle->flag |= SENSOR_MASK_MOVEMENT;
	}
//	return STRING_construct(resolveTempBuf);
}

static void resolve_default(TI_CC2541_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    /*
	STRING_HANDLE result = STRING_construct("\"");
	STRING_concat(result, name);
	STRING_concat(result, "\":\"");
	STRING_HANDLE rawData = getRawData(buffer->buffer, buffer->size);
	sprintf(resolveTempBuf, "%s", STRING_c_str(rawData));
	STRING_concat(result, resolveTempBuf);
	STRING_concat(result, "\"");
	return result;
    */
}


MODULE_HANDLE TI_CC2541_Resolver_Create(BROKER_HANDLE broker, const void* configuration)
{
    TI_CC2541_RESOLVER_HANDLE_DATA* result;
        if(broker == NULL)
    {
        LogError("NULL parameter detected broker=%p", broker);
        result = NULL;
    }
    else
    {
        result = (TI_CC2541_RESOLVER_HANDLE_DATA*)malloc(sizeof(TI_CC2541_RESOLVER_HANDLE_DATA));
        if (result == NULL)
        {
            /*Codes_SRS_BLE_CTOD_13_002: [ TI_CC2541_RESOLVER_Create shall return NULL if any of the underlying platform calls fail. ]*/
            LogError("malloc failed");
        }
        else
        {
            result->broker = broker;
            result->flag = 0;
            if (configuration!=NULL){
                TICC2541_CONFIG* config = (TICC2541_CONFIG*)configuration;
                result->availables = config->availables;
            }
        }
    }
    
    /*Codes_SRS_BLE_CTOD_13_023: [ TI_CC2541_RESOLVER_Create shall return a non-NULL handle when the function succeeds. ]*/

    return (MODULE_HANDLE)result;
}

void* TI_CC2541_Resolver_ParseConfigurationFromJson(const char* configuration)
{
    TICC2541_CONFIG* config = NULL;
    if (configuration!=NULL){
        JSON_Value* json = json_parse_string((const char*)configuration);
        if (json == NULL){
            // error
        }
        else {
            JSON_Object* root = json_value_get_object(json);
            if (root==NULL){
                // error
            }
            else {
                config = (TICC2541_CONFIG*)malloc(sizeof(TICC2541_CONFIG));
                config->availables = 0;
//                config->accRange = 0;
                JSON_Array* sensorTypes = json_object_get_array(root,SENSOR_TYPES_JSON);
                if (sensorTypes==NULL){
                    // error
                }
                else{
                    size_t count = json_array_get_count(sensorTypes);
                    for (size_t i=0;i<count;i++){
                        JSON_Object* sensorType = json_array_get_object(sensorTypes,i);
                        if (sensorType==NULL){
                            // error
                        }
                        const char* typeName = json_object_get_string(sensorType,SENSOR_TYPE_JSON);
                        if (typeName == NULL){
                            // error
                        }
                        else {
                            if (g_ascii_strcasecmp(typeName, SENSOR_TYPE_TEMPERATURE)==0){
                                config->availables |= SENSOR_MASK_TEMPERATURE;
                            } else if (g_ascii_strcasecmp(typeName, SENSOR_TYPE_HUMIDITY)==0){
                                config->availables |= SENSOR_MASK_HUMIDITY;
                            } else if (g_ascii_strcasecmp(typeName, SENSOR_TYPE_PRESSURE)==0){
                                config->availables |= SENSOR_MASK_PRESSURE;                                
                       //         config->availables |= SENSOR_MASK_PRESSURE_CARIB;
                            } else if (g_ascii_strcasecmp(typeName, SENSOR_TYPE_MOVEMENT)==0){
                                config->availables |= SENSOR_MASK_MOVEMENT;
//                                const char* movementConfig = json_object_get_string(sensorType,SENSOR_CONFIG_JSON);
//                                if (movementConfig!=NULL){
                                    // set accRange
//                                }
                            } else {
                                // error
                            }
                        }
                    }
                }
            }
        }
    }
    return config;
}

void TI_CC2541_Resolver_FreeConfiguration(void * configuration)
{
    if (configuration!=NULL){
        free(configuration);
    }
    return;
}

#define MSG_TMP_BUF_SIZE 1024

void TI_CC2541_Resolver_Receive(MODULE_HANDLE module, MESSAGE_HANDLE message)
{
    TI_CC2541_RESOLVER_HANDLE_DATA* handle;
    if (module == NULL){
        LogError("module is NULL");
    }
    else{
        if (message != NULL)
        {
            CONSTMAP_HANDLE props = Message_GetProperties(message);
            if (props != NULL)
            {
                const char* source = ConstMap_GetValue(props, GW_SOURCE_PROPERTY);
                if (source != NULL && strcmp(source, GW_SOURCE_BLE_TELEMETRY) == 0)
                {
                    handle = (TI_CC2541_RESOLVER_HANDLE_DATA*)module;

                    const char* ble_controller_id = ConstMap_GetValue(props, GW_BLE_CONTROLLER_INDEX_PROPERTY);
                    const char* mac_address_str = ConstMap_GetValue(props, GW_MAC_ADDRESS_PROPERTY);
                    const char* timestamp = ConstMap_GetValue(props, GW_TIMESTAMP_PROPERTY);
                    const char* characteristic_uuid = ConstMap_GetValue(props, GW_CHARACTERISTIC_UUID_PROPERTY);
                    const CONSTBUFFER* buffer = Message_GetContent(message);
                    if (buffer != NULL && characteristic_uuid != NULL)
                    {
                        // dispatch the message based on the characteristic uuid
                        size_t i;
                        for (i = 0; i < g_dispatch_entries_length; i++)
                        {
                            if (g_ascii_strcasecmp(
                                    characteristic_uuid,
                                    g_dispatch_entries[i].characteristic_uuid
                                ) == 0)
                            {
                                g_dispatch_entries[i].message_resolver(
                                    handle,
                                    g_dispatch_entries[i].name,
                                    buffer
                                );
                                break;
                            }
                        }

                        if (i == g_dispatch_entries_length)
                        {
                            // dispatch to default printer
                            resolve_default(handle, characteristic_uuid, buffer);
                        }
                        if((handle->flag & handle->availables) == handle->availables)
                        {
                            JSON_Value* json_init = json_value_init_object();
                            JSON_Object* json = json_value_get_object(json_init);
                            JSON_Status json_status = json_object_set_string(json, "time", timestamp);
                            if ((handle->flag&SENSOR_MASK_TEMPERATURE)!=0) {
                                json_status = json_object_set_number(json, "ambience", handle->lastAmbience);
                                json_status= json_object_set_number(json,"object", handle->lastObject);
                            }
                            if ((handle->flag&SENSOR_MASK_HUMIDITY)!=0){
                                json_status = json_object_set_number(json, "humidity", handle->lastHumidity);
                                json_status = json_object_set_number(json, "humidityTemperature", handle->lastHumidityTemperature);
                            }
                            if ((handle->flag&SENSOR_MASK_PRESSURE)!=0){
                                json_status = json_object_set_number(json, "pressure", handle->lastPressure);
                            }
                            if ((handle->flag & SENSOR_MASK_MOVEMENT)!=0){
                                json_status = json_object_set_number(json, "accelx", handle->lastAccelX);
                                json_status = json_object_set_number(json, "accely", handle->lastAccelY);
                                json_status = json_object_set_number(json, "accelz", handle->lastAccelZ);
                            }
                            
                            char* jsonContent = json_serialize_to_string_pretty(json_init);
                            // current version timestamp format is bad

                            CONSTBUFFER_HANDLE msgContent = CONSTBUFFER_Create((const unsigned char*)jsonContent, strlen(jsonContent));
                            MAP_HANDLE msgProps = ConstMap_CloneWriteable(props);
                            MESSAGE_BUFFER_CONFIG msgBufConfig =  {
                                msgContent,
                                msgProps
                            };
                            MESSAGE_HANDLE newMessage = Message_CreateFromBuffer(&msgBufConfig);

                            if (newMessage == NULL)
                            {
                                LogError("Message_CreateFromBuffer() failed");
                            }
                            else
                            {
                                if (Broker_Publish(handle->broker, (MODULE_HANDLE)handle, newMessage) != BROKER_OK)
                                {
                                    LogError("Broker_Publish() failed");
                                }
                                handle->flag = 0;

                                Message_Destroy(newMessage);
                            }
                            json_free_serialized_string(jsonContent);
                            json_value_free(json_init);
                            
                            CONSTBUFFER_Destroy(msgContent);
              //      ConstMap_Destory(props);

                        }
                    }
                    else
                    {
                        LogError("Message is invalid. Nothing to print.");
                    }
                }
            }
            else
            {
                LogError("Message_GetProperties for the message returned NULL");
            }
        }
        else
        {
            LogError("message is NULL");
        }
    }
}

void TI_CC2541_Resolver_Destroy(MODULE_HANDLE module)
{
    // Nothing to do here
}

static const MODULE_API_1 Module_GetApi_Impl =
{
    {MODULE_API_VERSION_1},

    TI_CC2541_Resolver_ParseConfigurationFromJson,
    TI_CC2541_Resolver_FreeConfiguration,
    TI_CC2541_Resolver_Create,
    TI_CC2541_Resolver_Destroy,
    TI_CC2541_Resolver_Receive

};

MODULE_EXPORT const MODULE_API* Module_GetApi(MODULE_API_VERSION gateway_api_version)
{
    (void)gateway_api_version;
    return (const MODULE_API*)&Module_GetApi_Impl;
}

