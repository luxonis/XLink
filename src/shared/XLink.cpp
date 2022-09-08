#include <XLink/XLink.h>

// std
#include <mutex>

// project
#include <XLink/XLinkPrivateDefines.h>



typedef enum {
    EVENT_ALLOCATED,
    EVENT_PENDING,
    EVENT_BLOCKED,
    EVENT_READY,
    EVENT_SERVED,
    EVENT_DROPPED,
} xLinkEventState_t;

typedef struct xLinkEventPriv_t {
    xLinkEvent_t packet;
    xLinkEvent_t *retEv;
    xLinkEventState_t isServed;
    xLinkEventOrigin_t origin;
    XLink_sem_t* sem;
    void* data;
} xLinkEventPriv_t;

typedef struct {
    XLink_sem_t sem;
    pthread_t threadId;
} localSem_t;

typedef struct{
    xLinkEventPriv_t* end;
    xLinkEventPriv_t* base;

    xLinkEventPriv_t* curProc;
    xLinkEventPriv_t* cur;
    XLINK_ALIGN_TO_BOUNDARY(64) xLinkEventPriv_t q[MAX_EVENTS];

}eventQueueHandler_t;

typedef struct {
    xLinkDeviceHandle_t deviceHandle; //will be device handler
    int schedulerId;

    int queueProcPriority;

    pthread_mutex_t queueMutex;

    XLink_sem_t addEventSem;
    XLink_sem_t notifyDispatcherSem;
    volatile uint32_t resetXLink;
    uint32_t semaphores;
    pthread_t xLinkThreadId;

    eventQueueHandler_t lQueue; //local queue
    eventQueueHandler_t rQueue; //remote queue
    localSem_t eventSemaphores[MAXIMUM_SEMAPHORES];

    uint32_t dispatcherLinkDown;
    uint32_t dispatcherDeviceFdDown;
    uint32_t server;
} xLinkSchedulerState_t;



class XLink {
    // private constructor
    XLink();
    ~XLink();

    std::mutex accessMtx;

public:
    static XLink& getInstance();
    XLink(Resources const&) = delete;
    void operator=(XLink const&) = delete;


    struct Dispatcher {

        typedef struct {
            int (*eventSend) (xLinkEvent_t*);
            int (*eventReceive) (xLinkEvent_t*);
            getRespFunction localGetResponse;
            getRespFunction remoteGetResponse;
            void (*closeLink) (void* fd, int fullClose);
            void (*closeDeviceFd) (xLinkDeviceHandle_t* deviceHandle);
        } ControlFunctions;

        XLinkError_t Initialize(DispatcherControlFunctions *controlFunc);

    private:
        ControlFunctions controlFunctionTbl;
        xLinkSchedulerState_t schedulerState[MAX_SCHEDULERS];
        int numSchedulers;

    };

    Dispatcher dispatcher;


    struct xLinkDesc_t {
        // Incremental number, doesn't get decremented.
        uint32_t nextUniqueStreamId;
        streamDesc_t availableStreams[XLINK_MAX_STREAMS];
        xLinkState_t peerState;
        xLinkDeviceHandle_t deviceHandle;
        linkId_t id;
        XLink_sem_t dispatcherClosedSem;
        UsbSpeed_t usbConnSpeed;
        char mxSerialId[XLINK_MAX_MX_ID_SIZE];
    };


    std::vector<xLinkDesc_t> availableLinks;

    void setControlFunctionTable(DispatcherControlFunctions functions) {
        std::unique_lock<std::mutex> lock(accessMtx);
        controlFunctionTbl = functions;
    }

};

XLinkError_t XLinkInitialize(XLinkGlobalHandler_t* globalHandler)
{

    static const XLinkError_t initialized = [&]() {

        xLinkPlatformErrorCode_t init_status = XLinkPlatformInit(globalHandler);

        if (init_status != X_LINK_PLATFORM_SUCCESS) {
            return parsePlatformError(init_status);
        }

        XLink::DispatcherControlFunctions controlFunctionTbl;
        controlFunctionTbl.eventReceive      = &dispatcherEventReceive;
        controlFunctionTbl.eventSend         = &dispatcherEventSend;
        controlFunctionTbl.localGetResponse  = &dispatcherLocalEventGetResponse;
        controlFunctionTbl.remoteGetResponse = &dispatcherRemoteEventGetResponse;
        controlFunctionTbl.closeLink         = &dispatcherCloseLink;
        controlFunctionTbl.closeDeviceFd     = &dispatcherCloseDeviceFd;

        XLink& xlink = XLink::getInstance();
        if (xlink.dispatcher.Initialize(&controlFunctionTbl)) {
            mvLog(MVLOG_ERROR, "Condition failed: DispatcherInitialize(&controlFunctionTbl)");
            return X_LINK_ERROR;
        }


        //initialize availableStreams
        memset(availableXLinks, 0, sizeof(availableXLinks));

        xLinkDesc_t* link;
        for (i = 0; i < MAX_LINKS; i++) {
            link = &availableXLinks[i];

            link->id = INVALID_LINK_ID;
            link->deviceHandle.xLinkFD = NULL;
            link->peerState = XLINK_NOT_INIT;
            int stream;
            for (stream = 0; stream < XLINK_MAX_STREAMS; stream++)
                link->availableStreams[stream].id = INVALID_STREAM_ID;
        }



        return X_LINK_SUCCESS;
    }();

    return initialized;
}



XLinkError_t XLink::Dispatcher::Initialize(DispatcherControlFunctions *controlFunc) {
    ASSERT_XLINK(controlFunc != NULL);

    if (!controlFunc->eventReceive ||
        !controlFunc->eventSend ||
        !controlFunc->localGetResponse ||
        !controlFunc->remoteGetResponse) {
        return X_LINK_ERROR;
    }

    controlFunctionTbl = *controlFunc;

    if (sem_init(&addSchedulerSem, 0, 1)) {
        mvLog(MVLOG_ERROR, "Can't create semaphore\n");
        return X_LINK_ERROR;
    }

    for (int i = 0; i < MAX_SCHEDULERS; i++){
        schedulerState[i].schedulerId = -1;
    }

    return X_LINK_SUCCESS;
}







