// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>


#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include "dynamic_library.h"

#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/constmap.h"
#include "azure_c_shared_utility/constbuffer.h"

#include "module.h"
#include "message.h"
#include "broker.h"
#include "module_loader.h"

#include "filter.h"
#include "filter_api.h"
#include "messageproperties.h"


#include <parson.h>

typedef struct FILTER_RESOLVER_LIST_TAG
{
    void* resolverHandle;
	const char* name;
    const RESOLVER_API* resolverAPI;
	DYNAMIC_LIBRARY_HANDLE dlHandle;
	void* next;
} FILTER_RESOLVER_LIST;

typedef struct FILTER_RESOLVER_CONTEXT_TAG
{
    void* resolverHandle;
    void* resolverContext;
    const char* macAddress;
} FILTER_RESOLVER_CONTEXT;

typedef struct FILTER_RESOLVER_CONTEXT_LIST_TAG
{
    FILTER_RESOLVER_CONTEXT resolver;
    void* next;
} FILTER_RESOLVER_CONTEXT_LIST;

typedef struct FILTER_RESOLVER_DATA_TAG
{
    BROKER_HANDLE broker;
    FILTER_RESOLVER_CONTEXT_LIST* resolverContexts;
    FILTER_RESOLVER_LIST* resolvers;
} FILTER_RESOLVER_DATA;

typedef struct FILTER_RESOLVER_AVAILABLES_CONFIG_TAG
{
    const char* macAddress;
    JSON_Object* configration;
} FILTER_RESOLVER_AVAILABLES_CONFIG;

typedef struct FILTER_RESOLVER_AVAILABLES_CONFIG_LIST_TAG
{
    FILTER_RESOLVER_AVAILABLES_CONFIG resolverMaskConfig;
    void* next;
} FILTER_RESOLVER_AVAILABLES_CONFIG_LIST;

typedef struct FILTER_RESOLVER_CONFIG_TAG
{
	const char* filterName;
	const char* loaderName;
	const char* modulePath;
	FILTER_RESOLVER_AVAILABLES_CONFIG_LIST* resolverMaskConfigs;
} FILTER_RESOLVER_CONFIG;

typedef struct FILTER_RESOLVER_CONFIG_LIST_TAG
{
	FILTER_RESOLVER_CONFIG resolverConfig;
	void* next;
} FILTER_RESOLVER_CONFIG_LIST;


typedef struct FILTER_CONFIG_TAG
{
    FILTER_RESOLVER_CONFIG_LIST* resolverConfigs;
	JSON_Value* configJson;
} FILTER_CONFIG;

// typedef void (*MESSAGE_RESOLVER)(FILTER_HANDLE_DATA* handle, const char* name, const char* buffer);

/*
typedef struct DIPATCH_ENTRY_TAG
{
    const char* name;
    const char* characteristic_uuid;
    MESSAGE_RESOLVER message_resolver;
}DIPATCH_ENTRY;

static void resolve_default(FILTER_RESOLVER_HANDLE_DATA* handle, const char* name, const CONSTBUFFER* buffer)
{
    (void)handle;
    
	STRING_HANDLE result = STRING_construct("\"");
	STRING_concat(result, name);
	STRING_concat(result, "\":\"");
	STRING_HANDLE rawData = getRawData(buffer->buffer, buffer->size);
	sprintf(resolveTempBuf, "%s", STRING_c_str(rawData));
	STRING_concat(result, resolveTempBuf);
	STRING_concat(result, "\"");
	return result;
}
*/

const RESOLVER_API* GetResolver(FILTER_RESOLVER_CONFIG* resolverConfig, FILTER_RESOLVER_LIST* resolver)
{
	//(void)loaderName;  

    DYNAMIC_LIBRARY_HANDLE libHandle = DynamicLibrary_LoadLibrary(resolverConfig->modulePath);
    if (libHandle==NULL)
    {
        LogError("DynamicLibrary_LoadLibrary() returned NULL for resolver %s", resolverConfig->modulePath);
    }
    else
    {
		resolver->dlHandle = libHandle;
        rResolver_GetApi resolverApi = (rResolver_GetApi)DynamicLibrary_FindSymbol(libHandle, RESOLVER_GETAPI_NAME);
        if (resolverApi==NULL)
        {
            LogError("DynamicLibrary_FindSymbol() returned NULL - resolver");
            DynamicLibrary_UnloadLibrary(libHandle);
        }
        else
		{
            return resolverApi(RESOLVER_API_VERSION_1);
        }
    }
    return NULL;
}

