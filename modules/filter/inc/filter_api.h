// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifndef _FILTER_API_H
#define _FILTER_API_H

#include "azure_c_shared_utility/constmap.h"
#include "azure_c_shared_utility/constbuffer.h"

#include <parson.h>

typedef void* RESOLVER_HANDLE;

typedef struct RESOLVER_API_TAG RESOLVER_API;


#ifdef __cplusplus
extern "C"
{
#endif

	typedef void* RESOLVER_CONTEXT;
	typedef STRING_HANDLE (*MESSAGE_RESOLVER)(RESOLVER_CONTEXT* handle, const char* name, const CONSTBUFFER* buffer);

	typedef struct DIPATCH_ENTRY_TAG
	{
		const char* name;
		const char* characteristic_uuid;
		MESSAGE_RESOLVER message_resolver;
	} DIPATCH_ENTRY;

typedef struct RESOLVER_DATA_TAG
{
    const RESOLVER_API* resolver_apis;
    RESOLVER_HANDLE resolver_handle;
} RESOLVER_DATA;


typedef RESOLVER_HANDLE(*rResolver_Create)(MODULE_HANDLE module, const char* resolverName);
typedef RESOLVER_CONTEXT*(*rResolver_CreateContext)(RESOLVER_HANDLE module, JSON_Object* configuration);
typedef void(*rResolver_Destroy)(RESOLVER_HANDLE resolverHandle);
typedef CONSTBUFFER_HANDLE(*rResolver_Resolve)(RESOLVER_CONTEXT* context, const char* characteristics, const char* timestamp, const CONSTBUFFER* message);

typedef enum RESOLVER_API_VERSION_TAG
{
    RESOLVER_API_VERSION_1
} RESOLVER_API_VERSION;

static const RESOLVER_API_VERSION Resolver_ApiGatewayVersion = RESOLVER_API_VERSION_1;

struct RESOLVER_API_TAG
{
    RESOLVER_API_VERSION version;
};

typedef struct RESOLVER_API_1_TAG
{
    RESOLVER_API base;
    rResolver_Create Resolver_Create;
    rResolver_CreateContext Resolver_CreateContext;
    rResolver_Resolve Resolver_Resolve;
    rResolver_Destroy Resolver_Destroy;
} RESOLVER_API_1;

typedef const RESOLVER_API* (*rResolver_GetApi)(RESOLVER_API_VERSION resolver_api_version);

#define RESOLVER_GETAPI_NAME ("Resolver_GetApi")

#ifdef _WIN32
#define RESOLVER_EXPORT __declspec(dllexport)
#else
#define RESOLVER_EXPORT
#endif // _WIN32

#define RESOLVER_STATIC_GETAPI(RESOLVER_NAME) C2(Resolver_GetApi_, RESOLVER_NAME)

RESOLVER_EXPORT const RESOLVER_API* Resolver_GetApi(RESOLVER_API_VERSION resolver_api_version);

#define RESOLVER_CREATE(resolver_api_ptr) (((const RESOLVER_API_1*)(resolver_api_ptr))->Resolver_Create)
#define RESOLVER_DESTROY(resolver_api_ptr) (((const RESOLVER_API_1*)(resolver_api_ptr))->Resolver_Destroy)
#define RESOLVER_CREATE_CONTEXT(resolver_api_ptr) (((const RESOLVER_API_1*)(resolver_api_ptr))->Resolver_CreateContext)
#define RESOLVER_RESOLVE(resolver_api_ptr) (((const RESOLVER_API_1*)(resolver_api_ptr))->Resolver_Resolve)

#ifdef __cplusplus
}
#endif

#endif /*_FILTER_API_H*/
