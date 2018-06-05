// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/macro_utils.h"
#include "azure_c_shared_utility/httpapiex.h"
#include "azure_uamqp_c/amqp_definitions.h"
#include "gateway.h"
#include "parson.h"
#include "experimental/event_system.h"

#include "module_loaders/dynamic_loader.h"
#include "gateway_internal.h"

#define GATEWAY_KEY "gateway"
#define MODULES_KEY "modules"
#define LOADERS_KEY "loaders"
#define LOADER_KEY "loader"
#define MODULE_NAME_KEY "name"
#define LOADER_NAME_KEY "name"
#define LOADER_ENTRYPOINT_KEY "entrypoint"
#define MODULE_PATH_KEY "module.path"
#define ARG_KEY "args"

#define GATEWAY_KEY "gateway"
#define GATEWAY_IOTHUB_CONNECTION_STRING_KEY "connection-string"
#define GATEWAY_IOTHUB_TRANSPORT_KEY "transport"
#define GATEWAY_IOTHUB_MODULES_LOCAL_PATH "modules-local-path"
#define MODULE_REMOTE_URL "module.uri"

#define LINKS_KEY "links"
#define SOURCE_KEY "source"
#define SINK_KEY "sink"
#define LINK_MSGTYPE_KEY "message.type"

#define PARSE_JSON_RESULT_VALUES \
    PARSE_JSON_SUCCESS, \
    PARSE_JSON_FAILURE, \
    PARSE_JSON_MISSING_OR_MISCONFIGURED_CONFIG, \
    PARSE_JSON_VECTOR_FAILURE, \
    PARSE_JSON_MISCONFIGURED_OR_OTHER

DEFINE_ENUM(PARSE_JSON_RESULT, PARSE_JSON_RESULT_VALUES);

GATEWAY_HANDLE gateway_create_internal(const GATEWAY_PROPERTIES* properties, bool use_json);
static PARSE_JSON_RESULT parse_json_internal(GATEWAY_PROPERTIES* out_properties, JSON_Value *root);
static void destroy_properties_internal(GATEWAY_PROPERTIES* properties);
void gateway_destroy_internal(GATEWAY_HANDLE gw);
unsigned char* gateway_get_current_module_version(JSON_Object* json, const unsigned char* moduleName);

void gateway_deviceTwinCallback(DEVICE_TWIN_UPDATE_STATE update_state, const unsigned char* payLoad, size_t size, void* userContextCallback)
{
	GATEWAY_HANDLE gw = (GATEWAY_HANDLE)userContextCallback;
    unsigned char* buf = (unsigned char*)malloc(size);
    memcpy(buf, payLoad, size);
    if (buf[size - 1] != '}') {
        int index = size;
        while (index > 0) {
            if (buf[--index] == '}') {
                buf[index + 1] = '\0';
                break;
            }
        }
//        buf[size - 1] = '\0';
    }
    LogInfo("Device Twin Desired Properteis Updated - '%s'", payLoad);

    JSON_Value* root = json_parse_string(buf);
    JSON_Object* document = json_value_get_object(root);
    JSON_Object* desiredProperties = json_object_get_object(document, "desired");
    if (desiredProperties != NULL) {
        JSON_Object* gwConfig = json_object_get_object(desiredProperties, GATEWAY_KEY);
        if (gwConfig != NULL) {
            const char* configValue = json_object_get_string(gwConfig, "configuration");
            if (configValue != NULL) {
                char* configValueHost = NULL;
                const char* configValueRelPath = NULL;
                int cvIndex = 0;
                int cvLen = strlen(configValue);
                const char cvProtocol[9] = { 'h','t','t','p','s',':','/','/','\0' };
                int cvPLen = strlen(cvProtocol);
                while (cvIndex < cvPLen) {
                    if (cvProtocol[cvIndex] != configValue[cvIndex]) {
                        // error
                    }
                    cvIndex++;
                }
                if (cvIndex == cvPLen) {
                    configValueHost = &configValue[cvIndex];
                }
                while (cvIndex < cvLen) {
                    if (configValue[cvIndex++] == '/') {
                        break;
                    }
                }
                if (0 < cvIndex&& cvIndex < cvLen) {
                    int hostLen = cvIndex - cvPLen;
                    configValueHost = (char*)malloc(hostLen);
                    memcpy(configValueHost, &configValue[cvPLen], hostLen - 1);
                    configValueHost[hostLen - 1] = '\0';
                    configValueRelPath = &configValue[cvIndex-1];
                }
                if (configValueHost == NULL || configValueRelPath == NULL) {
                    LogError("Bad url!");
                }
                else {
                    HTTP_HEADERS_HANDLE httpReqHeadersHandle, httpResHeadersHandle;
                    httpReqHeadersHandle = HTTPHeaders_Alloc();
                    httpResHeadersHandle = HTTPHeaders_Alloc();
                    if (httpReqHeadersHandle == NULL || httpResHeadersHandle == NULL) {
                        LogError("Failed to allocate http headers");
                        if (httpReqHeadersHandle != NULL) HTTPHeaders_Free(httpReqHeadersHandle);
                        if (httpResHeadersHandle != NULL) HTTPHeaders_Free(httpResHeadersHandle);
                    }
                    else {
                        BUFFER_HANDLE reqContent = BUFFER_new();
                        BUFFER_HANDLE resContent = BUFFER_new();
                        if (reqContent == NULL || resContent == NULL) {
                            LogError("Failed to allocate http contents");
                            if (reqContent != NULL) BUFFER_delete(reqContent);
                            if (resContent != NULL) BUFFER_delete(resContent);
                            HTTPHeaders_Free(httpReqHeadersHandle);
                            HTTPHeaders_Free(httpResHeadersHandle);
                        }
                        else {
                            unsigned int statusCode;
                            HTTPAPIEX_HANDLE httpapiexHandle = HTTPAPIEX_Create(configValueHost);
                            if (httpapiexHandle == NULL) {
                                LogError("Failed to create httpapiex handle");
                                HTTPHeaders_Free(httpReqHeadersHandle);
                                HTTPHeaders_Free(httpResHeadersHandle);
                                BUFFER_delete(reqContent);
                                BUFFER_delete(resContent);
                            }
                            else {
                                HTTPAPIEX_RESULT httpApiExResult = HTTPAPIEX_ExecuteRequest(httpapiexHandle, HTTPAPI_REQUEST_GET, configValueRelPath, httpReqHeadersHandle, reqContent, &statusCode, httpResHeadersHandle, resContent);

                                if (httpApiExResult == HTTPAPIEX_OK) {
                                    if (statusCode == 200) {
                                        const char* rc = BUFFER_u_char(resContent);
                                        int rcLen = BUFFER_length(resContent);
                                        STRING_HANDLE sh = STRING_construct_n(rc, rcLen);
                                        const char* configJson = STRING_c_str(sh);
                                        LogInfo("Received configuration - \n >>>%s<<<\n", configJson);
                                        Gateway_UpdateFromJson(gw, rc);
                                    }
                                    else {
                                        LogInfo("http access result %d", statusCode);
                                    }
                                }
                                else {
                                    LogError("http access failed %d", httpApiExResult);
                                }
                                HTTPAPIEX_Destroy(httpapiexHandle);
                                HTTPHeaders_Free(httpReqHeadersHandle);
                                HTTPHeaders_Free(httpResHeadersHandle);
                                BUFFER_delete(reqContent);
                                BUFFER_delete(resContent);
                            }
                        }
                    }
                }
                if (configValueHost != NULL) {
                    free(configValueHost);
                }
            }
        }
    }
    free(buf);
}

