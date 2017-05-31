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
#include "ticc2650.h"
#include "messageproperties.h"

#include <parson.h>

#define SENSOR_MASK_TEMPERATURE 0x00000001
#define SENSOR_MASK_HUMIDITY    0x00000002
#define SENSOR_MASK_MOVEMENT    0x00000004
#define SENSOR_MASK_PRESSURE    0x00000008
#define SENSOR_MASK_BRIGHTNESS  0x00000010

typedef struct TI_CC2650_RESOLVER_HANDLE_DATA_TAG
{
    BROKER_HANDLE broker;
    double lastAmbience;
    double lastObject;
    double lastHumidity;
    double lastHumidityTemperature;
    double lastPressure;
    double lastPressureTemperature;
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
} TI_CC2650_RESOLVER_HANDLE_DATA;

typedef void (*MESSAGE_RESOLVER)(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);

static void resolve_temperature(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_humidity(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_pressure(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_movement(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);
static void resolve_brightness(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer);

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
static void resolve_temperature(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	if (buffer->size == 4) {
		uint16_t* temps = (uint16_t *)buffer->buffer;
		float ambient, object;
		sensortag_temp_convert(temps[0], temps[1], &ambient, &object);

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

static void resolve_pressure(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
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
		float pressure = ((double)pres) / 100.0f;
		float temperature = ((double)temp) / 100.0f;
//		sprintf(resolveTempBuf, "\"pressure\",:%f",
//			pressure,
//		);
        handle->lastPressure = pressure;
        handle->lastPressureTemperature = temperature;
        handle->flag |= SENSOR_MASK_PRESSURE;
	}
//	return STRING_construct(resolveTempBuf);
}

static void sensortag_hum_convert(
	uint16_t rawTemp,
	uint16_t rawHum,
	float *tTemp,
	float *tHum
)
{
	*tTemp = ((double)(int16_t)rawTemp / 65536) * 165 - 40;
	*tHum = ((double)rawHum / 65536) * 100;
}

static void resolve_humidity(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	if (buffer->size == 4) {
		uint16_t* temps = (uint16_t *)buffer->buffer;
		float temp, hum;
		sensortag_hum_convert(temps[0], temps[1], &temp, &hum);
//		sprintf(resolveTempBuf, "\"humidityTemperature\":%f,\"humidity\":%f",
//			htemp,
//			humidity
//		);
        handle->lastHumidity = hum;
        handle->lastHumidityTemperature = temp;
        handle->flag |= SENSOR_MASK_HUMIDITY;
	}
//	return STRING_construct(resolveTempBuf);
}

#define ACC_RANGE_2G      0
#define ACC_RANGE_4G      1
#define ACC_RANGE_8G      2
#define ACC_RANGE_16G     3
 
int accRange = ACC_RANGE_2G;
float sensorMpu9250AccConvert(int16_t rawData)
{
  float v;
 
  switch (accRange)
  {
  case ACC_RANGE_2G:
    //-- calculate acceleration, unit G, range -2, +2
    v = (rawData * 1.0) / (32768/2);
    break;
 
  case ACC_RANGE_4G:
    //-- calculate acceleration, unit G, range -4, +4
    v = (rawData * 1.0) / (32768/4);
    break;
 
  case ACC_RANGE_8G:
    //-- calculate acceleration, unit G, range -8, +8
    v = (rawData * 1.0) / (32768/8);
    break;
 
  case ACC_RANGE_16G:
    //-- calculate acceleration, unit G, range -16, +16
    v = (rawData * 1.0) / (32768/16);
    break;
  }
 
  return v;
}

float sensorMpu9250GyroConvert(int16_t data)
{
  //-- calculate rotation, unit deg/s, range -250, +250
  return (data * 1.0) / (65536 / 500);
}

float sensorMpu9250MagConvert(int16_t data)
{
  //-- calculate magnetism, unit uT, range +-4900
  return 1.0 * (float)data;
}

static void resolve_movement(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	if (buffer->size == 18) {
		int16_t* rawdata = (int16_t*)buffer->buffer;
		float gyrox = sensorMpu9250GyroConvert(rawdata[0]);
		float gyroy = sensorMpu9250GyroConvert(rawdata[1]);
		float gyroz = sensorMpu9250GyroConvert(rawdata[2]);
		float accelx = sensorMpu9250AccConvert(rawdata[3]);
		float accely = sensorMpu9250AccConvert(rawdata[4]);
		float accelz = sensorMpu9250AccConvert(rawdata[5]);
		float magx = sensorMpu9250MagConvert(rawdata[6]);
		float magy = sensorMpu9250MagConvert(rawdata[7]);
		float magz = sensorMpu9250MagConvert(rawdata[8]);
//		sprintf(resolveTempBuf, "\"accelx:%f,\"accely\":%f,\"accelz\":%f",
//			accelx,
//			accely,
//			accelz);
        handle->lastAccelX = accelx;
        handle->lastAccelY = accely;
        handle->lastAccelZ = accelz;
        handle->lastGyroX = gyrox;
        handle->lastGyroY = gyroy;
        handle->lastGyroZ = gyroz;
        handle->lastMagX = magx;
        handle->lastMagY = magy;
        handle->lastMagZ = magz;
        handle->flag |= SENSOR_MASK_MOVEMENT;
	}
//	return STRING_construct(resolveTempBuf);
}

static void resolve_brightness(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)name;
	if (buffer->size == 2)
	{
		uint16_t* temps = (uint16_t *)buffer->buffer;
		uint16_t e, m;
		m = temps[0] & 0xFFF;
		e = (temps[0] & 0xF000) >> 12;

		float brightness = ((double)m)*(0.01*pow(2.0, e));
//		sprintf(resolveTempBuf, "\"brightness\":%f",
//			brightness
//		);
        handle->lastBrightness = brightness;
        handle->flag |= SENSOR_MASK_BRIGHTNESS;
	}
}

static void resolve_default(TI_CC2650_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)handle;
    (void)name;
    (void)buffer;
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



MODULE_HANDLE TI_CC2650_Resolver_Create(BROKER_HANDLE broker, const void* configuration)
{
    TI_CC2650_RESOLVER_HANDLE_DATA* result;
        if(broker == NULL)
    {
        LogError("NULL parameter detected broker=%p", broker);
        result = NULL;
    }
    else
    {
        result = (TI_CC2650_RESOLVER_HANDLE_DATA*)malloc(sizeof(TI_CC2650_RESOLVER_HANDLE_DATA));
        if (result == NULL)
        {
            /*Codes_SRS_BLE_CTOD_13_002: [ TI_CC2650_RESOLVER_Create shall return NULL if any of the underlying platform calls fail. ]*/
            LogError("malloc failed");
        }
        else
        {
            result->broker = broker;
            result->flag = 0x00000000;
      //      result->availables = SENSOR_MASK_TEMPERATURE | SENSOR_MASK_HUMIDITY | SENSOR_MASK_PRESSURE | SENSOR_MASK_MOVEMENT | SENSOR_MASK_BRIGHTNESS;
            if (configuration!=NULL){
                TICC2650_CONFIG* config = (TICC2650_CONFIG*)configuration;
                result->availables = config->availables;
            }
        }
    }
    
    /*Codes_SRS_BLE_CTOD_13_023: [ TI_CC2650_RESOLVER_Create shall return a non-NULL handle when the function succeeds. ]*/

    return (MODULE_HANDLE)result;
}

/*
     "args": {
        "sensor-types":[
          {"sensor-type":"temperature"},
          {"sensor-type":"movement", "config":"4G"} <- config is optional
        ]
      }
 return TICC2650_Config
*/
void* TI_CC2650_Resolver_ParseConfigurationFromJson(const char* configuration)
{
    TICC2650_CONFIG* config = NULL;
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
                config = (TICC2650_CONFIG*)malloc(sizeof(TICC2650_CONFIG));
                config->availables = 0;
                config->accRange = 0;
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
                            } else if (g_ascii_strcasecmp(typeName, SENSOR_TYPE_MOVEMENT)==0){
                                config->availables |= SENSOR_MASK_MOVEMENT;
                                const char* movementConfig = json_object_get_string(sensorType,SENSOR_CONFIG_JSON);
                                if (movementConfig!=NULL){
                                    // set accRange
                                }
                            }else if (g_ascii_strcasecmp(typeName, SENSOR_TPYE_BRIGHTNESS)==0){
                                config->availables |= SENSOR_MASK_BRIGHTNESS;
                            }
                            else {
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

void TI_CC2650_Resolver_FreeConfiguration(void * configuration)
{
    if (configuration!=NULL){
        free(configuration);
    }
    return;
}

#define MSG_TMP_BUF_SIZE 1024

void TI_CC2650_Resolver_Receive(MODULE_HANDLE module, MESSAGE_HANDLE message)
{
    TI_CC2650_RESOLVER_HANDLE_DATA* handle;
    if (module == NULL){
        LogError("module is NULL");
    }
    else{
        handle = (TI_CC2650_RESOLVER_HANDLE_DATA*)module;
        if (message != NULL)
        {
            CONSTMAP_HANDLE props = Message_GetProperties(message);
            if (props != NULL)
            {
                const char* source = ConstMap_GetValue(props, GW_SOURCE_PROPERTY);
                if (source != NULL && strcmp(source, GW_SOURCE_BLE_TELEMETRY) == 0)
                {
               //     const char* ble_controller_id = ConstMap_GetValue(props, GW_BLE_CONTROLLER_INDEX_PROPERTY);
             //       const char* mac_address_str = ConstMap_GetValue(props, GW_MAC_ADDRESS_PROPERTY);
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
                        if((handle->flag & handle->availables)==handle->availables)
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
                                json_status = json_object_set_number(json, "pressureTemperature", handle->lastPressureTemperature);
                            }
                            if ((handle->flag & SENSOR_MASK_MOVEMENT)!=0){
                                json_status = json_object_set_number(json, "accelx", handle->lastAccelX);
                                json_status = json_object_set_number(json, "accely", handle->lastAccelY);
                                json_status = json_object_set_number(json, "accelz", handle->lastAccelZ);
                                json_status = json_object_set_number(json, "gyrox", handle->lastGyroX);
                                json_status = json_object_set_number(json, "gyroy", handle->lastGyroY);
                                json_status = json_object_set_number(json, "gyroz", handle->lastGyroZ);
                                json_status = json_object_set_number(json, "magx", handle->lastMagX);
                                json_status = json_object_set_number(json, "magy", handle->lastMagY);
                                json_status = json_object_set_number(json, "magz", handle->lastMagZ);
                            }
                            if ((handle->flag & SENSOR_MASK_BRIGHTNESS)!=0){
                                json_status = json_object_set_number(json, "brightness", handle->lastBrightness);
                            }

                            char* jsonContent = json_serialize_to_string_pretty(json_init);
                            // I don't know how to remove /'s prefix.
                            char* tmpBuf = (char*)malloc(strlen(jsonContent));
                            int tmpi=0;
                            int tmpj=0;
                            while (jsonContent[tmpi]!='\0'){
                                if (jsonContent[tmpi]==0x5c&&jsonContent[tmpi+1]==0x2f){
                                    tmpBuf[tmpj] = jsonContent[tmpi+1];
                                    tmpi++;
                                }
                                else {
                                    tmpBuf[tmpj] = jsonContent[tmpi];
                                }
                                tmpi++; tmpj++;
                            }
                            tmpBuf[tmpj] = '\0';

                            CONSTBUFFER_HANDLE msgContent = CONSTBUFFER_Create((const unsigned char*)tmpBuf, strlen(tmpBuf));
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
                            free(tmpBuf);

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

void TI_CC2650_Resolver_Destroy(MODULE_HANDLE module)
{
    (void)module;
    // Nothing to do here
}

static const MODULE_API_1 Module_GetApi_Impl =
{
    {MODULE_API_VERSION_1},

    TI_CC2650_Resolver_ParseConfigurationFromJson,
    TI_CC2650_Resolver_FreeConfiguration,
    TI_CC2650_Resolver_Create,
    TI_CC2650_Resolver_Destroy,
    TI_CC2650_Resolver_Receive,
    NULL
};

MODULE_EXPORT const MODULE_API* Module_GetApi(MODULE_API_VERSION gateway_api_version)
{
    (void)gateway_api_version;
    return (const MODULE_API*)&Module_GetApi_Impl;
}

