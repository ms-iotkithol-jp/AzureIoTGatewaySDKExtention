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
#include "ticc2650.h"
#include "messageproperties.h"
#include "filter_api.h"

#include <parson.h>

#define SENSOR_MASK_TEMPERATURE 0x00000001
#define SENSOR_MASK_HUMIDITY    0x00000002
#define SENSOR_MASK_MOVEMENT    0x00000004
#define SENSOR_MASK_PRESSURE    0x00000008
#define SENSOR_MASK_BRIGHTNESS  0x00000010

typedef struct TI_CC2650_RESOLVER_CONTEXT_TAG
{
	RESOLVER_HANDLE resolverHandle;
    double lastAmbience;
    double lastObject;
    double lastHumidity;
    double lastHumidityTemperature;
    double lastPressure;
    double lastPressureTemperature;
	int accRange;
    double lastAccelX;
    double lastAccelY;
    double lastAccelZ;
    double lastGyroX;
    double lastGyroY;
    double lastGyroZ;
    double lastMagX;
    double lastMagY;
    double lastMagZ;
    double lastBrightness;
    int flag;
    int availables;
} TI_CC2650_RESOLVER_CONTEXT;

typedef struct TI_CC2650_RESOLVER_CONTEXT_LIST_TAG
{
    TI_CC2650_RESOLVER_CONTEXT context;
    void* next;
} TI_CC2650_RESOLVER_CONTEXT_LIST;

typedef struct TI_CC2650_RESOLVER_DATA_TAG
{
    MODULE_HANDLE moduleHandle;
	const char* name;
    TI_CC2650_RESOLVER_CONTEXT_LIST* contexts;
} TI_CC2650_RESOLVER_DATA;