static int strcmp_i(const char* lhs, const char* rhs)
{
	char lc, rc;
	int cmp;

	do
	{
		lc = *lhs++;
		rc = *rhs++;
		cmp = tolower(lc) - tolower(rc);
	} while (cmp == 0 && lc != 0 && rc != 0);

	return cmp;
}

GATEWAY_HANDLE Gateway_CreateFromJson(const char* file_path)
{
    GATEWAY_HANDLE gw;

    if (file_path != NULL)
    {
        /*Codes_SRS_GATEWAY_JSON_17_005: [ The function shall initialize the default module loader list. ]*/
        if (ModuleLoader_Initialize() != MODULE_LOADER_SUCCESS)
        {
            /*Codes_SRS_GATEWAY_JSON_17_012: [ This function shall return NULL if the module list is not initialized. ]*/
            LogError("ModuleLoader_Initialize failed");
            gw = NULL;
        }
        else
        {
            JSON_Value *root_value;

            /*Codes_SRS_GATEWAY_JSON_14_002: [The function shall use parson to read the file and parse the JSON string to a parson JSON_Value structure.]*/
            root_value = json_parse_file(file_path);
            if (root_value != NULL)
            {
                /*Codes_SRS_GATEWAY_JSON_14_004: [The function shall traverse the JSON_Value object to initialize a GATEWAY_PROPERTIES instance.]*/
                GATEWAY_PROPERTIES *properties = (GATEWAY_PROPERTIES*)malloc(sizeof(GATEWAY_PROPERTIES));

                if (properties != NULL)
                {
                    properties->gateway_modules = NULL;
                    properties->gateway_links = NULL;
                    properties->deployConfig = NULL;
					if ((parse_json_internal(properties, root_value) == PARSE_JSON_SUCCESS) && properties->gateway_modules != NULL && properties->gateway_links != NULL)
                    {
                        /*Codes_SRS_GATEWAY_JSON_14_007: [The function shall use the GATEWAY_PROPERTIES instance to create and return a GATEWAY_HANDLE using the lower level API.]*/
                        /*Codes_SRS_GATEWAY_JSON_17_004: [ The function shall set the module loader to the default dynamically linked library module loader. ]*/
                        gw = gateway_create_internal(properties, true);

                        if (gw == NULL)
                        {
                            LogError("Failed to create gateway using lower level library.");
                        }
                        else
                        {
                            /*Codes_SRS_GATEWAY_JSON_17_001: [ Upon successful creation, this function shall start the gateway. ]*/
                            GATEWAY_START_RESULT start_result;
                            start_result = Gateway_Start(gw);
                            if (start_result != GATEWAY_START_SUCCESS)
                            {
                                /*Codes_SRS_GATEWAY_JSON_17_002: [ This function shall return NULL if starting the gateway fails. ]*/
                                LogError("failed to start gateway");
                                gateway_destroy_internal(gw);
                                gw = NULL;
                            }
							else {
								JSON_Object *json_document = json_value_get_object(root_value);
								JSON_Object *gwConfig = json_object_get_object(json_document, GATEWAY_KEY);
								IOTHUB_CLIENT_TRANSPORT_PROVIDER transportProvider = NULL;
								if (gwConfig != NULL) {
									const char* connectionString = json_object_get_string(gwConfig, GATEWAY_IOTHUB_CONNECTION_STRING_KEY);
									const char* iothubTransport = json_object_get_string(gwConfig, GATEWAY_IOTHUB_TRANSPORT_KEY);
                                    const char* modulesLocalPath = json_object_get_string(gwConfig, GATEWAY_IOTHUB_MODULES_LOCAL_PATH);
									if (connectionString != NULL && iothubTransport != NULL && modulesLocalPath!=NULL) {
										if (strcmp_i(iothubTransport, "AMQP") == 0)
										{
											transportProvider = AMQP_Protocol;
										}
										else if (strcmp_i(iothubTransport, "MQTT") == 0)
										{
											transportProvider = MQTT_Protocol;
										}
										if (transportProvider != NULL) {
											gw->iothub_client = IoTHubClient_CreateFromConnectionString(connectionString, transportProvider);
											if (gw->iothub_client != NULL) {
												IoTHubClient_SetDeviceTwinCallback(gw->iothub_client, gateway_deviceTwinCallback, gw);
											}
										}
                                        gw->modules_local_path = STRING_construct(modulesLocalPath); // should be free
									}
								}
							}
                        }
                    }
                    /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
                    else
                    {
                        gw = NULL;
                        LogError("Failed to create properties structure from JSON configuration.");
                    }
                    destroy_properties_internal(properties);
                    free(properties);
                }
                /*Codes_SRS_GATEWAY_JSON_14_008: [This function shall return NULL upon any memory allocation failure.]*/
                else
                {
                    gw = NULL;
                    LogError("Failed to allocate GATEWAY_PROPERTIES.");
                }

                json_value_free(root_value);
            }
            else
            {
                /*Codes_SRS_GATEWAY_JSON_14_003: [The function shall return NULL if the file contents could not be read and / or parsed to a JSON_Value.]*/
                gw = NULL;
                LogError("Input file [%s] could not be read.", file_path);
            }
            if (gw == NULL)
            {
                /*Codes_SRS_GATEWAY_JSON_17_006: [ Upon failure this function shall destroy the module loader list. ]*/
                ModuleLoader_Destroy();
            }
        }
    }    /*Codes_SRS_GATEWAY_JSON_14_001: [If file_path is NULL the function shall return NULL.]*/
    else
    {
        gw = NULL;
        LogError("Input file path is NULL.");
    }

    return gw;
}

