// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include <stdbool.h>

#include "azure_c_shared_utility/gballoc.h"
#include "azure_c_shared_utility/vector.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/lock.h"
#include "azure_c_shared_utility/condition.h"
#include "azure_c_shared_utility/threadapi.h"
#include "azure_c_shared_utility/xlogging.h"
#include "azure_c_shared_utility/refcount.h"
#include "azure_c_shared_utility/singlylinkedlist.h"
#include "azure_c_shared_utility/uniqueid.h"

#include "nanomsg/nn.h"
#include "nanomsg/pubsub.h"

#include "message.h"
#include "module.h"
#include "module_access.h"
#include "broker.h"

/* minimum size for a guid string, 36 characters + null terminator */
#define BROKER_GUID_SIZE 37
#define INPROC_URL_HEAD "inproc://"
#define INPROC_URL_HEAD_SIZE 9
#define URL_SIZE (INPROC_URL_HEAD_SIZE + BROKER_GUID_SIZE +1)

/*The structure backing the message broker handle*/
typedef struct BROKER_HANDLE_DATA_TAG
{
    SINGLYLINKEDLIST_HANDLE modules;
    LOCK_HANDLE             modules_lock;
    int                     publish_socket;
    STRING_HANDLE           url;
}BROKER_HANDLE_DATA;

DEFINE_REFCOUNT_TYPE(BROKER_HANDLE_DATA);

typedef struct THREAD_MESSAGE_CTRL_TAG {
    MESSAGE_HANDLE msg;
    void* next;
} THREAD_MESSAGE_CTRL;

typedef struct THREAD_MESSAGE_HANDLING_RECEIVER_TAG {
    LOCK_HANDLE lock;
    COND_HANDLE condition;
    THREAD_HANDLE receiver_thread;
    void* module_info;  // this type should be BROKER_MODULEINFO*
    void* senders;  // this type should be THREAD_MESSAGE_HANDLING_SENDERS_IN_RECEIVER
    bool toContinue;
} THREAD_MESSAGE_HANDLING_RECEIVER;

typedef struct THREAD_MESSAGE_HANDLING_RECEIVERS_IN_SENDER_TAG{
    THREAD_MESSAGE_HANDLING_RECEIVER* receiver;
    THREAD_MESSAGE_CTRL*    sendingMessages;
    void* next;
} THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER;

typedef struct THREAD_MESSAGE_HANDLING_SENDER_TAG {
    LOCK_HANDLE lock;
    THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* receivers;
} THREAD_MESSAGE_HANDLING_SENDER;


typedef struct BROKER_MODULEINFO_TAG
{
    /** Handle to the module that's associated with the broker */
    MODULE*         module;
    /** Handle to the thread on which this module's message processing loop is
     *  running
     */
    THREAD_HANDLE   thread;
    /** Socket this module will receive messages on */
    int             receive_socket;
    /** Lock to prevent nanomsg race condition */
    LOCK_HANDLE     socket_lock;
    /** Guid sent to module worker thread to close task */
    STRING_HANDLE   quit_message_guid;

    LOCK_HANDLE     fc_lock;

    THREAD_MESSAGE_HANDLING_SENDER*   senderThMsg;
    THREAD_MESSAGE_HANDLING_RECEIVER* receiverThMsg;

}BROKER_MODULEINFO;

typedef struct THREAD_MESSASGE_HANDLING_SENDER_FOR_RECEIVER_TAG {
    BROKER_MODULEINFO* sender_module_info;
    void* next;
} THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER;



static STRING_HANDLE construct_url()
{
    STRING_HANDLE result;

    /*Codes_SRS_BROKER_17_002: [ Broker_Create shall create a unique id. ]*/
    char uuid[BROKER_GUID_SIZE];
    memset(uuid, 0, BROKER_GUID_SIZE);
    if (UniqueId_Generate(uuid, BROKER_GUID_SIZE) != UNIQUEID_OK)
    {
        LogError("Unable to generate unique Id.");
        result = NULL;
    }
    else
    {
        /*Codes_SRS_BROKER_17_003: [ Broker_Create shall initialize a url consisting of "inproc://" + unique id. ]*/
        result = STRING_construct(INPROC_URL_HEAD);
        if (result == NULL)
        {
            LogError("Unable to construct url.");
        }
        else
        {
            if (STRING_concat(result, uuid) != 0)
            {
                /*Codes_SRS_BROKER_13_003: [ This function shall return NULL if an underlying API call to the platform causes an error. ]*/
                STRING_delete(result);
                LogError("Unable to append uuid to url.");
                result = NULL;
            }
        }
    }
    return result;
}

BROKER_HANDLE Broker_Create(void)
{
    BROKER_HANDLE_DATA* result;

    /*Codes_SRS_BROKER_13_067: [Broker_Create shall malloc a new instance of BROKER_HANDLE_DATA and return NULL if it fails.]*/
    result = REFCOUNT_TYPE_CREATE(BROKER_HANDLE_DATA);
    if (result == NULL)
    {
        LogError("malloc returned NULL");
        /*return as is*/
    }
    else
    {
        /*Codes_SRS_BROKER_13_007: [Broker_Create shall initialize BROKER_HANDLE_DATA::modules with a valid VECTOR_HANDLE.]*/
        result->modules = singlylinkedlist_create();
        if (result->modules == NULL)
        {
            /*Codes_SRS_BROKER_13_003: [This function shall return NULL if an underlying API call to the platform causes an error.]*/
            LogError("VECTOR_create failed");
            free(result);
            result = NULL;
        }
        else
        {
            /*Codes_SRS_BROKER_13_023: [Broker_Create shall initialize BROKER_HANDLE_DATA::modules_lock with a valid LOCK_HANDLE.]*/
            result->modules_lock = Lock_Init();
            if (result->modules_lock == NULL)
            {
                /*Codes_SRS_BROKER_13_003: [This function shall return NULL if an underlying API call to the platform causes an error.]*/
                LogError("Lock_Init failed");
                singlylinkedlist_destroy(result->modules);
                free(result);
                result = NULL;
            }
            else
            {
                /*Codes_SRS_BROKER_17_001: [ Broker_Create shall initialize a socket for publishing messages. ]*/
                result->publish_socket = nn_socket(AF_SP, NN_PUB);
                if (result->publish_socket < 0)
                {
                    /*Codes_SRS_BROKER_13_003: [ This function shall return NULL if an underlying API call to the platform causes an error. ]*/
                    LogError("nanomsg puclish socket create failedL %d", result->publish_socket);
                    singlylinkedlist_destroy(result->modules);
                    Lock_Deinit(result->modules_lock);
                    free(result);
                    result = NULL;
                }
                else
                {
                    result->url = construct_url();
                    if (result->url == NULL)
                    {
                        /*Codes_SRS_BROKER_13_003: [ This function shall return NULL if an underlying API call to the platform causes an error. ]*/
                        singlylinkedlist_destroy(result->modules);
                        Lock_Deinit(result->modules_lock);
                        nn_close(result->publish_socket);
                        free(result);
                        LogError("Unable to generate unique url.");
                        result = NULL;
                    }
                    else
                    {
                        /*Codes_SRS_BROKER_17_004: [ Broker_Create shall bind the socket to the BROKER_HANDLE_DATA::url. ]*/
                        if (nn_bind(result->publish_socket, STRING_c_str(result->url)) < 0)
                        {
                            /*Codes_SRS_BROKER_13_003: [ This function shall return NULL if an underlying API call to the platform causes an error. ]*/
                            LogError("nanomsg bind failed");
                            singlylinkedlist_destroy(result->modules);
                            Lock_Deinit(result->modules_lock);
                            nn_close(result->publish_socket);
                            STRING_delete(result->url);                
                            free(result);
                            result = NULL;
                        }
                    }
                }
            }
        }
    }

    /*Codes_SRS_BROKER_13_001: [This API shall yield a BROKER_HANDLE representing the newly created message broker. This handle value shall not be equal to NULL when the API call is successful.]*/
    return result;
}