static STRING_HANDLE resolve_temperature(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_humidity(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_pressure(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_movement(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);
static STRING_HANDLE resolve_brightness(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);

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
		"Pressure",
		"F000AA41-0451-4000-B000-000000000000",
		resolve_pressure
	},
	{
		"Movement",
		"F000AA81-0451-4000-B000-000000000000",
		resolve_movement
	},
	{
		"Brightness",
		"F000AA71-0451-4000-B000-000000000000",
		resolve_brightness
	}
};

size_t g_dispatch_entries_length = sizeof(g_dispatch_entries) / sizeof(g_dispatch_entries[0]);

/**
* Code taken from:
*    http://processors.wiki.ti.com/index.php/CC2650_SensorTag_User's_Guide#Data
*/
static void sensortag_temp_convert(
	uint16_t rawAmbTemp,
	uint16_t rawObjTemp,
	float *tAmb,
	float *tObj
)
{
	const float SCALE_LSB = 0.03125;
	float t;
	int it;

	it = (int)((rawObjTemp) >> 2);
	t = ((float)(it)) * SCALE_LSB;
	*tObj = t;

	it = (int)((rawAmbTemp) >> 2);
	t = (float)it;
	*tAmb = t * SCALE_LSB;
}
static STRING_HANDLE resolve_temperature(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2650_RESOLVER_CONTEXT* context = (TI_CC2650_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 4) {
		uint16_t* temps = (uint16_t *)buffer->buffer;
		float ambient, object;
		sensortag_temp_convert(temps[0], temps[1], &ambient, &object);

//		sprintf(resolveTempBuf, "\"ambience\":%f,\"object\":%f",
//			ambient,
//			object
//		);
        context->lastAmbience = ambient;
        context->lastObject = object;
        context->flag |= SENSOR_MASK_TEMPERATURE;
	}
	return NULL;
}

static STRING_HANDLE resolve_pressure(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2650_RESOLVER_CONTEXT* context = (TI_CC2650_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 6) {
		unsigned char tempBuf[4];
		unsigned char pressBuf[4];
		uint8_t* data = (uint8_t *)buffer->buffer;
		tempBuf[0] = data[0];
		tempBuf[1] = data[1];
		tempBuf[2] = data[2];
		tempBuf[3] = 0;
		pressBuf[0] = data[3];
		pressBuf[1] = data[4];
		pressBuf[2] = data[5];
		pressBuf[3] = 0;
		uint32_t temp = *((uint32_t*)&tempBuf[0]);
		uint32_t pres = *((uint32_t*)&pressBuf[0]);
		float pressure = ((float)pres) / 100.0f;
		float temperature = ((float)temp) / 100.0f;
//		sprintf(resolveTempBuf, "\"pressure\",:%f",
//			pressure,
//		);
        context->lastPressure = pressure;
        context->lastPressureTemperature = temperature;
        context->flag |= SENSOR_MASK_PRESSURE;
	}
	return NULL;
}

static void sensortag_hum_convert(
	uint16_t rawTemp,
	uint16_t rawHum,
	float *tTemp,
	float *tHum
)
{
	*tTemp = ((float)(int16_t)rawTemp / 65536.0f) * 165.0f - 40.0f;
	*tHum = ((float)rawHum / 65536.0f) * 100.0f;
}

static STRING_HANDLE resolve_humidity(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2650_RESOLVER_CONTEXT* context = (TI_CC2650_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 4) {
		uint16_t* temps = (uint16_t *)buffer->buffer;
		float temp, hum;
		sensortag_hum_convert(temps[0], temps[1], &temp, &hum);
//		sprintf(resolveTempBuf, "\"humidityTemperature\":%f,\"humidity\":%f",
//			htemp,
//			humidity
//		);
        context->lastHumidity = hum;
        context->lastHumidityTemperature = temp;
        context->flag |= SENSOR_MASK_HUMIDITY;
	}

	return NULL;
}

#define ACC_RANGE_2G      0
#define ACC_RANGE_4G      1
#define ACC_RANGE_8G      2
#define ACC_RANGE_16G     3

#define ACC_RANGE_KEY_2G	"2G"
#define ACC_RANGE_KEY_4G	"4G"
#define ACC_RANGE_KEY_8G	"8G"
#define ACC_RANGE_KEY_16G	"16G"
 
float sensorMpu9250AccConvert(TI_CC2650_RESOLVER_CONTEXT* context, int16_t rawData)
{
  float v;
 
  switch (context->accRange)
  {
  case ACC_RANGE_2G:
	  //-- calculate acceleration, unit G, range -2, +2
	  v = (float)((rawData * 1.0) / (float)(32768 / 2));
	  break;

  case ACC_RANGE_4G:
	  //-- calculate acceleration, unit G, range -4, +4
	  v = (float)((rawData * 1.0) / (float)(32768 / 4));
	  break;

  case ACC_RANGE_8G:
	  //-- calculate acceleration, unit G, range -8, +8
	  v = (float)((rawData * 1.0) / (float)(32768.0f / 8.0f));
	  break;

  case ACC_RANGE_16G:
	  //-- calculate acceleration, unit G, range -16, +16
	  v = (float)((rawData * 1.0) / (float)(32768.0f / 16.0f));
	  break;
  }
 
  return v;
}

float sensorMpu9250GyroConvert(int16_t data)
{
	//-- calculate rotation, unit deg/s, range -250, +250
	return (float)((data * 1.0) / (float)(65536.0f / 500.0f));
}

float sensorMpu9250MagConvert(int16_t data)
{
	//-- calculate magnetism, unit uT, range +-4900
	return (float)1.0f * (float)data;
}

static STRING_HANDLE resolve_movement(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2650_RESOLVER_CONTEXT* context = (TI_CC2650_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 18) {
		int16_t* rawdata = (int16_t*)buffer->buffer;
		float gyrox = sensorMpu9250GyroConvert(rawdata[0]);
		float gyroy = sensorMpu9250GyroConvert(rawdata[1]);
		float gyroz = sensorMpu9250GyroConvert(rawdata[2]);
		float accelx = sensorMpu9250AccConvert(context, rawdata[3]);
		float accely = sensorMpu9250AccConvert(context, rawdata[4]);
		float accelz = sensorMpu9250AccConvert(context, rawdata[5]);
		float magx = sensorMpu9250MagConvert(rawdata[6]);
		float magy = sensorMpu9250MagConvert(rawdata[7]);
		float magz = sensorMpu9250MagConvert(rawdata[8]);
//		sprintf(resolveTempBuf, "\"accelx:%f,\"accely\":%f,\"accelz\":%f",
//			accelx,
//			accely,
//			accelz);
        context->lastAccelX = accelx;
        context->lastAccelY = accely;
        context->lastAccelZ = accelz;
        context->lastGyroX = gyrox;
        context->lastGyroY = gyroy;
        context->lastGyroZ = gyroz;
        context->lastMagX = magx;
        context->lastMagY = magy;
        context->lastMagZ = magz;
        context->flag |= SENSOR_MASK_MOVEMENT;
	}

	return NULL;
}

static STRING_HANDLE resolve_brightness(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	TI_CC2650_RESOLVER_CONTEXT* context = (TI_CC2650_RESOLVER_CONTEXT*)handle;
	if (buffer->size == 2)
	{
		uint16_t* temps = (uint16_t *)buffer->buffer;
		uint16_t e, m;
		m = temps[0] & 0xFFF;
		e = (temps[0] & 0xF000) >> 12;

		float brightness = ((float)m)*(float)(0.01*pow(2.0, e));
//		sprintf(resolveTempBuf, "\"brightness\":%f",
//			brightness
//		);
        context->lastBrightness = brightness;
        context->flag |= SENSOR_MASK_BRIGHTNESS;
	}
	return NULL;
}

static STRING_HANDLE resolve_default(RESOLVER_DATA_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer)
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


RESOLVER_HANDLE TICC2650_Create(MODULE_HANDLE module, const char* resolverName)
{
    TI_CC2650_RESOLVER_DATA* handle = (TI_CC2650_RESOLVER_DATA*)malloc(sizeof(TI_CC2650_RESOLVER_DATA));
    handle->moduleHandle = module;
    handle->contexts = NULL;
	handle->name = NULL;
	char* tmpRN = (char*)malloc(strlen(resolverName) + 1);
	strcpy(tmpRN, resolverName);
	handle->name = tmpRN;

    return handle;
}

RESOLVER_CONTEXT* TICC2650_CreateContext(MODULE_HANDLE module , JSON_Object* configuration)
{
    TI_CC2650_RESOLVER_DATA* handle = (TI_CC2650_RESOLVER_DATA*)module;
    TI_CC2650_RESOLVER_CONTEXT_LIST* element = (TI_CC2650_RESOLVER_CONTEXT_LIST*)malloc(sizeof(TI_CC2650_RESOLVER_CONTEXT_LIST));
	element->context.resolverHandle = handle;
	element->context.availables = 0;
	element->context.flag = 0;
	element->context.accRange = ACC_RANGE_2G;
	element->next = NULL;
    TI_CC2650_RESOLVER_CONTEXT_LIST* contexts = handle->contexts;
    if (contexts == NULL)
    {
        handle->contexts = element;
    }
    else
    {
        while(contexts->next!=NULL)
        {
            contexts = (TI_CC2650_RESOLVER_CONTEXT_LIST*)contexts->next;
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
				}
				else if (strcmp(typeName, SENSOR_TYPE_MOVEMENT) == 0) {
					element->context.availables |= SENSOR_MASK_MOVEMENT;
					const char* movementConfig = json_object_get_string(sensorType, SENSOR_CONFIG_JSON);
					if (movementConfig != NULL) {
						// set accRange
						if (strcmp(movementConfig, ACC_RANGE_KEY_2G) == 0)
						{
							element->context.accRange = ACC_RANGE_2G;
						}
						else if (strcmp(movementConfig, ACC_RANGE_KEY_4G) == 0)
						{
							element->context.accRange = ACC_RANGE_4G;
						}
						else if (strcmp(movementConfig, ACC_RANGE_KEY_8G) == 0)
						{
							element->context.accRange = ACC_RANGE_8G;
						}
						else if (strcmp(movementConfig, ACC_RANGE_KEY_16G) == 0)
						{
							element->context.accRange = ACC_RANGE_16G;
						}
					}
				}
				else if (strcmp(typeName, SENSOR_TPYE_BRIGHTNESS) == 0) {
					element->context.availables |= SENSOR_MASK_BRIGHTNESS;
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

void TICC2650_Destroy(RESOLVER_HANDLE resolverHandle)
{
    TI_CC2650_RESOLVER_DATA* handle = (TI_CC2650_RESOLVER_DATA*)resolverHandle;

    TI_CC2650_RESOLVER_CONTEXT_LIST* contexts = handle->contexts;
	while (contexts != NULL)
	{
		TI_CC2650_RESOLVER_CONTEXT_LIST* next = contexts->next;
		free(contexts);
		contexts = next;
	}
	if (handle->name != NULL)
	{
		free((void*)handle->name);
	}
}

CONSTBUFFER_HANDLE TICC2650_Resolve(RESOLVER_DATA_CONTEXT* context, const char* characteristics, const char* timestamp, const CONSTBUFFER* message)
{
	CONSTBUFFER_HANDLE buffer = NULL;
	TI_CC2650_RESOLVER_CONTEXT* currentContext = (TI_CC2650_RESOLVER_CONTEXT*)context;
	size_t i;
	for (i = 0; i < g_dispatch_entries_length; i++)
	{
		if (strcmp(
			characteristics,
			g_dispatch_entries[i].characteristic_uuid) == 0)
		{
			g_dispatch_entries[i].message_resolver(
				context,
				g_dispatch_entries[i].name,
				message
			);
			break;
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
				json_status |= json_object_set_number(json, "pressureTemperature", currentContext->lastPressureTemperature);
			}
			if ((currentContext->flag & SENSOR_MASK_MOVEMENT) != 0) {
				json_status |= json_object_set_number(json, "accelx", currentContext->lastAccelX);
				json_status |= json_object_set_number(json, "accely", currentContext->lastAccelY);
				json_status |= json_object_set_number(json, "accelz", currentContext->lastAccelZ);
				json_status |= json_object_set_number(json, "gyrox", currentContext->lastGyroX);
				json_status |= json_object_set_number(json, "gyroy", currentContext->lastGyroY);
				json_status |= json_object_set_number(json, "gyroz", currentContext->lastGyroZ);
				json_status |= json_object_set_number(json, "magx", currentContext->lastMagX);
				json_status |= json_object_set_number(json, "magy", currentContext->lastMagY);
				json_status |= json_object_set_number(json, "magz", currentContext->lastMagZ);
			}
			if ((currentContext->flag & SENSOR_MASK_BRIGHTNESS) != 0) {
				json_status |= json_object_set_number(json, "brightness", currentContext->lastBrightness);
			}
			if (json_status == JSONSuccess)
			{
	//			jsonContent = json_serialize_to_string_pretty(json_init);
				jsonContent = json_serialize_to_string(json_init);
			}
			else {
				LogError("failed to resolved data to JSON string");
			}
			currentContext->flag = 0;
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
    TICC2650_Create,
    TICC2650_CreateContext,
    TICC2650_Resolve,
    TICC2650_Destroy
};

RESOLVER_EXPORT const RESOLVER_API* Resolver_GetApi(RESOLVER_API_VERSION resolver_api_version)
{
    (void)resolver_api_version;
    return (const RESOLVER_API*)&Resolver_GetApi_Impl;
}