static void rollbackModules(GATEWAY_HANDLE gw, VECTOR_HANDLE modules_added_successfully)
{
    size_t success_modules_entries_count = VECTOR_size(modules_added_successfully);
    if (success_modules_entries_count > 0)
    {
        for (size_t properties_index = 0; properties_index < success_modules_entries_count; ++properties_index)
        {
            GATEWAY_MODULES_ENTRY* entry = (GATEWAY_MODULES_ENTRY*)VECTOR_element(modules_added_successfully, properties_index);
            if (Gateway_RemoveModuleByName(gw, entry->module_name) != 0)
            {
                LogError("Failed to remove module %s up failure.", entry->module_name);
            }
        }
    }
}

GATEWAY_UPDATE_FROM_JSON_RESULT Gateway_UpdateFromJson(GATEWAY_HANDLE gw, const char* json_content)
{
    GATEWAY_UPDATE_FROM_JSON_RESULT result;
    /* Codes_SRS_GATEWAY_JSON_04_004: [ If gw is NULL the function shall return GATEWAY_UPDATE_FROM_JSON_ERROR. ] */
    if (gw == NULL)
    {
        LogError("Gw handle can't be NULL.");
        result = GATEWAY_UPDATE_FROM_JSON_INVALID_ARG;
    }
    /* Codes_SRS_GATEWAY_JSON_04_003: [ If json_content is NULL the function shall return GATEWAY_UPDATE_FROM_JSON_ERROR. ] */
    else if (json_content == NULL)
    {
        LogError("json_content can't be NULL.");
        result = GATEWAY_UPDATE_FROM_JSON_INVALID_ARG;
    }
    else
    {
        bool isUpdating = true;
        while (isUpdating) {
            Lock(gw->update_lock);
            if (gw->runtime_status != GATEWAY_RUNTIME_STATUS_UPDATING) {
                isUpdating = false;
                gw->runtime_status = GATEWAY_RUNTIME_STATUS_UPDATING;
            }
            Unlock(gw->update_lock);
        }
        /* Codes_SRS_GATEWAY_JSON_04_005: [ The function shall use parson to parse the JSON string to a parson JSON_Value structure. ] */
        JSON_Value *root_value = json_parse_string(json_content);
        
        if (root_value == NULL)
        {
            /* Codes_SRS_GATEWAY_JSON_04_006: [ The function shall return GATEWAY_UPDATE_FROM_JSON_ERROR if the JSON content could not be parsed to a JSON_Value. ] */
            LogError("Input file [%s] could not be read.", json_content);
            result = GATEWAY_UPDATE_FROM_JSON_ERROR;
        }
        else
        {
            GATEWAY_PROPERTIES *properties = (GATEWAY_PROPERTIES*)malloc(sizeof(GATEWAY_PROPERTIES));
            if (properties == NULL)
            {
                /* Codes_SRS_GATEWAY_JSON_04_008: [ This function shall return GATEWAY_UPDATE_FROM_JSON_ERROR upon any memory allocation failure. ] */
                LogError("Failed to allocate GATEWAY_PROPERTIES.");
                result = GATEWAY_UPDATE_FROM_JSON_MEMORY;
            }
            else
            {
                properties->gateway_modules = NULL;
                properties->gateway_links = NULL;
                properties->deployConfig = NULL;
                JSON_Object *json_document = json_value_get_object(root_value);
                char* deployConfig = NULL;
                JSON_Value* dcJsonRoot = NULL;
                if (json_document != NULL)
                {
                    JSON_Value* gateway_config = json_object_get_value(json_document, GATEWAY_KEY);
                    JSON_Object* gateway_object = json_value_get_object(gateway_config);
                    //     JSON_Object* gateway_object = json_object_get_object(json_document, GATEWAY_KEY);
                    deployConfig = json_object_get_string(gateway_object, "deploy-path");
                    //    const char* deployConfig = json_value_get_string (gateway_config, "deploy-path");
                    if (deployConfig != NULL) {
                        FILE* fpDC = fopen(deployConfig, "r");
                        if (fpDC == NULL) {
                            unsigned char* baseDCJsonString = "{\"modules\":[]}";
                            dcJsonRoot = json_parse_string(baseDCJsonString);
                            properties->deployConfig = json_value_get_object(dcJsonRoot);
                        }
                        else {
                            fclose(fpDC);
                            dcJsonRoot = json_parse_file(deployConfig);
                            properties->deployConfig = json_value_get_object(dcJsonRoot);
                        }
                    }
                }
                /* Codes_SRS_GATEWAY_JSON_04_007: [ The function shall traverse the JSON_Value object to initialize a GATEWAY_PROPERTIES instance. ] */
                /* Codes_SRS_GATEWAY_JSON_04_011: [ The function shall be able to add just `modules`, just `links` or both. ] */
                if (parse_json_internal(properties, root_value) != PARSE_JSON_SUCCESS)
                {
                    /* Codes_SRS_GATEWAY_JSON_04_010: [ The function shall return GATEWAY_UPDATE_FROM_JSON_ERROR if the JSON_Value contains incomplete information. ] */
                    LogError("Failed to create properties structure from JSON configuration.");
                    result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                }
                else
                {
                    gw->deployConfig = properties->deployConfig;
                    VECTOR_HANDLE modules_added_successfully = VECTOR_create(sizeof(GATEWAY_MODULES_ENTRY));
                    if (modules_added_successfully == NULL)
                    {
                        LogError("Failed to create Vector for successfully added modules.");
                        result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                    }
                    else
                    {
                        VECTOR_HANDLE links_added_successfully = VECTOR_create(sizeof(GATEWAY_LINK_ENTRY));
                        if (links_added_successfully == NULL)
                        {
                            LogError("Failed to create Vector for successfully added links.");
                            result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                        }
                        else
                        {
                            if (properties->gateway_modules != NULL)
                            {
                                size_t entries_count = VECTOR_size(properties->gateway_modules);
                                if (entries_count > 0)
                                {
                                    //Add the first module, if successful add others
                                    GATEWAY_MODULES_ENTRY* entry = (GATEWAY_MODULES_ENTRY*)VECTOR_element(properties->gateway_modules, 0);
                                    MODULE_HANDLE module = gateway_addmodule_internal(gw, entry, true);

                                    if (module != NULL)
                                    {
                                        if (VECTOR_push_back(modules_added_successfully, entry, 1) != 0)
                                        {
                                            LogError("Failed to save successfully added module.");
                                            if (Gateway_RemoveModuleByName(gw, entry->module_name) != 0)
                                            {
                                                LogError("Failed to remove module %s upon failure.", entry->module_name);
                                            }
                                            module = NULL;
                                            result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                        }
                                    }
                                    else
                                    {
                                        result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                    }

                                    //Continue adding modules until all are added or one fails
                                    for (size_t properties_index = 1; properties_index < entries_count && module != NULL; ++properties_index)
                                    {
                                        entry = (GATEWAY_MODULES_ENTRY*)VECTOR_element(properties->gateway_modules, properties_index);
                                        module = gateway_addmodule_internal(gw, entry, true);

                                        if (module != NULL)
                                        {
                                            if (VECTOR_push_back(modules_added_successfully, entry, 1) != 0)
                                            {
                                                LogError("Failed to save successfully added module.");
                                                if (Gateway_RemoveModuleByName(gw, entry->module_name) != 0)
                                                {
                                                    LogError("Failed to remove module %s up failure.", entry->module_name);
                                                }
                                                module = NULL;
                                                result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                            }
                                        }
                                        else
                                        {
                                            result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                        }
                                    }

                                    if (module == NULL)
                                    {
                                        //Clean up modules.
                                        /* Codes_SRS_GATEWAY_JSON_04_009: [ The function shall be able to roll back previous operation if any module or link fails to be added. ] */
                                        rollbackModules(gw, modules_added_successfully);
                                    }
                                    else
                                    {
                                        //Notify Event System.
                                        GATEWAY_HANDLE_DATA* gateway = (GATEWAY_HANDLE_DATA*)gw;
                                        EventSystem_ReportEvent(gateway->event_system, gateway, GATEWAY_MODULE_LIST_CHANGED);
                                        result = GATEWAY_UPDATE_FROM_JSON_SUCCESS;
                                    }
                                }
                                else
                                {
                                    result = GATEWAY_UPDATE_FROM_JSON_SUCCESS;
                                }
                            }
                            else
                            {
                                result = GATEWAY_UPDATE_FROM_JSON_SUCCESS;
                            }


                            if (result == 0 && properties->gateway_links != NULL)
                            {
                                size_t entries_count = VECTOR_size(properties->gateway_links);

                                if (entries_count > 0)
                                {
                                    //Add the first link, if successfull add others
                                    GATEWAY_LINK_ENTRY* entry = (GATEWAY_LINK_ENTRY*)VECTOR_element(properties->gateway_links, 0);
                                    bool linkAdded = gateway_addlink_internal(gw, entry);

                                    if (linkAdded)
                                    {
                                        if (VECTOR_push_back(links_added_successfully, entry, 1) != 0)
                                        {
                                            LogError("Failed to save successfully added link.");
                                            Gateway_RemoveLink(gw, entry);
                                            linkAdded = false;
                                            result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                        }
                                    }
                                    else
                                    {
                                        result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                    }


                                    //Continue adding links until all are added or one fails
                                    for (size_t links_index = 1; links_index < entries_count && linkAdded; ++links_index)
                                    {
                                        entry = (GATEWAY_LINK_ENTRY*)VECTOR_element(properties->gateway_links, links_index);
                                        linkAdded = gateway_addlink_internal(gw, entry);
                                        if (linkAdded)
                                        {
                                            if (VECTOR_push_back(links_added_successfully, entry, 1) != 0)
                                            {
                                                LogError("Failed to save successfully added link.");
                                                Gateway_RemoveLink(gw, entry);
                                                linkAdded = false;
                                                result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                            }
                                        }
                                        else
                                        {
                                            result = GATEWAY_UPDATE_FROM_JSON_ERROR;
                                        }
                                    }

                                    if (!linkAdded)
                                    {

                                        size_t success_link_entries_count = VECTOR_size(links_added_successfully);

                                        //Clean up links. Have to clean up links first before removing modules.
                                        /* Codes_SRS_GATEWAY_JSON_04_009: [ The function shall be able to roll back previous operation if any module or link fails to be added. ] */
                                        if (success_link_entries_count > 0)
                                        {
                                            for (size_t properties_index = 0; properties_index < success_link_entries_count; ++properties_index)
                                            {
                                                GATEWAY_LINK_ENTRY* entry = (GATEWAY_LINK_ENTRY*)VECTOR_element(links_added_successfully, properties_index);
                                                Gateway_RemoveLink(gw, entry);
                                            }
                                        }

                                        //Clean up modules.
                                        /* Codes_SRS_GATEWAY_JSON_04_009: [ The function shall be able to roll back previous operation if any module or link fails to be added. ] */
                                        rollbackModules(gw, modules_added_successfully);

                                        LogError("Unable to add link from '%s' to '%s'.Rolling back Update Operation.", entry->module_source, entry->module_sink);
                                    }
                                }
                            }
                            VECTOR_destroy(links_added_successfully);
                        }
                        VECTOR_destroy(modules_added_successfully);
                    }
                }
                if (deployConfig!=NULL&& dcJsonRoot != NULL) {
                    json_serialize_to_file(dcJsonRoot, deployConfig);
                    json_value_free(dcJsonRoot);
                }
                destroy_properties_internal(properties);
                free(properties);
            }
            json_value_free(root_value);
        }
        Lock(gw->update_lock);
        gw->runtime_status = GATEWAY_RUNTIME_STATUS_UPDATED;
        Unlock(gw->update_lock);
    }

    return result;
}


