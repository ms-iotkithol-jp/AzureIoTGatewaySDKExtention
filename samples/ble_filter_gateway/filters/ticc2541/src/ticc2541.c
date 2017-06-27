// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

//#include <glib.h>

#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/constmap.h"
#include "azure_c_shared_utility/constbuffer.h"
#include "azure_c_shared_utility/base64.h"

#include "module.h"
#include "message.h"
#include "broker.h"
#include "ticc2541.h"
#include "messageproperties.h"
#include "filter_api.h"

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

typedef struct TI_CC2541_RESOLVER_CONTEXT_TAG
{
	RESOLVER_HANDLE resolverHandle;
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
} TI_CC2541_RESOLVER_CONTEXT;

typedef struct TI_CC2541_RESOLVER_CONTEXT_LIST_TAG
{
    TI_CC2541_RESOLVER_CONTEXT context;
    void* next;
} TI_CC2541_RESOLVER_CONTEXT_LIST;

typedef struct TI_CC2541_RESOLVER_DATA_TAG
{
    MODULE_HANDLE moduleHandle;
	const char* name;
    TI_CC2541_RESOLVER_CONTEXT_LIST* contexts;
} TI_CC2541_RESOLVER_DATA;

static STRING_HANDLE resolve_temperature(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_humidity(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_pressure(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_pressure_calib(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_accelerometer(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);

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
static STRING_HANDLE resolve_temperature(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
	(void)name;
	TI_CC2541_RESOLVER_CONTEXT* context = (TI_CC2541_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 4) {
		uint16_t* temps = (uint16_t *)buffer->buffer;
		float ambient, object;
		ambient = ((float)temps[1]) / (float)128.0f;
		float vobj2 = (float)(temps[0]);
		vobj2 *= (float)0.00000015625f;
		float tdie2 = ambient + (float)273.15;
		const float s0 = (float)(6.4e-14);
		const float a1 = (float)1.75E-3;
		const float a2 = (float)-1.678E-5;
		const float b0 = (float)-2.94E-5;
		const float b1 = (float)-5.7E-7;
		const float b2 = (float) 4.63E-9;
		const float c2 = (float)13.4;
		const float tref = (float) 298.15;
		float s = s0 * ((float)1.0f + a1*(tdie2 - tref) + a2*(tdie2 - tref)*(tdie2 - tref));
		float vos = b0 + b1*(tdie2 - tref) + b2*(tdie2 - tref)*(tdie2 - tref);
		float fobj = (vobj2 - vos) + c2*(vobj2 - vos)*(vobj2 - vos);
		float tobj = (float)pow(pow(tdie2, 4.0f) + (fobj / s), 0.25f);

		object = tobj - (float)273.15f;

		//		sprintf(resolveTempBuf, "\"ambience\":%f,\"object\":%f",
		//			ambient,
		//			object
		//		);
		context->lastAmbience = ambient;
		context->lastObject = object;
		context->flag |= SENSOR_MASK_TEMPERATURE;
	}
	//	return STRING_construct(resolveTempBuf);

	return NULL;
}

static STRING_HANDLE resolve_pressure(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2541_RESOLVER_CONTEXT* context = (TI_CC2541_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 4 && ((context->flag & SENSOR_MASK_PRESSURE_CARIB) != 0)) {
		uint16_t* presses = (uint16_t *)buffer->buffer;

		int64_t s, o, pres, val;
		uint16_t c3, c4;
		int16_t c5, c6, c7, c8;
		uint16_t Pr;
		int16_t Tr;

		Pr = presses[1];
		Tr = presses[0];
		c3 = context->pressureCalibBuf_cc2541.c3;
		c4 = context->pressureCalibBuf_cc2541.c4;
		c5 = context->pressureCalibBuf_cc2541.c5;
		c6 = context->pressureCalibBuf_cc2541.c6;
		c7 = context->pressureCalibBuf_cc2541.c7;
		c8 = context->pressureCalibBuf_cc2541.c8;

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
        context->lastPressure = pressure;
        context->flag |= SENSOR_MASK_PRESSURE;
	}
	return NULL;
}

static STRING_HANDLE resolve_pressure_calib(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2541_RESOLVER_CONTEXT* context = (TI_CC2541_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 16) {
		size_t i = 0;
		uint16_t* ptr = (uint16_t*)buffer->buffer;
		context->pressureCalibBuf_cc2541.c1 = ptr[i++];
		context->pressureCalibBuf_cc2541.c2 = ptr[i++];
		context->pressureCalibBuf_cc2541.c3 = ptr[i++];
		context->pressureCalibBuf_cc2541.c4 = ptr[i++];
		context->pressureCalibBuf_cc2541.c5 = (int16_t)ptr[i++];
		context->pressureCalibBuf_cc2541.c6 = (int16_t)ptr[i++];
		context->pressureCalibBuf_cc2541.c7 = (int16_t)ptr[i++];
		context->pressureCalibBuf_cc2541.c8 = (int16_t)ptr[i++];
//		sprintf(resolveTempBuf, "\"pressure-calib\":\"%s\"",
//			STRING_c_str(rawData)
//		);
        context->flag |= SENSOR_MASK_PRESSURE_CARIB;
		LogInfo("Pressure Calibration:%04x-%04x-%04x-%04x-%04x-%04x-%04x-%04x", ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7]);
	}
	return NULL;
}

static STRING_HANDLE resolve_humidity(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
	(void)name;
	TI_CC2541_RESOLVER_CONTEXT* context = (TI_CC2541_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 4) {
		uint16_t* hums = (uint16_t *)buffer->buffer;
		float htemp, humidity;
		htemp = (float)(-46.85 + 175.72 / 65536 * (double)(int16_t)hums[0]);
		uint16_t rawH = hums[1];
		rawH &= ~0x0003;
		humidity = (float)(-6.0 + 125.0 / 65536 * (double)rawH);
		//		sprintf(resolveTempBuf, "\"humidityTemperature\":%f,\"humidity\":%f",
		//			htemp,
		//			humidity
		//		);
		context->lastHumidity = humidity;
		context->lastHumidityTemperature = htemp;
		context->flag |= SENSOR_MASK_HUMIDITY;
	}
	return NULL;
}

static STRING_HANDLE resolve_accelerometer(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
	TI_CC2541_RESOLVER_CONTEXT* context = (TI_CC2541_RESOLVER_CONTEXT*)handle;
    (void)name;
	if (buffer->size == 3) {
		int8_t* accel = (int8_t*)buffer->buffer;
		float accelx = ((float)accel[0]) / 64;
		float accely = ((float)accel[1]) / 64;
		float accelz = ((float)accel[2]) / 64;
//		sprintf(resolveTempBuf, "\"accelx:%f,\"accely\":%f,\"accelz\":%f",
//			accelx,
//			accely,
//			accelz);
        context->lastAccelX = accelx;
        context->lastAccelY = accely;
        context->lastAccelZ = accelz;
        context->flag |= SENSOR_MASK_MOVEMENT;
	}

	return NULL;
}

static STRING_HANDLE resolve_default(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)handle;
	(void)name;
	STRING_HANDLE result = STRING_construct("");
	char* tmpRawData = (char*)malloc(buffer->size);
	memcpy(tmpRawData, buffer->buffer, buffer->size);
	STRING_concat(result, tmpRawData);
	free(tmpRawData);
	return result;
}

RESOLVER_HANDLE TICC2541_Create(MODULE_HANDLE module, const char* resolverName)
{
	TI_CC2541_RESOLVER_DATA* handle = (TI_CC2541_RESOLVER_DATA*)malloc(sizeof(TI_CC2541_RESOLVER_DATA));
	handle->moduleHandle = module;
	handle->contexts = NULL;
	handle->name = NULL;
	char* tempRN = (char*)malloc(strlen(resolverName) + 1);
	strcpy(tempRN, resolverName);
	handle->name = tempRN;

	return handle;
}

RESOLVER_CONTEXT* TICC2541_CreateContext(MODULE_HANDLE module , JSON_Object* configuration)
{
    TI_CC2541_RESOLVER_DATA* handle = (TI_CC2541_RESOLVER_DATA*)module;
    TI_CC2541_RESOLVER_CONTEXT_LIST* element = (TI_CC2541_RESOLVER_CONTEXT_LIST*)malloc(sizeof(TI_CC2541_RESOLVER_CONTEXT_LIST));
	element->context.resolverHandle=handle;
	element->context.availables = 0;
	element->context.flag = 0;
	element->next = NULL;
    TI_CC2541_RESOLVER_CONTEXT_LIST* contexts = handle->contexts;
    if (contexts == NULL)
    {
        handle->contexts = element;
    }
    else
    {
        while(contexts->next!=NULL)
        {
            contexts = (TI_CC2541_RESOLVER_CONTEXT_LIST*)contexts->next;
        }
        contexts->next = element;
    }

	JSON_Array* sensorTypes = json_object_get_array(configuration, "sensor-types");
	if (sensorTypes == NULL) {
		// error
		LogError("Can't find 'sensor-types' specification.");
	}
	else {
		size_t count = json_array_get_count(sensorTypes);
		for (size_t i = 0; i < count; i++) {
			JSON_Object* sensorType = json_array_get_object(sensorTypes, i);
			if (sensorType == NULL) {
				// error
				LogError("Can't find 'sensor-type' specification.");
			}
			const char* typeName = json_object_get_string(sensorType, SENSOR_TYPE_JSON);
			if (typeName == NULL) {
				// error
				LogError("Can't find 'sensor-type' value.");
			}
			else {
				if (strcmp(typeName, SENSOR_TYPE_TEMPERATURE) == 0) {
					element->context.availables |= SENSOR_MASK_TEMPERATURE;
				}
				else if (strcmp(typeName, SENSOR_TYPE_HUMIDITY) == 0) {
					element->context.availables |= SENSOR_MASK_HUMIDITY;
				}
				else if (strcmp(typeName, SENSOR_TYPE_PRESSURE) == 0) {
					element->context.availables |= SENSOR_MASK_PRESSURE;
       //             element->context.availables|=SENSOR_MASK_PRESSURE_CARIB;
				}
				else if (strcmp(typeName, SENSOR_TYPE_MOVEMENT) == 0) {
					element->context.availables |= SENSOR_MASK_MOVEMENT;
				}
				else {
					// error
				}
			}
		}
	}
    // TODO: parse JSON 
    return (RESOLVER_CONTEXT*)&element->context;
}

void TICC2541_Destroy(RESOLVER_HANDLE resolverHandle)
{
    TI_CC2541_RESOLVER_DATA* handle = (TI_CC2541_RESOLVER_DATA*)resolverHandle;

    TI_CC2541_RESOLVER_CONTEXT_LIST* contexts = handle->contexts;
	while (contexts != NULL)
	{
		TI_CC2541_RESOLVER_CONTEXT_LIST* next = contexts->next;
		free(contexts);
		contexts = next;
	}
	if (handle->name != NULL)
	{
		free((void*)handle->name);
	}
}

CONSTBUFFER_HANDLE TICC2541_Resolve(RESOLVER_CONTEXT* context, const char* characteristics, const char* timestamp, const CONSTBUFFER* message)
{
	CONSTBUFFER_HANDLE buffer = NULL;
	TI_CC2541_RESOLVER_CONTEXT* currentContext = (TI_CC2541_RESOLVER_CONTEXT*)context;
	size_t i;
	for (i = 0; i < g_dispatch_entries_length; i++)
	{
		if (strcmp(
			characteristics,
			g_dispatch_entries[i].characteristic_uuid
		) == 0)
		{
			g_dispatch_entries[i].message_resolver(
				context,
				g_dispatch_entries[i].name,
				message
			);
		}
	}
	JSON_Value* json_init = json_value_init_object();
	JSON_Object* json = json_value_get_object(json_init);
	char* jsonContent = NULL;
	if (i == g_dispatch_entries_length)
	{
		// dispatch to default printer
		STRING_HANDLE defResolved = resolve_default(context, characteristics, message);
		if (defResolved != NULL)
		{
			buffer = CONSTBUFFER_Create((const unsigned char*)STRING_c_str(defResolved), STRING_length(defResolved));
			JSON_Status json_status = json_object_set_string(json, "time", timestamp);
			if (json_status == JSONFailure)
			{
				LogError("json time set - failed");
			}
			else
			{
				json_status = json_object_set_string(json, "characteristics-uuid", characteristics);
			}
			if (json_status == JSONFailure)
			{
				LogError("json characteristics-uuid set - failed");
			}
			else
			{
				json_status = json_object_set_string(json, "message", STRING_c_str(defResolved));

			}
			if (json_status == JSONFailure)
			{
				LogError("json message set - failed");
				BUFFER_HANDLE tmpBuffer = BUFFER_create(message->buffer, message->size);
				STRING_HANDLE base64 = Base64_Encode(tmpBuffer);
				json_status = json_object_set_string(json, "message", STRING_c_str(base64));
			}
			else
			{
				jsonContent = json_serialize_to_string_pretty(json_init);
			}
			json_value_free(json_init);
		}
	}
	else
	{
		if ((currentContext->flag & currentContext->availables) == currentContext->availables)
		{
			currentContext->flag &= currentContext->availables;
			JSON_Status json_status = json_object_set_string(json, "time", timestamp);
			if ((currentContext->flag&SENSOR_MASK_TEMPERATURE) != 0) {
				json_status |= json_object_set_number(json, "ambience", currentContext->lastAmbience);
				json_status |= json_object_set_number(json, "object", currentContext->lastObject);
			}
			if ((currentContext->flag&SENSOR_MASK_HUMIDITY) != 0) {
				json_status |= json_object_set_number(json, "humidity", currentContext->lastHumidity);
				json_status |= json_object_set_number(json, "humidityTemperature", currentContext->lastHumidityTemperature);
			}
			if ((currentContext->flag&SENSOR_MASK_PRESSURE) != 0) {
				json_status |= json_object_set_number(json, "pressure", currentContext->lastPressure);
			}
			if ((currentContext->flag & SENSOR_MASK_MOVEMENT) != 0) {
				json_status |= json_object_set_number(json, "accelx", currentContext->lastAccelX);
				json_status |= json_object_set_number(json, "accely", currentContext->lastAccelY);
				json_status |= json_object_set_number(json, "accelz", currentContext->lastAccelZ);
			}

			if (json_status == JSONSuccess)
			{
				jsonContent = json_serialize_to_string_pretty(json_init);
			}
			else {
				LogError("failed to resolved data to JSON string");
			}
			currentContext->flag = (currentContext->flag&SENSOR_MASK_PRESSURE_CARIB);
		}
	}
	if (jsonContent != NULL)
	{
		// I don't know how to remove /'s prefix.
		char* tmpBuf = (char*)malloc(strlen(jsonContent));
		int tmpi = 0;
		int tmpj = 0;
		while (jsonContent[tmpi] != '\0') {
			if (jsonContent[tmpi] == 0x5c && jsonContent[tmpi + 1] == 0x2f) {
				tmpBuf[tmpj] = jsonContent[tmpi + 1];
				tmpi++;
			}
			else {
				tmpBuf[tmpj] = jsonContent[tmpi];
			}
			tmpi++; tmpj++;
		}
		tmpBuf[tmpj] = '\0';
		buffer = CONSTBUFFER_Create((const unsigned char*)tmpBuf, strlen(tmpBuf));
		json_free_serialized_string(jsonContent);
	}
	json_value_free(json_init);
	return buffer;
}


static const RESOLVER_API_1 Resolver_GetApi_Impl =
{
    {RESOLVER_API_VERSION_1},
    TICC2541_Create,
    TICC2541_CreateContext,
    TICC2541_Resolve,
    TICC2541_Destroy
};

RESOLVER_EXPORT const RESOLVER_API* Resolver_GetApi(RESOLVER_API_VERSION resolver_api_version)
{
    (void)resolver_api_version;
    return (const RESOLVER_API*)&Resolver_GetApi_Impl;
}
