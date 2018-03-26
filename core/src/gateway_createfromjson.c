// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/macro_utils.h"
#include "gateway.h"
#include "parson.h"
#include "experimental/event_system.h"

#include "module_loaders/dynamic_loader.h"
#include "gateway_internal.h"

#define MODULES_KEY "modules"
#define LOADERS_KEY "loaders"
#define LOADER_KEY "loader"
#define MODULE_NAME_KEY "name"
#define LOADER_NAME_KEY "name"
#define LOADER_ENTRYPOINT_KEY "entrypoint"
#define MODULE_PATH_KEY "module.path"
#define ARG_KEY "args"

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
                destroy_properties_internal(properties);
                free(properties);
            }
            json_value_free(root_value);
        }
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

static PARSE_JSON_RESULT parse_loader(JSON_Object* loader_json, GATEWAY_MODULE_LOADER_INFO* loader_info)
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
                            if (parse_loader(loader_args, &loader_info) != PARSE_JSON_SUCCESS)
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

                                    GATEWAY_MODULES_ENTRY entry = {
                                        module_name,
                                        loader_info,
                                        args_str
                                    };

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