static void destroy_properties_internal(GATEWAY_PROPERTIES* properties)
{
    if (properties->gateway_modules != NULL)
    {
        size_t vector_size = VECTOR_size(properties->gateway_modules);
        for (size_t element_index = 0; element_index < vector_size; ++element_index)
        {
            GATEWAY_MODULES_ENTRY* element = (GATEWAY_MODULES_ENTRY*)VECTOR_element(properties->gateway_modules, element_index);
            element->module_loader_info.loader->api->FreeEntrypoint(element->module_loader_info.loader, element->module_loader_info.entrypoint);
            json_free_serialized_string((char*)(element->module_configuration));
        }

        VECTOR_destroy(properties->gateway_modules);
        properties->gateway_modules = NULL;
    }

    if (properties->gateway_links != NULL)
    {
        VECTOR_destroy(properties->gateway_links);
        properties->gateway_links = NULL;
    }
}

static PARSE_JSON_RESULT parse_loader(JSON_Object* loader_json, GATEWAY_MODULE_LOADER_INFO* loader_info, JSON_Object* deployConfig)
{
    PARSE_JSON_RESULT result;

    // get loader name; we assume it is "native" if the JSON doesn't have a name
    /*Codes_SRS_GATEWAY_JSON_17_013: [ The function shall parse each modules object for "loader.name" and "loader.entrypoint". ]*/
    const char* loader_name = json_object_get_string(loader_json, LOADER_NAME_KEY);
    if (loader_name == NULL)
    {
        //Codes_SRS_GATEWAY_JSON_13_001: [ If loader.name is not found in the JSON then the gateway assumes that the loader name is native. ]
        loader_name = DYNAMIC_LOADER_NAME;
    }

    // locate the loader
    /*Codes_SRS_GATEWAY_JSON_17_014: [ The function shall find the correct loader by "loader.name". ]*/
    const MODULE_LOADER* loader = ModuleLoader_FindByName(loader_name);
    if (loader == NULL)
    {
        /*Codes_SRS_GATEWAY_JSON_17_010: [ If the module's loader is not found by name, the the function shall fail and return NULL. ]*/
        LogError("Loader JSON has a non-existent loader 'name' specified - %s.", loader_name);
        result = PARSE_JSON_MISSING_OR_MISCONFIGURED_CONFIG;
    }
    else
    {
        loader_info->loader = loader;

        // get entrypoint
        JSON_Value* entrypoint_json = json_object_get_value(loader_json, LOADER_ENTRYPOINT_KEY);
        if (entrypoint_json != NULL) {
            // download logic module
            JSON_Object* entrypoint = json_value_get_object(entrypoint_json);
            const char* modulePath = json_object_get_string(entrypoint, MODULE_REMOTE_URL);
            const char* deployPath = json_object_get_string(entrypoint, MODULE_PATH_KEY);
            JSON_Value* entrypointParrent_json = json_value_get_parent(entrypoint_json);
            const char* newModuleVersion = NULL;
            const char* moduleName = NULL;
            if (entrypointParrent_json != NULL) {
                JSON_Value* moduleEntry_json = json_value_get_parent(entrypointParrent_json);
                JSON_Object* entryPointParent = json_value_get_object(moduleEntry_json);
                newModuleVersion = json_object_get_string(entryPointParent, "version");
                moduleName = json_object_get_string(entryPointParent, "name");
            }
            if (modulePath != NULL && (modulePath[0] == 'h'&&modulePath[1] == 't'&&modulePath[2] == 't'&&modulePath[3] == 'p' &&modulePath[4] == 's')) {
                const char* currentModuleVersion = gateway_get_current_module_version(deployConfig, moduleName);
                if (currentModuleVersion==NULL || (currentModuleVersion!=NULL && strcmp(newModuleVersion, currentModuleVersion) != 0)) {
                    unsigned char* remoteModuleHost;
                    unsigned char* remoteModulePath;
                    int modulePathIndex = strlen("https://");
                    int modulePathLength = strlen(modulePath);
                    int lastIndex = modulePathIndex;
                    while (modulePathIndex < modulePathLength) {
                        if (modulePath[modulePathIndex] == '/') {
                            break;
                        }
                        modulePathIndex++;
                    }
                    remoteModuleHost = (unsigned char*)malloc(modulePathIndex - lastIndex + 1);
                    memcpy(remoteModuleHost, &modulePath[lastIndex], modulePathIndex - lastIndex);
                    remoteModuleHost[modulePathIndex - lastIndex] = '\0';
                    remoteModulePath = (unsigned char*)malloc(modulePathLength - modulePathIndex + 2);
                    memcpy(remoteModulePath, &modulePath[modulePathIndex], modulePathLength - modulePathIndex + 1);
                    remoteModulePath[modulePathLength - modulePathIndex + 1] = '\0';

                    HTTP_HEADERS_HANDLE httpReqHeadersHandle, httpResHeadersHandle;
                    httpReqHeadersHandle = HTTPHeaders_Alloc();
                    httpResHeadersHandle = HTTPHeaders_Alloc();
                    if (httpReqHeadersHandle == NULL || httpResHeadersHandle == NULL) {
                        LogError("Failed to allocate http headers");
                        if (httpReqHeadersHandle != NULL) HTTPHeaders_Free(httpReqHeadersHandle);
                        if (httpResHeadersHandle != NULL) HTTPHeaders_Free(httpResHeadersHandle);
                    }
                    else {
                        BUFFER_HANDLE reqContent = BUFFER_new();
                        BUFFER_HANDLE resContent = BUFFER_new();
                        if (reqContent == NULL || resContent == NULL) {
                            LogError("Failed to allocate http contents");
                            if (reqContent != NULL) BUFFER_delete(reqContent);
                            if (resContent != NULL) BUFFER_delete(resContent);
                            HTTPHeaders_Free(httpReqHeadersHandle);
                            HTTPHeaders_Free(httpResHeadersHandle);
                        }
                        else {
                            unsigned int statusCode;
                            HTTPAPIEX_HANDLE httpapiexHandle = HTTPAPIEX_Create(remoteModuleHost);
                            if (httpapiexHandle == NULL) {
                                LogError("Failed to create httpapiex handle");
                                HTTPHeaders_Free(httpReqHeadersHandle);
                                HTTPHeaders_Free(httpResHeadersHandle);
                                BUFFER_delete(reqContent);
                                BUFFER_delete(resContent);
                            }
                            else {
                                HTTPAPIEX_RESULT httpApiExResult = HTTPAPIEX_ExecuteRequest(httpapiexHandle, HTTPAPI_REQUEST_GET, remoteModulePath, httpReqHeadersHandle, reqContent, &statusCode, httpResHeadersHandle, resContent);
                                if (httpApiExResult == HTTPAPIEX_OK) {
                                    if (statusCode == 200) {
                                        LogInfo("Received library - %s : %d byte", deployPath, BUFFER_length(resContent));
                                        FILE* fp = fopen(deployPath, "wb");
                                        if (fp != NULL) {
                                            fwrite(BUFFER_u_char(resContent), sizeof(unsigned char), BUFFER_length(resContent), fp);
                                            fclose(fp);
                                        }
                                        else {
                                            LogError("Loaded module file can't be stored - %s", deployPath);
                                        }
                                    }
                                    else {
                                        LogInfo("http access result %d", statusCode);
                                    }
                                }
                                else {
                                    LogError("http access failed %d", httpApiExResult);
                                }
                                HTTPAPIEX_Destroy(httpapiexHandle);
                                HTTPHeaders_Free(httpReqHeadersHandle);
                                HTTPHeaders_Free(httpResHeadersHandle);
                                BUFFER_delete(reqContent);
                                BUFFER_delete(resContent);
                            }
                        }
                    }
                    if (remoteModuleHost != NULL) free(remoteModuleHost);
                    if (remoteModulePath != NULL) free(remoteModulePath);
                }
            }
        }
        loader_info->entrypoint = entrypoint_json == NULL ? NULL :
                    loader->api->ParseEntrypointFromJson(loader, entrypoint_json);

        // if entrypoint_json is not NULL then loader_info->entrypoint must not be NULL
        if (entrypoint_json != NULL && loader_info->entrypoint == NULL)
        {
            LogError("An error occurred when parsing the entrypoint for loader - %s.", loader_name);
            result = PARSE_JSON_MISSING_OR_MISCONFIGURED_CONFIG;
        }
        else
        {
            result = PARSE_JSON_SUCCESS;
        }
    }

    return result;
}