MODULE_HANDLE FILTER_Create(BROKER_HANDLE broker, const void* configuration)
{
    FILTER_RESOLVER_DATA* result = NULL;
    if(broker == NULL)
    {
        LogError("NULL parameter detected broker=%p", broker);
        result = NULL;
    }
	else
	{
		result = (FILTER_RESOLVER_DATA*)malloc(sizeof(FILTER_RESOLVER_DATA));
		result->broker = broker;
		result->resolvers = NULL;
		result->resolverContexts = NULL;

		FILTER_CONFIG* config = (FILTER_CONFIG*)configuration;

		FILTER_RESOLVER_CONFIG_LIST* rcs = config->resolverConfigs;
		while (rcs != NULL)
		{
			FILTER_RESOLVER_LIST* resolvers = (FILTER_RESOLVER_LIST*)malloc(sizeof(FILTER_RESOLVER_LIST));
			resolvers->next = NULL;
			if (result->resolvers == NULL)
			{
				result->resolvers = resolvers;
			}
			else
			{
				FILTER_RESOLVER_LIST* tmpRs = result->resolvers;
				while (tmpRs->next != NULL)
				{
					tmpRs = tmpRs->next;
				}
				tmpRs->next = resolvers;
			}
			resolvers->resolverAPI = GetResolver(&rcs->resolverConfig, resolvers);
			resolvers->resolverHandle = RESOLVER_CREATE(resolvers->resolverAPI)(result, rcs->resolverConfig.filterName);
			char* tmp = (char*)malloc(strlen(rcs->resolverConfig.filterName) + 1);
			strcpy(tmp, rcs->resolverConfig.filterName);
			resolvers->name = tmp;
			FILTER_RESOLVER_AVAILABLES_CONFIG_LIST* racs = rcs->resolverConfig.resolverMaskConfigs;
			while (racs != NULL)
			{
				FILTER_RESOLVER_CONTEXT_LIST* contexts = (FILTER_RESOLVER_CONTEXT_LIST*)malloc(sizeof(FILTER_RESOLVER_CONTEXT_LIST));
				contexts->next = NULL;
				if (result->resolverContexts == NULL)
				{
					result->resolverContexts = contexts;
				}
				else
				{
					FILTER_RESOLVER_CONTEXT_LIST* tmpCs = result->resolverContexts;
					while (tmpCs->next != NULL)
					{
						tmpCs = tmpCs->next;
					}
					tmpCs->next = contexts;
				}
				tmp = (char*)malloc(strlen(racs->resolverMaskConfig.macAddress) + 1);
				strcpy(tmp, racs->resolverMaskConfig.macAddress);
				contexts->resolver.macAddress = tmp;

				contexts->resolver.resolverHandle = resolvers;
				contexts->resolver.resolverContext = RESOLVER_CREATE_CONTEXT(resolvers->resolverAPI)(resolvers->resolverHandle, racs->resolverMaskConfig.configration);
				racs = racs->next;
			}
			rcs = rcs->next;
		}


	}
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
void* FILTER_ParseConfigurationFromJson(const char* configuration)
{
    FILTER_CONFIG* config = NULL;
    if (configuration!=NULL){
        JSON_Value* json = json_parse_string((const char*)configuration);
        if (json == NULL)
        {
            // error
            LogError("Failed to parse json configuration");
        }
        else {
            config = (FILTER_CONFIG*)malloc(sizeof(FILTER_CONFIG));
            config->resolverConfigs = NULL;
			config->configJson = json;
            JSON_Array* filters = json_value_get_array(json);
            if (filters==NULL || config==NULL){
                // error
                LogError("Failed to parse json configuration");
            }
            else {
                size_t tcount = json_array_get_count(filters);
                for(size_t t=0;t<tcount;t++)
                {
                    JSON_Object* filter = json_array_get_object(filters, t);
                    const char* filterName = json_object_get_string(filter,"filter-name");
					if (filterName == NULL)
					{
						LogError("Can't get filter-name");
						free(config);
					}
					else
					{
						JSON_Object* filterLoader = json_object_get_object(filter, "loader");
						if (filterLoader == NULL)
						{
							LogError("Can't get filter loader");
							free(config);
						}
						else
						{
							const char* loaderName = json_object_get_string(filterLoader, "name");
							JSON_Object* entrypoint = json_object_get_object(filterLoader, "entrypoint");
							if (entrypoint == NULL)
							{
								LogError("Can't get filter's loader entrypoint");
								free(config);
							}
							else
							{
								const char* modulePath = json_object_get_string(entrypoint, "module.path");
								if (modulePath == NULL)
								{
									LogError("Can't get filter's loader entorypoint module.path");
									free(config);
								}
								else
								{
									FILTER_RESOLVER_CONFIG_LIST* resolverConfigs = (FILTER_RESOLVER_CONFIG_LIST*)malloc(sizeof(FILTER_RESOLVER_CONFIG_LIST));
									resolverConfigs->next = NULL;
									resolverConfigs->resolverConfig.resolverMaskConfigs = NULL;
									if (config->resolverConfigs == NULL)
									{
										config->resolverConfigs = resolverConfigs;
									}
									else {
										FILTER_RESOLVER_CONFIG_LIST* tmpResolverConfigs = config->resolverConfigs;
										while (resolverConfigs->next != NULL)
										{
											tmpResolverConfigs = tmpResolverConfigs->next;
										}
										tmpResolverConfigs->next = resolverConfigs;
									}
									char* tmp = (char*)malloc(strlen(filterName) + 1);
									strcpy(tmp, filterName);
									resolverConfigs->resolverConfig.filterName = tmp;
									tmp = (char *)malloc(strlen(loaderName) + 1);
									strcpy(tmp, loaderName);
									resolverConfigs->resolverConfig.loaderName = tmp;
									tmp = (char*)malloc(strlen(modulePath) + 1);
									strcpy(tmp, modulePath);
									resolverConfigs->resolverConfig.modulePath = tmp;

									JSON_Array* availables = json_object_get_array(filter, "availables");
									size_t acount = json_array_get_count(availables);
									for (size_t a = 0; a < acount; a++)
									{
										JSON_Object* available = json_array_get_object(availables, a);
										const char* sensorTag = json_object_get_string(available, "sensor-tag");

										FILTER_RESOLVER_AVAILABLES_CONFIG_LIST* availableConfigs = (FILTER_RESOLVER_AVAILABLES_CONFIG_LIST*)malloc(sizeof(FILTER_RESOLVER_AVAILABLES_CONFIG_LIST));
										availableConfigs->next = NULL;

										if (resolverConfigs->resolverConfig.resolverMaskConfigs == NULL)
										{
											resolverConfigs->resolverConfig.resolverMaskConfigs = availableConfigs;
										}
										else
										{
											FILTER_RESOLVER_AVAILABLES_CONFIG_LIST* tmpAvailableConfigs = resolverConfigs->resolverConfig.resolverMaskConfigs;
											while (tmpAvailableConfigs->next != NULL)
											{
												tmpAvailableConfigs = tmpAvailableConfigs->next;
											}
											tmpAvailableConfigs->next = availableConfigs;
										}
										tmp = (char*)malloc(strlen(sensorTag) + 1);
										strcpy(tmp, sensorTag);
										availableConfigs->resolverMaskConfig.macAddress = tmp;

										availableConfigs->resolverMaskConfig.configration = available;
									}
								}
							}
						}
					}
                }
            }
        }
    }
    return config;
}

void FILTER_FreeConfiguration(void * configuration)
{
    if (configuration!=NULL){
		FILTER_CONFIG* config = (FILTER_CONFIG*)configuration;
		FILTER_RESOLVER_CONFIG_LIST* rcs = config->resolverConfigs;
		while (rcs != NULL)
		{
			free((void*)rcs->resolverConfig.filterName);
			free((void*)rcs->resolverConfig.loaderName);
			free((void*)rcs->resolverConfig.modulePath);
			FILTER_RESOLVER_AVAILABLES_CONFIG_LIST* racs = rcs->resolverConfig.resolverMaskConfigs;

			while (racs != NULL)
			{
				free((void*)racs->resolverMaskConfig.macAddress);
				FILTER_RESOLVER_AVAILABLES_CONFIG_LIST* tmpRACs = racs->next;
				free(racs);
				racs = tmpRACs;
			}

			FILTER_RESOLVER_CONFIG_LIST* tmpRCs = rcs->next;
			free(rcs);
			rcs = tmpRCs;
		}
		json_value_free(config->configJson);
		free(configuration);
    }
    return;
}

#define MSG_TMP_BUF_SIZE 1024

void FILTER_Receive(MODULE_HANDLE module, MESSAGE_HANDLE message)
{
	FILTER_RESOLVER_DATA* handle;
	if (module == NULL) {
		LogError("module is NULL");
	}
	else {
		handle = (FILTER_RESOLVER_DATA*)module;
		if (message != NULL)
		{
			CONSTMAP_HANDLE props = Message_GetProperties(message);
			if (props != NULL)
			{
				const char* source = ConstMap_GetValue(props, GW_SOURCE_PROPERTY);
				if (source != NULL && strcmp(source, GW_SOURCE_BLE_TELEMETRY) == 0)
				{
					handle = (FILTER_RESOLVER_DATA*)module;
					//     const char* ble_controller_id = ConstMap_GetValue(props, GW_BLE_CONTROLLER_INDEX_PROPERTY);
					const char* mac_address_str = ConstMap_GetValue(props, GW_MAC_ADDRESS_PROPERTY);
					const char* timestamp = ConstMap_GetValue(props, GW_TIMESTAMP_PROPERTY);
					const char* characteristic_uuid = ConstMap_GetValue(props, GW_CHARACTERISTIC_UUID_PROPERTY);
					const CONSTBUFFER* buffer = Message_GetContent(message);
					if (buffer != NULL && characteristic_uuid != NULL)
					{
						FILTER_RESOLVER_CONTEXT* targetResolverContext = NULL;
						FILTER_RESOLVER_CONTEXT_LIST* resolverContext = handle->resolverContexts;
						while (resolverContext != NULL)
						{
							if (strcmp(resolverContext->resolver.macAddress, mac_address_str) == 0)
							{
								targetResolverContext = &resolverContext->resolver;
								break;
							}
							resolverContext = resolverContext->next;
						}
						if (targetResolverContext != NULL)
						{
							FILTER_RESOLVER_LIST* targetResolver = NULL;
							FILTER_RESOLVER_LIST* resolvers = handle->resolvers;
							while (resolvers != NULL)
							{
								if (resolvers == targetResolverContext->resolverHandle)
								{
									targetResolver = resolvers;
									break;
								}
								resolvers = resolvers->next;
							}
							if (targetResolver != NULL)
							{
								CONSTBUFFER_HANDLE resolved = RESOLVER_RESOLVE(targetResolver->resolverAPI)(targetResolverContext->resolverContext, characteristic_uuid,timestamp, buffer);
								if (resolved != NULL)
								{
									MAP_HANDLE msgProps = ConstMap_CloneWriteable(props);
									MESSAGE_BUFFER_CONFIG msgBufConfig = {
										resolved,
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
										Message_Destroy(newMessage);
									}
								}
							}
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

void FILTER_Destroy(MODULE_HANDLE module)
{
	FILTER_RESOLVER_DATA* handle = (FILTER_RESOLVER_DATA*)module;
	FILTER_RESOLVER_CONTEXT_LIST* contexts = handle->resolverContexts;
	while (contexts != NULL)
	{
		FILTER_RESOLVER_CONTEXT_LIST* tmpCs = contexts->next;
		free((void*)contexts->resolver.macAddress);
		// contexts->resolver.resolverContext will be free in Resolver Library;
		free(contexts);
		contexts = tmpCs;
	}
	FILTER_RESOLVER_LIST* resolvers = handle->resolvers;
	while (resolvers != NULL)
	{
		FILTER_RESOLVER_LIST* tmpRs = resolvers->next;
		free((void*)resolvers->name);
		RESOLVER_DESTROY(resolvers->resolverAPI)(resolvers->resolverHandle);
		DynamicLibrary_UnloadLibrary(resolvers->dlHandle);
		free((void*)resolvers->resolverHandle);
		// unload resolvers->resolverApi
		free(resolvers);
		resolvers = tmpRs;
	}
}

static const MODULE_API_1 Module_GetApi_Impl =
{
    {MODULE_API_VERSION_1},

    FILTER_ParseConfigurationFromJson,
    FILTER_FreeConfiguration,
    FILTER_Create,
    FILTER_Destroy,
    FILTER_Receive,
    NULL
};

MODULE_EXPORT const MODULE_API* Module_GetApi(MODULE_API_VERSION gateway_api_version)
{
    (void)gateway_api_version;
    return (const MODULE_API*)&Module_GetApi_Impl;
}