void Broker_IncRef(BROKER_HANDLE broker)
{
    /*Codes_SRS_BROKER_13_108: [If `broker` is NULL then Broker_IncRef shall do nothing.]*/
    if (broker == NULL)
    {
        LogError("invalid arg: broker is NULL");
    }
    else
    {
        /*Codes_SRS_BROKER_13_109: [Otherwise, Broker_IncRef shall increment the internal ref count.]*/
        INC_REF(BROKER_HANDLE_DATA, broker);
    }
}

/**
* This function runs for each module. It receives a pointer to a MODULE_INFO
* object that describes the module. Its job is to call the Receive function on
* the associated module whenever it receives a message.
*/
static int module_worker(void * user_data)
{
    /*Codes_SRS_BROKER_13_026: [This function shall assign `user_data` to a local variable called `module_info` of type `BROKER_MODULEINFO*`.]*/
    BROKER_MODULEINFO* module_info = (BROKER_MODULEINFO*)user_data;

    int should_continue = 1;
    while (should_continue)
    {
        /*Codes_SRS_BROKER_13_089: [ This function shall acquire the lock on module_info->socket_lock. ]*/
        if (Lock(module_info->socket_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_02_004: [ If acquiring the lock fails, then module_worker shall return. ]*/
            LogError("unable to Lock");
            should_continue = 0;
            break;
        }
        int nn_fd = module_info->receive_socket;
        int nbytes;
        unsigned char *buf = NULL;

        /*Codes_SRS_BROKER_17_005: [ For every iteration of the loop, the function shall wait on the receive_socket for messages. ]*/
        nbytes = nn_recv(nn_fd, (void *)&buf, NN_MSG, 0);
        /*Codes_SRS_BROKER_13_091: [ The function shall unlock module_info->socket_lock. ]*/
        if (Unlock(module_info->socket_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_17_016: [ If releasing the lock fails, then module_worker shall return. ]*/
            should_continue = 0;
            if (nbytes > 0)
            {
                /*Codes_SRS_BROKER_17_019: [ The function shall free the buffer received on the receive_socket. ]*/
                nn_freemsg(buf);
            }
            break;
        }

        if (nbytes < 0)
        {
            /*Codes_SRS_BROKER_17_006: [ An error on receiving a message shall terminate the loop. ]*/
            should_continue = 0;
        }
        if (should_continue!=0)
        {
            if (nbytes == BROKER_GUID_SIZE &&
                (strncmp(STRING_c_str(module_info->quit_message_guid), (const char *)buf, BROKER_GUID_SIZE-1)==0))
            {
                /*Codes_SRS_BROKER_13_068: [ This function shall run a loop that keeps running until module_info->quit_message_guid is sent to the thread. ]*/
                /* received special quit message for this module */
                should_continue = 0;
            }
            if (module_info->receiverThMsg != NULL) {
                if (Lock(module_info->receiverThMsg->lock) != LOCK_OK) {
                    LogError("The lock for receiverThMsg in module_worker failed!");
                }
                else {
                    // to check this message come from and ...
                    Unlock(module_info->receiverThMsg->lock);
                }
            }
            if(should_continue!=0)
            {
                /*Codes_SRS_BROKER_17_024: [ The function shall strip off the topic from the message. ]*/
                const unsigned char*buf_bytes = (const unsigned char*)buf;
                buf_bytes += sizeof(MODULE_HANDLE);
                /*Codes_SRS_BROKER_17_017: [ The function shall deserialize the message received. ]*/
                MESSAGE_HANDLE msg = Message_CreateFromByteArray(buf_bytes, nbytes - sizeof(MODULE_HANDLE));
                /*Codes_SRS_BROKER_17_018: [ If the deserialization is not successful, the message loop shall continue. ]*/
                if (msg != NULL)
                {
                    /*Codes_SRS_BROKER_13_092: [The function shall deliver the message to the module's callback function via module_info->module_apis. ]*/
                    MODULE_RECEIVE(module_info->module->module_apis)(module_info->module->module_handle, msg);
                    /*Codes_SRS_BROKER_13_093: [ The function shall destroy the message that was dequeued by calling Message_Destroy. ]*/
                    Message_Destroy(msg);
                }
            }
            /*Codes_SRS_BROKER_17_019: [ The function shall free the buffer received on the receive_socket. ]*/
            nn_freemsg(buf);
        }    
    }

    return 0;
}

static BROKER_RESULT init_module(BROKER_MODULEINFO* module_info, const MODULE* module)
{
    BROKER_RESULT result;

    /*Codes_SRS_BROKER_13_107: The function shall assign the `module` handle to `BROKER_MODULEINFO::module`.*/
    module_info->module = (MODULE*)malloc(sizeof(MODULE));
    if (module_info->module == NULL)
    {
        LogError("Allocate module failed");
        result = BROKER_ERROR;
    }
    else
    {
        module_info->module->module_apis = module->module_apis;
        module_info->module->module_handle = module->module_handle;
        module_info->module->module_loader_type = module->module_loader_type;

        /*Codes_SRS_BROKER_13_099: [The function shall initialize BROKER_MODULEINFO::socket_lock with a valid lock handle.]*/
        module_info->socket_lock = Lock_Init();
        if (module_info->socket_lock == NULL)
        {
            /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
            LogError("Lock_Init for socket lock failed");
            result = BROKER_ERROR;
        }
        else
        {
            char uuid[BROKER_GUID_SIZE];
            memset(uuid, 0, BROKER_GUID_SIZE);
/*Codes_SRS_BROKER_17_020: [ The function shall create a unique ID used as a quit signal. ]*/
if (UniqueId_Generate(uuid, BROKER_GUID_SIZE) != UNIQUEID_OK)
{
    /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
    LogError("Lock_Init for socket lock failed");
    Lock_Deinit(module_info->socket_lock);
    result = BROKER_ERROR;
}
else
{
    module_info->quit_message_guid = STRING_construct(uuid);
    if (module_info->quit_message_guid == NULL)
    {
        /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
        LogError("String construct failed for module guid");
        Lock_Deinit(module_info->socket_lock);
        result = BROKER_ERROR;
    }
    else
    {
        result = BROKER_OK;
    }
}
        }
        if (result == BROKER_OK) {
            module_info->fc_lock = Lock_Init();
        }
    }
    return result;
}

static void deinit_module(BROKER_MODULEINFO* module_info)
{
    /*Codes_SRS_BROKER_13_057: [The function shall free all members of the MODULE_INFO object.]*/
    Lock_Deinit(module_info->socket_lock);
    Lock_Deinit(module_info->fc_lock);

    STRING_delete(module_info->quit_message_guid);

    if (module_info->senderThMsg != NULL) {
        // Can I think senderThMsg has no receiver and no sending message?
        free((void*)module_info->senderThMsg);
    }
    if (module_info->receiverThMsg != NULL) {
        // Can I think receiverThMsg has no sender?
        if (Lock(module_info->receiverThMsg->lock) != LOCK_OK) {
            LogError("lock for receiverThMsg in deinit_module failed!");
        }
        else {
            module_info->receiverThMsg->toContinue = false;
            COND_RESULT cond_result = Condition_Post(module_info->receiverThMsg->condition);
            if (cond_result == COND_OK) {
                if (Unlock(module_info->receiverThMsg->lock) == LOCK_OK) {
                    int threadResult = 0;
                    ThreadAPI_Join(module_info->receiverThMsg->receiver_thread, &threadResult);
                }
                else {
                    LogError("unlock for receiverThMsg failed.");
                }
            }
            else {
                LogError("Post for receiveThMsg in deinit_module doesn't return COND_OK. value was %d", cond_result);
            }
        }
        free((void*)module_info->receiverThMsg);
    }

    free(module_info->module);
}

static BROKER_RESULT start_module(BROKER_MODULEINFO* module_info, STRING_HANDLE url)
{
    BROKER_RESULT result;

    /* Connect to pub/sub */
    /*Codes_SRS_BROKER_17_013: [ The function shall create a nanomsg socket for reception. ]*/
    module_info->receive_socket = nn_socket(AF_SP, NN_SUB);
    if (module_info->receive_socket < 0)
    {
        /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
        LogError("module receive socket create failed");
        result = BROKER_ERROR;
    }
    else
    {
        /*Codes_SRS_BROKER_17_014: [ The function shall bind the socket to the the BROKER_HANDLE_DATA::url. ]*/
        int connect_result = nn_connect(module_info->receive_socket, STRING_c_str(url));
        if (connect_result < 0)
        {
            /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
            LogError("nn_connect failed");
            nn_close(module_info->receive_socket);
            module_info->receive_socket = -1;
            result = BROKER_ERROR;
        }
        else
        {
            /* Codes_SRS_BROKER_17_028: [ The function shall subscribe BROKER_MODULEINFO::receive_socket to the quit signal GUID. ]*/
            if (nn_setsockopt(
                module_info->receive_socket, NN_SUB, NN_SUB_SUBSCRIBE, STRING_c_str(module_info->quit_message_guid), STRING_length(module_info->quit_message_guid)) < 0)
            {
                /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
                LogError("nn_setsockopt failed");
                nn_close(module_info->receive_socket);
                module_info->receive_socket = -1;
                result = BROKER_ERROR;
            }
            else
            {
                /*Codes_SRS_BROKER_13_102: [The function shall create a new thread for the module by calling ThreadAPI_Create using module_worker as the thread callback and using the newly allocated BROKER_MODULEINFO object as the thread context.*/
                if (ThreadAPI_Create(
                    &(module_info->thread),
                    module_worker,
                    (void*)module_info
                ) != THREADAPI_OK)
                {
                    /*Codes_SRS_BROKER_13_047: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
                    LogError("ThreadAPI_Create failed");
                    nn_close(module_info->receive_socket);
                    result = BROKER_ERROR;
                }
                else
                {
                    result = BROKER_OK;
                }
            }
        }
    }

    return result;
}

/*stop module means: stop the thread that feeds messages to Module_Receive function + deletion of all queued messages */
/*returns 0 if success, otherwise __LINE__*/
static int stop_module(int publish_socket, BROKER_MODULEINFO* module_info)
{
    int  quit_result, close_result, thread_result, result;

    /*Codes_SRS_BROKER_17_021: [ This function shall send a quit signal to the worker thread by sending BROKER_MODULEINFO::quit_message_guid to the publish_socket. ]*/
    /* send the unique quite id for this module */
    if ((quit_result = nn_send(publish_socket, STRING_c_str(module_info->quit_message_guid), BROKER_GUID_SIZE, 0)) < 0)
    {
        /*Codes_SRS_BROKER_17_015: [ This function shall close the BROKER_MODULEINFO::receive_socket. ]*/
        /* at the cost of a data race, we will close the socket to terminate the thread */
        nn_close(module_info->receive_socket);
        LogError("unable to peacefully close thread for module [%p], nn_send error [%d], taking harsher methods", module_info, quit_result);
    }
    else
    {
        /*Codes_SRS_BROKER_02_001: [ Broker_RemoveModule shall lock BROKER_MODULEINFO::socket_lock. ]*/
        if (Lock(module_info->socket_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_17_015: [ This function shall close the BROKER_MODULEINFO::receive_socket. ]*/
            /* at the cost of a data race, we will close the socket to terminate the thread */
            nn_close(module_info->receive_socket);
            LogError("unable to peacefully close thread for module [%p], Lock error, taking harsher methods", module_info );
        }
        else
        {
            /*Codes_SRS_BROKER_17_015: [ This function shall close the BROKER_MODULEINFO::receive_socket. ]*/
            close_result = nn_close(module_info->receive_socket);
            if (close_result < 0)
            {
                LogError("Receive socket close failed for module at  item [%p] failed", module_info);
            }
            else
            {
                /*all is fine, thread will eventually stop and be joined*/
            }
            /*Codes_SRS_BROKER_02_003: [ After closing the socket, Broker_RemoveModule shall unlock BROKER_MODULEINFO::info_lock. ]*/
            if (Unlock(module_info->socket_lock) != LOCK_OK)
            {
                LogError("unable to unlock socket lock");
            }
        }
    }
    /*Codes_SRS_BROKER_13_104: [The function shall wait for the module's thread to exit by joining BROKER_MODULEINFO::thread via ThreadAPI_Join. ]*/
    if (ThreadAPI_Join(module_info->thread, &thread_result) != THREADAPI_OK)
    {
        result = __LINE__;
        LogError("ThreadAPI_Join() returned an error.");
    }
    else
    {
        result = 0;
    }
    return result;
}

static bool find_module_predicate(LIST_ITEM_HANDLE list_item, const void* value)
{
    BROKER_MODULEINFO* element = (BROKER_MODULEINFO*)singlylinkedlist_item_get_value(list_item);
    return element->module->module_handle == ((MODULE*)value)->module_handle;
}

BROKER_RESULT Broker_RemoveModule(BROKER_HANDLE broker, const MODULE* module)
{
    /*Codes_SRS_BROKER_13_048: [If `broker` or `module` is NULL the function shall return BROKER_INVALIDARG.]*/
    BROKER_RESULT result;
    if (broker == NULL || module == NULL)
    {
        /*Codes_SRS_BROKER_13_053: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
        result = BROKER_INVALIDARG;
        LogError("invalid parameter (NULL).");
    }
    else
    {
        /*Codes_SRS_BROKER_13_088: [This function shall acquire the lock on BROKER_HANDLE_DATA::modules_lock.]*/
        BROKER_HANDLE_DATA* broker_data = (BROKER_HANDLE_DATA*)broker;
        if (Lock(broker_data->modules_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_13_053: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
            LogError("Lock on broker_data->modules_lock failed");
            result = BROKER_ERROR;
        }
        else
        {
            /*Codes_SRS_BROKER_13_049: [Broker_RemoveModule shall perform a linear search for module in BROKER_HANDLE_DATA::modules.]*/
            LIST_ITEM_HANDLE module_info_item = singlylinkedlist_find(broker_data->modules, find_module_predicate, module);

            if (module_info_item == NULL)
            {
                /*Codes_SRS_BROKER_13_050: [Broker_RemoveModule shall unlock BROKER_HANDLE_DATA::modules_lock and return BROKER_ERROR if the module is not found in BROKER_HANDLE_DATA::modules.]*/
                LogError("Supplied module is not attached to the broker");
                result = BROKER_ERROR;
            }
            else
            {
                BROKER_MODULEINFO* module_info = (BROKER_MODULEINFO*)singlylinkedlist_item_get_value(module_info_item);
                if (stop_module(broker_data->publish_socket, module_info) == 0)
                {
                    deinit_module(module_info);
                }
                else
                {
                    LogError("unable to stop module");
                }

                /*Codes_SRS_BROKER_13_052: [The function shall remove the module from BROKER_HANDLE_DATA::modules.]*/
                singlylinkedlist_remove(broker_data->modules, module_info_item);
                free(module_info);

                /*Codes_SRS_BROKER_13_053: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                result = BROKER_OK;
            }

            /*Codes_SRS_BROKER_13_054: [This function shall release the lock on BROKER_HANDLE_DATA::modules_lock.]*/
            Unlock(broker_data->modules_lock);
        }
    }

    return result;
}

BROKER_MODULEINFO* broker_locate_handle(BROKER_HANDLE_DATA* broker_data, MODULE_HANDLE handle)
{
    BROKER_MODULEINFO* result;
    MODULE module;
    module.module_apis = NULL;
    module.module_handle = handle;

    LIST_ITEM_HANDLE module_info_item = singlylinkedlist_find(broker_data->modules, find_module_predicate, &module);
    if (module_info_item == NULL)
    {
        result = NULL;
    }
    else
    {
result = (BROKER_MODULEINFO*)singlylinkedlist_item_get_value(module_info_item);
    }
    return result;
}

BROKER_RESULT Broker_AddModule(BROKER_HANDLE broker, const MODULE* module)
{
    BROKER_RESULT result;

    /*Codes_SRS_BROKER_99_013: [If `broker` or `module` is NULL the function shall return BROKER_INVALIDARG.]*/
    if (broker == NULL || module == NULL)
    {
        result = BROKER_INVALIDARG;
        LogError("invalid parameter (NULL).");
    }
    /*Codes_SRS_BROKER_99_014: [If `module_handle` or `module_apis` are `NULL` the function shall return `BROKER_INVALIDARG`.]*/
    else if (module->module_apis == NULL || module->module_handle == NULL)
    {
        result = BROKER_INVALIDARG;
        LogError("invalid parameter (NULL).");
    }
    else
    {
        BROKER_MODULEINFO* module_info = (BROKER_MODULEINFO*)malloc(sizeof(BROKER_MODULEINFO));
        if (module_info == NULL)
        {
            LogError("Allocate module info failed");
            result = BROKER_ERROR;
        }
        else
        {
            module_info->receiverThMsg = NULL;
            module_info->senderThMsg = NULL;
            if (init_module(module_info, module) != BROKER_OK)
            {
                /*Codes_SRS_BROKER_13_047: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                LogError("start_module failed");
                free(module_info->module);
                free(module_info);
                result = BROKER_ERROR;
            }
            else
            {
                /*Codes_SRS_BROKER_13_039: [This function shall acquire the lock on BROKER_HANDLE_DATA::modules_lock.]*/
                BROKER_HANDLE_DATA* broker_data = (BROKER_HANDLE_DATA*)broker;
                if (Lock(broker_data->modules_lock) != LOCK_OK)
                {
                    /*Codes_SRS_BROKER_13_047: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                    LogError("Lock on broker_data->modules_lock failed");
                    deinit_module(module_info);
                    free(module_info);
                    result = BROKER_ERROR;
                }
                else
                {
                    /*Codes_SRS_BROKER_13_045: [Broker_AddModule shall append the new instance of BROKER_MODULEINFO to BROKER_HANDLE_DATA::modules.]*/
                    LIST_ITEM_HANDLE moduleListItem = singlylinkedlist_add(broker_data->modules, module_info);
                    if (moduleListItem == NULL)
                    {
                        /*Codes_SRS_BROKER_13_047: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                        LogError("singlylinkedlist_add failed");
                        deinit_module(module_info);
                        free(module_info);
                        result = BROKER_ERROR;
                    }
                    else
                    {
                        if (start_module(module_info, broker_data->url) != BROKER_OK)
                        {
                            LogError("start_module failed");
                            deinit_module(module_info);
                            singlylinkedlist_remove(broker_data->modules, moduleListItem);
                            free(module_info);
                            result = BROKER_ERROR;
                        }
                        else
                        {
                            /*Codes_SRS_BROKER_13_047: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                            result = BROKER_OK;
                        }
                    }

                    /*Codes_SRS_BROKER_13_046: [This function shall release the lock on BROKER_HANDLE_DATA::modules_lock.]*/
                    Unlock(broker_data->modules_lock);
                }
            }

        }
    }

    return result;
}



static int thread_message_control_receiver_thread_worker(void* context)
{
    THREAD_MESSAGE_HANDLING_RECEIVER* receiverContext = (THREAD_MESSAGE_HANDLING_RECEIVER*)context;
    BROKER_MODULEINFO* receiver_module_info = (BROKER_MODULEINFO*)receiverContext->module_info;
    if (Lock(receiverContext->lock) != LOCK_OK) {
        LogError("lock for receiverContext in thread_message_control_receiver_thread_worker failed");
    }
    else {
        while (true) {
            THREAD_MESSAGE_CTRL* msgCtrl = NULL;
            if (!receiverContext->toContinue)
            {
                LogInfo("thread_message_control_receiver_thread_worker to be terminated.");
                break;
            }
            
            while (msgCtrl == NULL) {
                THREAD_MESSAGE_CTRL* pre_receive_msg = NULL;
                THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER* sender = receiverContext->senders;
                while (sender != NULL) {
//                  if (Lock(sender->sender_module_info->senderThMsg->lock) != LOCK_OK) {
//                      LogError("Lock for senderThMsg in thread_message_control_receiver_thread_worker failed");
//                  }
//                  else {
                        THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* current_receiver = sender->sender_module_info->senderThMsg->receivers;
                        while (current_receiver != NULL) {
                            if (current_receiver->receiver == receiverContext) {
                                if (msgCtrl == NULL) {
                                    msgCtrl = current_receiver->sendingMessages;
                                    pre_receive_msg = msgCtrl;
                                }
                                else {
                                    pre_receive_msg->next = current_receiver->sendingMessages;
                                    pre_receive_msg = pre_receive_msg->next;
                                }
                                THREAD_MESSAGE_CTRL* current_sending_msg = pre_receive_msg;
                                while (current_sending_msg != NULL) {
                                    if (current_sending_msg->next == NULL) {
                                        pre_receive_msg = current_sending_msg;
                                    }
                                    current_sending_msg = current_sending_msg->next;
                                }
                                current_receiver->sendingMessages = NULL;
                                break;
                            }
                            current_receiver = current_receiver->next;
                        }
                        THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER* current_sender = sender;
                        sender = sender->next;
//                      if (Unlock(current_sender->sender_module_info->senderThMsg->lock) != LOCK_OK) {
//                          LogError("Unock for senderThMsg in thread_message_control_receiver_thread_worker failed");
//                      }
                    }
                    if (msgCtrl == NULL) {
                        if (!receiverContext->toContinue) {
                            LogInfo("Received stop order so that exit message loop!");
                            break;
                        }
                        if (Condition_Wait(receiverContext->condition, receiverContext->lock, 0) != COND_OK) {
                            LogError("Wait for condition for receiverContext in thread_message_control_receiver_thread_worker failed");
                        }
                    }
//                }
                THREAD_MESSAGE_CTRL* current_msg = msgCtrl;
                Unlock(receiverContext->lock);
                while (current_msg != NULL) {
                    MODULE_RECEIVE(receiver_module_info->module->module_apis)(receiver_module_info->module->module_handle, current_msg->msg);
                    ThreadAPI_Sleep(0);
                    THREAD_MESSAGE_CTRL* tmp_msg = current_msg;
                    current_msg = current_msg->next;
                    Message_Destroy(tmp_msg->msg);
                    free((void*)tmp_msg);
                }
                Lock(receiverContext->lock);
            }
        }
        LOCK_RESULT lock_result = Unlock(receiverContext->lock);
        if (lock_result != LOCK_OK) {
            LogError("Unlock for receiverContext in thread_message_control_receiver_thread_worker failed");
        }
    }
    Condition_Deinit(receiverContext->condition);
    if (Lock_Deinit(receiverContext->lock) != LOCK_OK) {
        LogError("Deinit for receiverContext in thread_message_control_receiver_thread_worker failed");
    }

    return 0;
}

BROKER_RESULT Broker_AddLink(BROKER_HANDLE broker, const BROKER_LINK_DATA* link)
{
    BROKER_RESULT result;
    /*Codes_SRS_BROKER_17_029: [ If broker or link are NULL, Broker_AddLink shall return BROKER_INVALIDARG. ]*/
    if (broker == NULL || link == NULL || link->module_sink_handle == NULL || link->module_source_handle == NULL)
    {
        LogError("Broker_AddLink, input is NULL.");
        result = BROKER_INVALIDARG;
    }
    else
    {
        BROKER_HANDLE_DATA* broker_data = (BROKER_HANDLE_DATA*)broker;
        /*Codes_SRS_BROKER_17_030: [ Broker_AddLink shall lock the modules_lock. ]*/
        if (Lock(broker_data->modules_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_17_034: [ Upon an error, Broker_AddLink shall return BROKER_ADD_LINK_ERROR ]*/
            LogError("Broker_AddLink, Lock on broker_data->modules_lock failed");
            result = BROKER_ADD_LINK_ERROR;
        }
        else
        {
            /*Codes_SRS_BROKER_17_031: [ Broker_AddLink shall find the BROKER_HANDLE_DATA::module_info for link->sink. ]*/
            BROKER_MODULEINFO* module_info = broker_locate_handle(broker_data, link->module_sink_handle);

            if (module_info == NULL)
            {
                /*Codes_SRS_BROKER_17_034: [ Upon an error, Broker_AddLink shall return BROKER_ADD_LINK_ERROR ]*/
                LogError("Link->sink is not attached to the broker");
                result = BROKER_ADD_LINK_ERROR;
            }
            else
            {
                /*Codes_SRS_BROKER_17_041: [ Broker_AddLink shall find the BROKER_HANDLE_DATA::module_info for link->module_source_handle. ]*/
                BROKER_MODULEINFO* source_module = broker_locate_handle(broker_data, link->module_source_handle);

                if (source_module == NULL)
                {
                    LogError("Link->source is not attached to the broker");
                    result = BROKER_ADD_LINK_ERROR;
                }
                else
                {
                    if (module_info->module->module_loader_type != OUTPROCESS&&source_module->module->module_loader_type != OUTPROCESS&&link->message_type == BROKER_LINK_MESSAGE_TYPE_THREAD) {
                        if (module_info->receiverThMsg == NULL) {
                            module_info->receiverThMsg = (THREAD_MESSAGE_HANDLING_RECEIVER*)malloc(sizeof(THREAD_MESSAGE_HANDLING_RECEIVER));
                            if (module_info->receiverThMsg == NULL) {
                                LogError("malloc receiverThMsg in Broker_AddLink failed.");
                                result = BROKER_ADD_LINK_ERROR;
                            }
                            else {
                                module_info->receiverThMsg->lock = Lock_Init();
                                module_info->receiverThMsg->condition = Condition_Init();
                                if (module_info->receiverThMsg->lock == NULL || module_info->receiverThMsg->condition == NULL) {
                                    LogError("create lock or condition for receiverThMsg in Broker_AddLink failed. - lock:%08x,condition:%08x", module_info->receiverThMsg->lock, module_info->receiverThMsg->condition);
                                    if (module_info->receiverThMsg->lock != NULL) {
                                        Lock_Deinit(module_info->receiverThMsg->lock);
                                    }
                                    if (module_info->receiverThMsg->condition != NULL) {
                                        Condition_Deinit(module_info->receiverThMsg->condition);
                                    }
                                    free(module_info->receiverThMsg);
                                    module_info->receiverThMsg = NULL;
                                    result = BROKER_ADD_LINK_ERROR;
                                }
                                else {
                                    module_info->receiverThMsg->toContinue = true;
                                    module_info->receiverThMsg->senders = NULL;
                                    module_info->receiverThMsg->module_info = module_info;
                                    if (module_info->receiverThMsg->lock == NULL || module_info->receiverThMsg->condition == NULL) {
                                        LogError("create lock or condition for receiverThMsg in Broker_AddLink failed. - lock:%08x,condition:%08x", module_info->receiverThMsg->lock, module_info->receiverThMsg->condition);
                                        if (module_info->receiverThMsg->lock != NULL) {
                                            Lock_Deinit(module_info->receiverThMsg->lock);
                                        }
                                        if (module_info->receiverThMsg->condition != NULL) {
                                            Condition_Deinit(module_info->receiverThMsg->condition);
                                        }
                                        free(module_info->receiverThMsg);
                                        module_info->receiverThMsg = NULL;
                                        result = BROKER_ADD_LINK_ERROR;
                                    }
                                    else {
                                        ThreadAPI_Create(&(module_info->receiverThMsg->receiver_thread), thread_message_control_receiver_thread_worker, module_info->receiverThMsg);
                                    }
                                }
                            }
                        }
                        THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER* current_sender = module_info->receiverThMsg->senders;
                        THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER* pre_sender = NULL;
                        while (current_sender != NULL) {
                            pre_sender = current_sender;
                            current_sender = current_sender->next;
                        }
                        if (current_sender == NULL) {
                            current_sender = (THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER*)malloc(sizeof(THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER));
                            if (current_sender == NULL) {
                                LogError("malloc current_sender in Broker_AddLink failed.");
                                result = BROKER_ADD_LINK_ERROR;
                            }
                            else {
                                current_sender->next = NULL;
                                current_sender->sender_module_info = source_module;
                                if (pre_sender == NULL) {
                                    module_info->receiverThMsg->senders = current_sender;
                                }
                                else {
                                    pre_sender->next = current_sender;
                                }
                            }
                        }
                        if (source_module->senderThMsg == NULL) {
                            source_module->senderThMsg = (THREAD_MESSAGE_HANDLING_SENDER*)malloc(sizeof(THREAD_MESSAGE_HANDLING_SENDER));
                            if (source_module->senderThMsg == NULL) {
                                LogError("malloc senderThMsg in Broker_AddLink failed.");
                                result = BROKER_ADD_LINK_ERROR;
                            }
                            else {
                                source_module->senderThMsg->lock = Lock_Init();
                                if (source_module->senderThMsg->lock == NULL) {
                                    LogError("");
                                    free(source_module->senderThMsg);
                                    source_module->senderThMsg = NULL;
                                    result = BROKER_ADD_LINK_ERROR;
                                }
                                else {
                                    source_module->senderThMsg->receivers = NULL;
                                }
                            }
                        }
                        if (source_module->senderThMsg != NULL) {
                            THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* current_receiver = source_module->senderThMsg->receivers;
                            THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* pre_receiver = NULL;
                            while (current_receiver != NULL) {
                                pre_receiver = current_receiver;
                                current_receiver = current_receiver->next;
                            }
                            if (current_receiver == NULL) {
                                current_receiver = (THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER*)malloc(sizeof(THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER));
                                if (current_receiver == NULL) {
                                    LogError("malloc current_receiver in Broker_AddLink failed.");
                                    result = BROKER_ADD_LINK_ERROR;
                                }
                                else {
                                    current_receiver->next = NULL;
                                    current_receiver->receiver = module_info->receiverThMsg;
                                    current_receiver->sendingMessages = NULL;
                                    if (pre_receiver == NULL) {
                                        source_module->senderThMsg->receivers = current_receiver;
                                    }
                                    else {
                                        pre_receiver->next = current_receiver;
                                    }
                                    result = BROKER_OK;
                                }
                            }
                        }
                    }
                    else {
                        // in the case of out process module then should be done next step!! TODO:
                        /*Codes_SRS_BROKER_17_032: [ Broker_AddLink shall subscribe module_info->receive_socket to the link->source module handle. ]*/
                        if (nn_setsockopt(
                            module_info->receive_socket, NN_SUB, NN_SUB_SUBSCRIBE, &(link->module_source_handle), sizeof(MODULE_HANDLE)) < 0)
                        {
                            /*Codes_SRS_BROKER_17_034: [ Upon an error, Broker_AddLink shall return BROKER_ADD_LINK_ERROR ]*/
                            LogError("Unable to make link in Broker");
                            result = BROKER_ADD_LINK_ERROR;
                        }
                        else
                        {
                            result = BROKER_OK;
                        }
                    }
                }
            }
            /*Codes_SRS_BROKER_17_033: [ Broker_AddLink shall unlock the modules_lock. ]*/
            Unlock(broker_data->modules_lock);
        }
    }
    return result;
}

BROKER_RESULT Broker_RemoveLink(BROKER_HANDLE broker, const BROKER_LINK_DATA* link)
{
    BROKER_RESULT result;
    /*Codes_SRS_BROKER_17_035: [ If broker, link, link->module_source_handle or link->module_sink_handle are NULL, Broker_RemoveLink shall return BROKER_INVALIDARG. ]*/
    if (broker == NULL || link == NULL || link->module_sink_handle == NULL || link->module_source_handle == NULL)
    {
        LogError("Broker_AddLink, input is NULL.");
        result = BROKER_INVALIDARG;
    }
    else
    {
        BROKER_HANDLE_DATA* broker_data = (BROKER_HANDLE_DATA*)broker;
        /*Codes_SRS_BROKER_17_036: [ Broker_RemoveLink shall lock the modules_lock. ]*/
        if (Lock(broker_data->modules_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_17_040: [ Upon an error, Broker_RemoveLink shall return BROKER_REMOVE_LINK_ERROR. ]*/
            LogError("Broker_AddLink, Lock on broker_data->modules_lock failed");
            result = BROKER_REMOVE_LINK_ERROR;
        }
        else
        {
            /*Codes_SRS_BROKER_17_037: [ Broker_RemoveLink shall find the module_info for link->module_sink_handle. ]*/
            BROKER_MODULEINFO* module_info = broker_locate_handle(broker_data, link->module_sink_handle);

            if (module_info == NULL)
            {
                /*Codes_SRS_BROKER_17_040: [ Upon an error, Broker_RemoveLink shall return BROKER_REMOVE_LINK_ERROR. ]*/
                LogError("Link->sink is not attached to the broker");
                result = BROKER_REMOVE_LINK_ERROR;
            }
            else
            {
                /*Codes_SRS_BROKER_17_042: [ Broker_RemoveLink shall find the module_info for link->module_source_handle. ]*/
                BROKER_MODULEINFO* source_module_info = broker_locate_handle(broker_data, link->module_source_handle);
                if (source_module_info == NULL)
                {
                    LogError("Link->source is not attached to the broker");
                    result = BROKER_REMOVE_LINK_ERROR;
                }
                else
                {
                    /*Codes_SRS_BROKER_17_038: [ Broker_RemoveLink shall unsubscribe module_info->receive_socket from the link->module_source_handle module handle. ]*/
//                    if (nn_setsockopt(
//                      module_info->receive_socket, NN_SUB, NN_SUB_UNSUBSCRIBE, &(link->module_source_handle), sizeof(MODULE_HANDLE)) < 0)
//                  {
//                      /*Codes_SRS_BROKER_17_040: [ Upon an error, Broker_RemoveLink shall return BROKER_REMOVE_LINK_ERROR. ]*/
//                      LogError("Unable to make link in Broker");
//                      result = BROKER_REMOVE_LINK_ERROR;
//                  }
//                  else
//                  {
//                      result = BROKER_OK;
//                  }
                    if (module_info->receiverThMsg != NULL&&source_module_info->senderThMsg != NULL) {
                        if (Lock(module_info->receiverThMsg->lock) != LOCK_OK) {
                            LogError("Lock for receiverThMsg failed.");
                        }
                        else {
                            THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER* sender = module_info->receiverThMsg->senders;
                            THREAD_MESSAGE_HANDLING_SENDER_FOR_RECEIVER* pre_sender = NULL;
                            while (sender != NULL) {
                                if (sender->sender_module_info == source_module_info) {
                                    if (pre_sender == NULL) {
                                        module_info->receiverThMsg->senders = sender->next;
                                    }
                                    else {
                                        pre_sender->next = sender->next;
                                    }
                                    break;
                                }
                                pre_sender = sender;
                                sender = sender->next;
                            }
                            Unlock(module_info->receiverThMsg->lock);

                            if (Lock(source_module_info->senderThMsg->lock) != LOCK_OK) {
                                LogError("Lock senderThMsg failed.");
                            }
                            else {
                                THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* receiver = source_module_info->senderThMsg->receivers;
                                THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* pre_receiver = NULL;
                                while (receiver != NULL)
                                {
                                    if (receiver->receiver->module_info == module_info) {
                                        if (pre_receiver != NULL) {
                                            pre_receiver->next = receiver->next;
                                        }
                                        else {
                                            source_module_info->senderThMsg->receivers = receiver->next;
                                        }
                                        break;
                                    }
                                    pre_receiver = receiver;
                                    receiver = receiver->next;
                                }
                                if (receiver != NULL&&sender != NULL) {
                                    free((void*)sender);

                                    THREAD_MESSAGE_CTRL* sendingMsg = receiver->sendingMessages;
                                    while (sendingMsg != NULL) {
                                        THREAD_MESSAGE_CTRL* next = sendingMsg->next;
                                        Message_Destroy(sendingMsg->msg);
                                        free((void*)sendingMsg);
                                        sendingMsg = next;
                                    }
                                    free((void*)receiver);
                                    result = BROKER_OK;
                                }
                                else {
                                    // may be error but shoudn't be system error
                                    result = BROKER_REMOVE_LINK_ERROR;
                                }
                                Unlock(source_module_info->senderThMsg->lock);
                            }
                        }
                    }
                    else {
                        // error
                    }
                }
            }
            /*Codes_SRS_BROKER_17_039: [ Broker_RemoveLink shall unlock the modules_lock. ]*/
            Unlock(broker_data->modules_lock);
        }
    }
    return result;
}

static void broker_decrement_ref(BROKER_HANDLE broker)
{
    /*Codes_SRS_BROKER_13_058: [If `broker` is NULL the function shall do nothing.]*/
    if (broker != NULL)
    {
        /*Codes_SRS_BROKER_13_111: [Otherwise, Broker_Destroy shall decrement the internal ref count of the message.]*/
        /*Codes_SRS_BROKER_13_112: [If the ref count is zero then the allocated resources are freed.]*/
        if (DEC_REF(BROKER_HANDLE_DATA, broker) == DEC_RETURN_ZERO)
        {
            BROKER_HANDLE_DATA* broker_data = (BROKER_HANDLE_DATA*)broker; 
            if (singlylinkedlist_get_head_item(broker_data->modules) != NULL)
            {
                LogError("WARNING: There are still active modules attached to the broker and the broker is being destroyed.");
            }
            /* May want to do nn_shutdown first for cleanliness. */
            nn_close(broker_data->publish_socket);
            STRING_delete(broker_data->url);
            singlylinkedlist_destroy(broker_data->modules);
            Lock_Deinit(broker_data->modules_lock);
            free(broker_data);
        }
    }
    else
    {
        LogError("broker handle is NULL");
    }
}

extern void Broker_Destroy(BROKER_HANDLE broker)
{
    broker_decrement_ref(broker);
}

extern void Broker_DecRef(BROKER_HANDLE broker)
{
    /*Codes_SRS_BROKER_13_113: [This function shall implement all the requirements of the Broker_Destroy API.]*/
    broker_decrement_ref(broker);
}

BROKER_RESULT Broker_Publish(BROKER_HANDLE broker, MODULE_HANDLE source, MESSAGE_HANDLE message)
{
    BROKER_RESULT result = BROKER_OK;
    /*Codes_SRS_BROKER_13_030: [If broker or message is NULL the function shall return BROKER_INVALIDARG.]*/
    if (broker == NULL || source == NULL || message == NULL)
    {
        result = BROKER_INVALIDARG;
        LogError("Broker handle, source, and/or message handle is NULL");
        return result;
    }
    else
    {
        BROKER_HANDLE_DATA* broker_data = (BROKER_HANDLE_DATA*)broker;
        /*Codes_SRS_BROKER_17_022: [ Broker_Publish shall Lock the modules lock. ]*/
        if (Lock(broker_data->modules_lock) != LOCK_OK)
        {
            /*Codes_SRS_BROKER_13_053: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
            LogError("Lock on broker_data->modules_lock failed");
            result = BROKER_ERROR;
            return result;
        }
        else
        {
            BROKER_MODULEINFO* source_info = broker_locate_handle(broker_data, source);
            if (source_info == NULL) {
                LogError("Can't find BROKER_MODULEINFO");
                result = BROKER_ERROR;
                return result;
            }
            bool normalMessaging = true;
            if (source_info->senderThMsg != NULL) {
                // TODO: current version is not support nanomsg and thread messaging.
                if (Lock(source_info->senderThMsg->lock) != LOCK_OK) {
                    LogError("Lock senderThMsg in Broker_Publish failed.");
                }
                else {
                    THREAD_MESSAGE_HANDLING_RECIEVERS_IN_SENDER* target_receiver = source_info->senderThMsg->receivers;
                    while (target_receiver != NULL) {
                        THREAD_MESSAGE_CTRL* current_msg = (THREAD_MESSAGE_CTRL*)malloc(sizeof(THREAD_MESSAGE_CTRL));
                        if (current_msg == NULL) {
                            LogError("malloc current_msg in Broker_Publish failed.");
                        }
                        else {
                            current_msg->next = NULL;
                            current_msg->msg = Message_Clone(message);
                            if (current_msg->msg == NULL) {
                                LogError("clone message in Broker_Publish failed.");
                                free(current_msg);
                            }
                            else {
                                if (Lock(target_receiver->receiver->lock) != LOCK_OK) {
                                    LogError("Lock receiver in Broker_Publish failed.");
                                }
                                else {
                                    THREAD_MESSAGE_CTRL* last_msg = target_receiver->sendingMessages;
                                    if (last_msg == NULL) {
                                        target_receiver->sendingMessages = current_msg;
                                    }
                                    else {
                                        while (last_msg->next != NULL)
                                        {
                                            last_msg = last_msg->next;
                                        }
                                        last_msg->next = current_msg;
                                    }
                                    Unlock(source_info->senderThMsg->lock);
                                    Condition_Post(target_receiver->receiver->condition);
                                    Unlock(target_receiver->receiver->lock);
                                }
                                if (Lock(source_info->senderThMsg->lock) != LOCK_OK) {
                                    LogError("Lock senderThMsg in Broker_Publish failed.");
                                }
                                else {
                                    target_receiver = target_receiver->next;
                                }
                            }
                        }
                    }
                    if (Unlock(source_info->senderThMsg->lock) != LOCK_OK) {
                        LogError("unlock senderThMsg in Broker_Publish failed.");
                    }
                }
                normalMessaging = false;
                result = BROKER_OK;
            }

            if (normalMessaging) {
                int32_t msg_size;
                /*Codes_SRS_BROKER_17_007: [ Broker_Publish shall clone the message. ]*/
                MESSAGE_HANDLE msg = Message_Clone(message);
                /*Codes_SRS_BROKER_17_008: [ Broker_Publish shall serialize the message. ]*/
                msg_size = Message_ToByteArray(message, NULL, 0);
                if (msg_size < 0)
                {
                    /*Codes_SRS_BROKER_13_053: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                    LogError("unable to serialize a message [%p]", msg);
                    Message_Destroy(msg);
                    result = BROKER_ERROR;
                }
                else
                {
                    time_t current = time(NULL);

                        int32_t buf_size;
                        /*Codes_SRS_BROKER_17_025: [ Broker_Publish shall allocate a nanomsg buffer the size of the serialized message + sizeof(MODULE_HANDLE). ]*/
                        buf_size = msg_size + sizeof(MODULE_HANDLE) + sizeof(int32_t);
                            buf_size += sizeof(int32_t);
                        void* nn_msg = nn_allocmsg(buf_size, 0);
                        if (nn_msg == NULL)
                        {
                            /*Codes_SRS_BROKER_13_053: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                            LogError("unable to serialize a message [%p]", msg);
                            result = BROKER_ERROR;
                        }
                        else
                        {
                            /*Codes_SRS_BROKER_17_026: [ Broker_Publish shall copy source into the beginning of the nanomsg buffer. ]*/
                            unsigned char *nn_msg_bytes = (unsigned char *)nn_msg;
                            memcpy(nn_msg_bytes, &source, sizeof(MODULE_HANDLE));
                            /*Codes_SRS_BROKER_17_027: [ Broker_Publish shall serialize the message into the remainder of the nanomsg buffer. ]*/
                            nn_msg_bytes += sizeof(MODULE_HANDLE);
                            nn_msg_bytes += sizeof(int32_t);
                                *((int32_t*)nn_msg_bytes) = msg_size;
                                nn_msg_bytes += sizeof(int32_t);
                                Message_ToByteArray(message, nn_msg_bytes, msg_size);
                                result = BROKER_OK;

                            /*Codes_SRS_BROKER_17_010: [ Broker_Publish shall send a message on the publish_socket. ]*/
                            int nbytes = nn_send(broker_data->publish_socket, &nn_msg, NN_MSG, 0);
                            if (nbytes != buf_size)
                            {
                                /*Codes_SRS_BROKER_13_053: [This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise.]*/
                                LogError("unable to send a message [%p]", msg);
                                /*Codes_SRS_BROKER_17_012: [ Broker_Publish shall free the message. ]*/
                                nn_freemsg(nn_msg);
                                result = BROKER_ERROR;
                            }
                        }
                    /*Codes_SRS_BROKER_17_012: [ Broker_Publish shall free the message. ]*/
                    Message_Destroy(msg);
                    /*Codes_SRS_BROKER_17_011: [ Broker_Publish shall free the serialized message data. ]*/
                }

            }
            /*Codes_SRS_BROKER_17_023: [ Broker_Publish shall Unlock the modules lock. ]*/
            Unlock(broker_data->modules_lock);
        }
    }
    /*Codes_SRS_BROKER_13_037: [ This function shall return BROKER_ERROR if an underlying API call to the platform causes an error or BROKER_OK otherwise. ]*/
    return result;
}