static PARSE_JSON_RESULT parse_json_internal(GATEWAY_PROPERTIES* out_properties, JSON_Value *root)
{
    PARSE_JSON_RESULT result;

    JSON_Object *json_document = json_value_get_object(root);
    if (json_document != NULL)
    {
        // initialize the module loader configuration
        /*Codes_SRS_GATEWAY_JSON_17_007: [ The function shall parse the "loaders" JSON array and initialize new module loaders or update the existing default loaders. ]*/
        // "loaders" is not required in gateway JSON
        JSON_Value *loaders = json_object_get_value(json_document, LOADERS_KEY);
        if (loaders == NULL || ModuleLoader_InitializeFromJson(loaders) == MODULE_LOADER_SUCCESS)
        {
            JSON_Array *modules_array = json_object_get_array(json_document, MODULES_KEY);
            JSON_Array *links_array = json_object_get_array(json_document, LINKS_KEY);

            if (modules_array != NULL || links_array != NULL)
            {
                if (modules_array != NULL)
                {
                    out_properties->gateway_modules = VECTOR_create(sizeof(GATEWAY_MODULES_ENTRY));
                    if (out_properties->gateway_modules != NULL)
                    {
                        /*Codes_SRS_GATEWAY_JSON_17_008: [ The function shall parse the "modules" JSON array for each module entry. ]*/
                        JSON_Object *module;
                        size_t module_count = json_array_get_count(modules_array);
                        result = PARSE_JSON_SUCCESS;
                        for (size_t module_index = 0; module_index < module_count; ++module_index)
                        {
                            module = json_array_get_object(modules_array, module_index);

                            /*Codes_SRS_GATEWAY_JSON_17_009: [ For each module, the function shall call the loader's ParseEntrypointFromJson function to parse the entrypoint JSON. ]*/
                            JSON_Object* loader_args = json_object_get_object(module, LOADER_KEY);
                            GATEWAY_MODULE_LOADER_INFO loader_info;
                            if (parse_loader(loader_args, &loader_info, out_properties->deployConfig) != PARSE_JSON_SUCCESS)
                            {
                                result = PARSE_JSON_MISSING_OR_MISCONFIGURED_CONFIG;
                                LogError("Failed to parse loader configuration.");
                                break;
                            }
                            else
                            {
                                const char* module_name = json_object_get_string(module, MODULE_NAME_KEY);
                                if (module_name != NULL)
                                {
                                    /*Codes_SRS_GATEWAY_JSON_14_005: [The function shall set the value of const void* module_properties in the GATEWAY_PROPERTIES instance to a char* representing the serialized args value for the particular module.]*/
                                    JSON_Value *args = json_object_get_value(module, ARG_KEY);
                                    char* args_str = json_serialize_to_string(args);
                                    char* version_str = json_object_get_string(module, "version");
                                    GATEWAY_MODULES_ENTRY entry = {
                                        module_name,
                                        loader_info,
                                        args_str,
                                        NULL
                                    };
                                    if (version_str != NULL) {
                                        entry.module_version = version_str;
                                    }

                                    /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
                                    if (VECTOR_push_back(out_properties->gateway_modules, &entry, 1) == 0)
                                    {
                                        result = PARSE_JSON_SUCCESS;
                                    }
                                    else
                                    {
                                        loader_info.loader->api->FreeEntrypoint(loader_info.loader, loader_info.entrypoint);
                                        json_free_serialized_string(args_str);
                                        result = PARSE_JSON_VECTOR_FAILURE;
                                        LogError("Failed to push data into properties vector.");
                                        break;
                                    }
                                }
                                /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
                                else
                                {
                                    loader_info.loader->api->FreeEntrypoint(loader_info.loader, loader_info.entrypoint);
                                    result = PARSE_JSON_MISSING_OR_MISCONFIGURED_CONFIG;
                                    LogError("\"module name\" or \"module path\" in input JSON configuration is missing or misconfigured.");
                                    break;
                                }
                            }
                        }

                    }
                    /* Codes_SRS_GATEWAY_JSON_14_008: [ This function shall return NULL upon any memory allocation failure. ] */
                    else
                    {
                        result = PARSE_JSON_VECTOR_FAILURE;
                        LogError("Failed to create properties vector. ");
                    }
                }
                else
                {
                    result = PARSE_JSON_SUCCESS;
                    out_properties->gateway_modules = NULL;
                }

                if (result == PARSE_JSON_SUCCESS)
                {
                    if (links_array != NULL)
                    {
                        /* Codes_SRS_GATEWAY_JSON_04_001: [ The function shall create a Vector to Store all links to this gateway. ] */
                        out_properties->gateway_links = VECTOR_create(sizeof(GATEWAY_LINK_ENTRY));
                        if (out_properties->gateway_links != NULL)
                        {
                            JSON_Object *route;
                            size_t links_count = json_array_get_count(links_array);
                            for (size_t links_index = 0; links_index < links_count; ++links_index)
                            {
                                route = json_array_get_object(links_array, links_index);
                                const char* module_source = json_object_get_string(route, SOURCE_KEY);
                                const char* module_sink = json_object_get_string(route, SINK_KEY);
                                const char* message_type = json_object_get_string(route, LINK_MSGTYPE_KEY);

                                if (module_source != NULL && module_sink != NULL)
                                {
                                    GATEWAY_LINK_ENTRY entry = {
                                        module_source,
                                        module_sink
                                    };
                                    if (message_type != NULL&&strcmp(message_type, "thread-message") == 0) {
                                        entry.message_type = GATEWAY_LINK_ENTRY_MESSAGE_TYPE_THREAD;
                                    }
                                    else {
                                        entry.message_type = GATEWAY_LINK_ENTRY_MESSAGE_TYPE_DEFAULT;
                                    }

                                    /* Codes_SRS_GATEWAY_JSON_04_002: [ The function shall add all modules source and sink to GATEWAY_PROPERTIES inside gateway_links. ] */
                                    if (VECTOR_push_back(out_properties->gateway_links, &entry, 1) == 0)
                                    {
                                        result = PARSE_JSON_SUCCESS;
                                    }
                                    else
                                    {
                                        result = PARSE_JSON_VECTOR_FAILURE;
                                        LogError("Failed to push data into links vector.");
                                        break;
                                    }
                                }
                                /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
                                else
                                {
                                    result = PARSE_JSON_MISSING_OR_MISCONFIGURED_CONFIG;
                                    LogError("\"source\" or \"sink\" in input JSON configuration is missing or misconfigured.");
                                    break;
                                }
                            }
                        }
                        /* Codes_SRS_GATEWAY_JSON_14_008: [ This function shall return NULL upon any memory allocation failure. ] */
                        else
                        {
                            result = PARSE_JSON_VECTOR_FAILURE;
                            LogError("Failed to create links vector. ");
                        }
                    }
                    else
                    {
                        out_properties->gateway_links = NULL;
                    }
                }
                else
                {
                    out_properties->gateway_links = NULL;
                }
            }
            /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
            else
            {
                result = PARSE_JSON_MISCONFIGURED_OR_OTHER;
                LogError("JSON Configuration file is configured incorrectly or some other error occurred while parsing.");
            }
        }
        /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
        else
        {
            result = PARSE_JSON_MISCONFIGURED_OR_OTHER;
            LogError("An error occurred while parsing the loaders configuration for the gateway.");
        }
    }
    /*Codes_SRS_GATEWAY_JSON_14_006: [The function shall return NULL if the JSON_Value contains incomplete information.]*/
    else
    {
        result = PARSE_JSON_MISCONFIGURED_OR_OTHER;
        LogError("JSON Configuration file is configured incorrectly or some other error occurred while parsing.");
    }
    return result;
}
