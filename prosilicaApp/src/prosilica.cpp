/* prosilica.cpp
 *
 * This is a driver for Prosilica (now AVT) cameras (GigE and CameraLink).
 *
 * Author: Mark Rivers
 *         University of Chicago
 *
 * Created:  March 20, 2008
 *
 */
 
#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef linux
#include <readline/readline.h>
#endif

#include <ellLib.h>
#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsString.h>
#include <epicsStdio.h>
#include <epicsMutex.h>
#include <cantProceed.h>
#include <osiSock.h>
#include <iocsh.h>
#include <epicsExit.h>

#include "PvApi.h"

#include "ADDriver.h"

#include <epicsExport.h>

#define DRIVER_VERSION      2
#define DRIVER_REVISION     5
#define DRIVER_MODIFICATION 0

static const char *driverName = "prosilica";

static int PvApiInitialized;

static ELLLIST *cameraList;

#define MAX_PVAPI_FRAMES  2  /**< Number of frame buffers for PvApi */
#define MAX_PACKET_SIZE 8228

#define CONNECT_RETRY_COUNT    30 /* Number of times to retry connecting */
#define CONNECT_RETRY_INTERVAL  1 /* Time to sleep between trying to connect */

/** Driver for Prosilica GigE and CameraLink cameras using their PvApi library */
class prosilica : public ADDriver {
public:
    /* Constructor and Destructor */
    prosilica(const char *portName, const char *cameraId, int maxBuffers, size_t maxMemory,
              int priority, int stackSize, int maxPvAPIFrames);
    ~prosilica();

    /* These methods are overwritten from asynPortDriver */
    virtual asynStatus connect(asynUser* pasynUser);
    virtual asynStatus disconnect(asynUser* pasynUser);

    /* These are the methods that we override from ADDriver */
    virtual asynStatus writeInt32(asynUser *pasynUser, epicsInt32 value);
    virtual asynStatus writeFloat64(asynUser *pasynUser, epicsFloat64 value);
    void report(FILE *fp, int details);
    
    /* These are called from C and so must be public */
     /* This is called by the AVT driver when the connection status of a camera changes */
    static void PVDECL cameraLinkCallback(void* Context, tPvInterface Interface, 
                                          tPvLinkEvent Event, unsigned long UniqueId);
    void frameCallback(tPvFrame *pFrame);
    /* Removes the PvAPI callback functions and disconnects the camera */
    static void shutdown(void *arg);

 
protected:
    int PSReadStatistics;
    #define FIRST_PS_PARAM PSReadStatistics
    int PSBayerConvert;
    int PSGainMode;
    int PSExposureMode;
    int PSDriverType;
    int PSFilterVersion;
    int PSTimestampType;
    int PSResetTimer;
    int PSFrameRate;
    int PSByteRate;
    int PSPacketSize;
    int PSFramesCompleted;
    int PSFramesDropped;
    int PSPacketsErroneous;
    int PSPacketsMissed;
    int PSPacketsReceived;
    int PSPacketsRequested;
    int PSPacketsResent;
    int PSBadFrameCounter;
    int PSTriggerDelay;
    int PSTriggerEvent;
    int PSTriggerOverlap;
    int PSTriggerSoftware;
    int PSSyncIn1Level;
    int PSSyncIn2Level;
    int PSSyncOut1Mode;
    int PSSyncOut1Level;
    int PSSyncOut1Invert;
    int PSSyncOut2Mode;
    int PSSyncOut2Level;
    int PSSyncOut2Invert;
    int PSSyncOut3Mode;
    int PSSyncOut3Level;
    int PSSyncOut3Invert;
    int PSStrobe1Mode;
    int PSStrobe1Delay;
    int PSStrobe1CtlDuration;
    int PSStrobe1Duration;
    #define LAST_PS_PARAM PSStrobe1Duration
private:                                        
    /* These are the methods that are new to this class */
    asynStatus setPixelFormat();
    asynStatus setGeometry();
    asynStatus getGeometry();
    asynStatus readStats();
    asynStatus readParameters();
    asynStatus disconnectCamera();
    asynStatus connectCamera();
    asynStatus syncTimer();
    
    /* These items are specific to the Prosilica driver */
    tPvHandle PvHandle;            /* GenericPointer for the Prosilica PvAPI library */
    char *cameraId;                /* This can be a uniqueID, IP name, or IP address */
    unsigned long uniqueIP;
    unsigned long uniqueId;
    tPvCameraInfoEx PvCameraInfo;
    tPvFrame *PvFrames;
    int maxFrameSize;
    int maxPvAPIFrames_;
    int framesRemaining;
    char sensorType[20];
    char IPAddress[50];
    tPvUint32 sensorBits;
    tPvUint32 sensorWidth;
    tPvUint32 sensorHeight;
    tPvUint32 timeStampFrequency;
    struct epicsTimeStamp lastSyncTime;
};

typedef struct {
    ELLNODE node;
    prosilica *pCamera;
} cameraNode;

#define NUM_PS_PARAMS ((int)(&LAST_PS_PARAM - &FIRST_PS_PARAM + 1))
typedef enum {
    /* These parameters describe the trigger modes of the Prosilica
     * They must agree with the values in the mbbo/mbbi records in
     * the Prosilca database. */
    PSTriggerStartFreeRun,
    PSTriggerStartSyncIn1,
    PSTriggerStartSyncIn2,
    PSTriggerStartSyncIn3,
    PSTriggerStartSyncIn4,
    PSTriggerStartFixedRate,
    PSTriggerStartSoftware
} PSTriggerStartMode_t;


/* These describe the contents of the NDArray timeStamp parameter */
typedef enum {
    PSTimestampTypeNativeTicks,
    // The number of internal camera clock ticks which have elapsed 
    // since the last timer reset
    PSTimestampTypeNativeSeconds, 
    // The number of seconds which have elapsed since the last timer reset
    PSTimestampTypePOSIX,         
    // The number of seconds since the POSIX Epoch (00:00:00 UTC, January 1, 1970)
    PSTimestampTypeEPICS,         
    // The number of seconds since the EPICS Epoch (January 1, 1990)
    PSTimestampTypeIOC
    // Use the IOC clock to sync timeStamp and driver timestamps
} PSTimestampType_t;


typedef enum {
    PSBayerConvertNone,
    PSBayerConvertRGB1,
    PSBayerConvertRGB2,
    PSBayerConvertRGB3,
} PSBayerConvert_t;

static const char *PSTriggerStartModes[] = {
    "Freerun",
    "SyncIn1",
    "SyncIn2",
    "SyncIn3",
    "SyncIn4",
    "FixedRate",
    "Software"
};
#define NUM_TRIGGER_START_MODES (int)(sizeof(PSTriggerStartModes) / sizeof(PSTriggerStartModes[0]))

static const char *PSTriggerEventModes[] = {
    "EdgeRising",
    "EdgeFalling",
    "EdgeAny",
    "LevelHigh",
    "LevelLow"
};
#define NUM_TRIGGER_EVENT_MODES (int)(sizeof(PSTriggerEventModes) / sizeof(PSTriggerEventModes[0]))

static const char *PSTriggerOverlapModes[] = {
    "Off",
    "PreviousFrame"
};
#define NUM_TRIGGER_OVERLAP_MODES (int)(sizeof(PSTriggerOverlapModes) / sizeof(PSTriggerOverlapModes[0]))

static const char *PSSyncOutModes[] = {
    "GPO",
    "AcquisitionTriggerReady",
    "FrameTriggerReady",
    "FrameTrigger",
    "Exposing",
    "FrameReadout",
    "Imaging",
    "Acquiring",
    "SyncIn1",
    "SyncIn2",
    "SyncIn3",
    "SyncIn4",
    "Strobe1",
    "Strobe2",
    "Strobe3",
    "Strobe4"
};
#define NUM_SYNC_OUT_MODES (int)(sizeof(PSSyncOutModes) / sizeof(PSSyncOutModes[0]))

static const char *PSStrobeModes[] = {
    "AcquisitionTriggerReady",
    "FrameTriggerReady",
    "FrameTrigger",
    "Exposing",
    "FrameReadout",
    "Acquiring",
    "SyncIn1",
    "SyncIn2",
    "SyncIn3",
    "SyncIn4",
};
#define NUM_STROBE_MODES (int)(sizeof(PSStrobeModes) / sizeof(PSStrobeModes[0]))

static const char *PSExposureModes[] = {
    "Manual",
    "AutoOnce",
    "Auto",
    "External",
};
#define NUM_EXPOSURE_MODES (int)(sizeof(PSExposureModes) / sizeof(PSExposureModes[0]))

static const char *PSGainModes[] = {
    "Manual",
    "AutoOnce",
    "Auto",
};
#define NUM_GAIN_MODES (int)(sizeof(PSGainModes) / sizeof(PSGainModes[0]))

/** Driver-specific parameters for the Prosilica driver */
    /*                                       String              asyn interface  access   Description  */
#define PSReadStatisticsString       "PS_READ_STATISTICS"      /* (asynInt32,    r/w) Write to read statistics  */ 
#define PSBayerConvertString         "PS_BAYER_CONVERT"        /* (asynInt32,    r/w) Convert Bayer to another format */ 
#define PSGainModeString             "PS_GAIN_MODE"            /* (asynInt32,    r/w) Camera gain mode, manual or auto */
#define PSExposureModeString         "PS_EXPOSURE_MODE"        /* (asynInt32,    r/w) Camera exposure mode, manual or auto */
#define PSDriverTypeString           "PS_DRIVER_TYPE"          /* (asynOctet,    r/o) Ethernet driver type */ 
#define PSFilterVersionString        "PS_FILTER_VERSION"       /* (asynOctet,    r/o) Ethernet packet filter version */ 
#define PSTimestampTypeString        "PS_TIMESTAMP_TYPE"       /* (asynInt32,    r/w) Choose how the timestamping is performed */
#define PSResetTimerString           "PS_RESET_TIMER"          /* (asynInt32,    n/a) Software timer reset/sync */
#define PSFrameRateString            "PS_FRAME_RATE"           /* (asynFloat64,  r/o) Frame rate */ 
#define PSByteRateString             "PS_BYTE_RATE"            /* (asynInt32,    r/w) Stream bytes per second */ 
#define PSPacketSizeString           "PS_PACKET_SIZE"          /* (asynInt32,    r/o) Packet size */ 
#define PSFramesCompletedString      "PS_FRAMES_COMPLETED"     /* (asynInt32,    r/o) Frames completed */ 
#define PSFramesDroppedString        "PS_FRAMES_DROPPED"       /* (asynInt32,    r/o) Frames dropped */ 
#define PSPacketsErroneousString     "PS_PACKETS_ERRONEOUS"    /* (asynInt32,    r/o) Erroneous packets */ 
#define PSPacketsMissedString        "PS_PACKETS_MISSED"       /* (asynInt32,    r/o) Missed packets */ 
#define PSPacketsReceivedString      "PS_PACKETS_RECEIVED"     /* (asynInt32,    r/o) Packets received */ 
#define PSPacketsRequestedString     "PS_PACKETS_REQUESTED"    /* (asynInt32,    r/o) Packets requested */ 
#define PSPacketsResentString        "PS_PACKETS_RESENT"       /* (asynInt32,    r/o) Packets resent */
#define PSBadFrameCounterString      "PS_BAD_FRAME_COUNTER"    /* (asynInt32,    r/o) Bad frame counter */
#define PSTriggerDelayString         "PS_TRIGGER_DELAY"        /* (asynFloat64,  r/w) Frame start trigger delay */
#define PSTriggerEventString         "PS_TRIGGER_EVENT"        /* (asynInt32,    r/w) Frame start trigger event */
#define PSTriggerOverlapString       "PS_TRIGGER_OVERLAP"      /* (asynInt32,    r/w) Frame start trigger overlap */
#define PSTriggerSoftwareString      "PS_TRIGGER_SOFTWARE"     /* (asynInt32  ,  r/w) Frame start trigger software*/
#define PSSyncIn1LevelString         "PS_SYNC_IN_1_LEVEL"      /* (asynInt32,    r/o) Sync input 1 level */
#define PSSyncIn2LevelString         "PS_SYNC_IN_2_LEVEL"      /* (asynInt32,    r/o) Sync input 2 level */
#define PSSyncOut1ModeString         "PS_SYNC_OUT_1_MODE"      /* (asynInt32,    r/w) Sync output 1 mode */
#define PSSyncOut1LevelString        "PS_SYNC_OUT_1_LEVEL"     /* (asynInt32,    r/w) Sync output 1 level */
#define PSSyncOut1InvertString       "PS_SYNC_OUT_1_INVERT"    /* (asynInt32,    r/w) Sync output 1 invert */
#define PSSyncOut2ModeString         "PS_SYNC_OUT_2_MODE"      /* (asynInt32,    r/w) Sync output 2 mode */
#define PSSyncOut2LevelString        "PS_SYNC_OUT_2_LEVEL"     /* (asynInt32,    r/w) Sync output 2 level */
#define PSSyncOut2InvertString       "PS_SYNC_OUT_2_INVERT"    /* (asynInt32,    r/w) Sync output 2 invert */
#define PSSyncOut3ModeString         "PS_SYNC_OUT_3_MODE"      /* (asynInt32,    r/w) Sync output 3 mode */
#define PSSyncOut3LevelString        "PS_SYNC_OUT_3_LEVEL"     /* (asynInt32,    r/w) Sync output 3 level */
#define PSSyncOut3InvertString       "PS_SYNC_OUT_3_INVERT"    /* (asynInt32,    r/w) Sync output 3 invert */
#define PSStrobe1ModeString          "PS_STROBE_1_MODE"        /* (asynInt32,    r/w) Strobe 1 mode */
#define PSStrobe1DelayString         "PS_STROBE_1_DELAY"       /* (asynFloat64,  r/w) Strobe 1 delay */
#define PSStrobe1CtlDurationString   "PS_STROBE_1_CTL_DURATION"/* (asynInt32,    r/w) Strobe 1 controlled duration */
#define PSStrobe1DurationString      "PS_STROBE_1_DURATION"    /* (asynFloat64,  r/w) Strobe 1 duration */

void prosilica::shutdown (void* arg) {

    prosilica *p = (prosilica*)arg;
    if (p) delete p;
}


prosilica::~prosilica() {

    int status;
    cameraNode *pNode = (cameraNode *)ellFirst(cameraList);
    static const char *functionName = "~prosilica";

    this->lock();
    printf("Disconnecting camera %s\n", this->portName);
    disconnectCamera();
    this->unlock();

    // Find this camera in the list:
    while (pNode) {
        if (pNode->pCamera == this) break;
        pNode = (cameraNode *)ellNext(&pNode->node);
    }
    if (pNode) {
        ellDelete(cameraList, (ELLNODE *)pNode);
        delete pNode;
    }

    //  If this is the last camera in the IOC then unregister callbacks and uninitialize
    if (ellCount(cameraList) == 0) {
       status = PvLinkCallbackUnRegister(cameraLinkCallback, ePvLinkAdd);
       if (status) {
           asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
               "%s:%s: error calling PvLinkCallbackUnRegister for ePvLinkAdd, status=%d\n",
               driverName, functionName, status);
       }
       status = PvLinkCallbackUnRegister(cameraLinkCallback, ePvLinkRemove);
       if (status) {
           asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR,
               "%s:%s: error calling PvLinkCallbackUnRegister for ePvLinkRemove, status=%d\n",
               driverName, functionName, status);
       }

       if (PvApiInitialized) {
           printf("Uninitializing PvAPI\n");
           PvUnInitialize();
           PvApiInitialized = false;
       }
       delete cameraList;
   }
}



// Changes the connection status of the camera based on information from the AVT library
void PVDECL  prosilica::cameraLinkCallback(void *Context, tPvInterface Interface, tPvLinkEvent Event, unsigned long UniqueId ) {
    asynStatus status = asynSuccess;
    int found=0;
    unsigned long uniqueIP=0;
    prosilica *pDriver;
    cameraNode *pNode = (cameraNode *)ellFirst(cameraList);
    //static const char *functionName = "cameraLinkCallback";
    
    while (pNode) {;
        pDriver = pNode->pCamera;
        pDriver->lock();
        switch (Event) {
            case ePvLinkAdd:
                // We need to check to see if the UniqueId matches ours. If the camera have been
                // specified by IP address or IP name and may never have connected yet, we can find out
                // if the IP address of this recently connected camera matches with ours.
                if (!pDriver->uniqueId) {
                    if (!uniqueIP)  { /* If we did not find out the IP address of this recently connected camera yet */
                        tPvIpSettings ipSettings;
                        PvCameraIpSettingsGet(UniqueId, &ipSettings);
                        uniqueIP = ipSettings.CurrentIpAddress;
                    }
                    if (uniqueIP == pDriver->uniqueIP) {
                        status = pDriver->connectCamera();
                    if (status) 
                        printf("Camera uniqueIP 0x%lx connectCamera() error status %d\n",pDriver->uniqueIP, status);
                    else
                      found = 1;
                    }
                }
                else {
                    if (UniqueId == pDriver->uniqueId) {
                        status = pDriver->connectCamera();
                        if (status) 
                            printf("Camera uniqueId 0x%lx connectCamera() error status %d\n",pDriver->uniqueId, status);
                        else
                        found=1;
                    }
               }
               break;

            case ePvLinkRemove:
                if (UniqueId == pDriver->uniqueId ) {
                    // the camera has disconnected
                    status = pDriver->disconnectCamera();
                    if (status) 
                        printf("Camera uniqueId 0x%lx disconnectCamera() error status %d\n",pDriver->uniqueId, status);
                  else
                       found=1;
                }
                break;

            default:
                break;
        }
        pDriver->unlock();
        if (found) break;
        pNode = (cameraNode *)ellNext(&pNode->node);
    }
}


/* From asynPortDriver: Connects driver to device; */
asynStatus prosilica::connect(asynUser* pasynUser) {

    return connectCamera();
}


/* From asynPortDriver: Disconnects driver from device; */
asynStatus prosilica::disconnect(asynUser* pasynUser) {

    return disconnectCamera();
}


static void PVDECL frameCallbackC(tPvFrame *pFrame)
{
    prosilica *pPvt = (prosilica *) pFrame->Context[0];
    
    pPvt->frameCallback(pFrame);
}


/** Sync the camera time with an EPICS timestamp */
asynStatus prosilica::syncTimer() {

    //static const char *functionName = "syncTimer";
    if (this->PvHandle) {
        epicsTimeGetCurrent(&lastSyncTime);
        // Tell the camera to reset its internal clock
        PvCommandRun(this->PvHandle, "TimeStampReset");
        return asynSuccess;
    }
    else {
        return asynError;
    }
}


/** This function gets called in a thread from the PvApi library when a new frame arrives */
void prosilica::frameCallback(tPvFrame *pFrame)
{
    int ndims;
    size_t dims[3];
    int imageCounter;
    int arrayCallbacks;
    NDArray *pImage;
    NDArray *pTempImage;
    int binX, binY;
    int badFrameCounter;
    int bayerConvert;
    epicsInt32 bayerPattern, colorMode;
    static const char *functionName = "frameCallback";

    /* If this callback is coming from a shutdown operation rather than normal collection, 
     * we will not be able to take the mutex and things will hang.  Prevent this by looking at the frame
     * status and returning immediately if it is and in that case the mutex has already been taken.  Just return in
     * that case */
    if (pFrame->Status == ePvErrCancelled) return;

    this->lock();

    pImage = (NDArray *)pFrame->Context[1];
    
    /* If we're out of memory, pImage will be NULL */
    if (pImage && pFrame->Status == ePvErrSuccess) {
        /* The frame we just received has NDArray* in Context[1] */ 
        /* Set the properties of the image to those of the current frame */
        /* Convert from the PvApi data types to ADDataType */
        /* The pFrame structure does not contain the binning, get that from param lib,
         * but it could be wrong for this frame if recently changed */

        /* First lets set the timestamp so it is as close to acquisition as 
         * possible and set the unique id from the framecounter */

        pImage->uniqueId = pFrame->FrameCount;
        updateTimeStamp(&pImage->epicsTS);

        getIntegerParam(ADBinX, &binX);
        getIntegerParam(ADBinY, &binY);

        /* The mono cameras can return a Bayer pattern which is invalid, and this can
         * crash the file plugin.  Fix it here. */
        if (pFrame->BayerPattern > ePvBayerBGGR) pFrame->BayerPattern = ePvBayerRGGB;
        bayerPattern = pFrame->BayerPattern;
        getIntegerParam(PSBayerConvert, &bayerConvert);

        switch(pFrame->Format) {
            case ePvFmtMono8:
                colorMode = NDColorModeMono;
                pImage->dataType = NDUInt8;
                pImage->ndims = 2;
                pImage->dims[0].size    = pFrame->Width;
                pImage->dims[0].offset  = pFrame->RegionX;
                pImage->dims[0].binning = binX;
                pImage->dims[1].size    = pFrame->Height;
                pImage->dims[1].offset  = pFrame->RegionY;
                pImage->dims[1].binning = binY;
                break;

            case ePvFmtMono16:
                colorMode = NDColorModeMono;
                pImage->dataType = NDUInt16;
                pImage->ndims = 2;
                pImage->dims[0].size    = pFrame->Width;
                pImage->dims[0].offset  = pFrame->RegionX;
                pImage->dims[0].binning = binX;
                pImage->dims[1].size    = pFrame->Height;
                pImage->dims[1].offset  = pFrame->RegionY;
                pImage->dims[1].binning = binY;
                break;

            case ePvFmtBayer8:
                if (bayerConvert == PSBayerConvertNone) {
                    colorMode = NDColorModeBayer;
                    pImage->dataType = NDUInt8;
                    pImage->ndims = 2;
                    pImage->dims[0].size   = pFrame->Width;
                    pImage->dims[0].offset = pFrame->RegionX;
                    pImage->dims[0].binning = binX;
                    pImage->dims[1].size   = pFrame->Height;
                    pImage->dims[1].offset = pFrame->RegionY;
                    pImage->dims[1].binning = binY;
                } else {
                    pTempImage = pImage;
                    ndims = 3;
                    dims[0] = 3;
                    dims[1] = pFrame->Width;
                    dims[2] = pFrame->Height;
                    pImage = this->pNDArrayPool->alloc(ndims, dims, NDUInt8, this->maxFrameSize, NULL);
                    epicsUInt8 *pData = (epicsUInt8 *)pImage->pData;
                    switch (bayerConvert) {
                        case PSBayerConvertRGB1: {
                            PvUtilityColorInterpolate(pFrame, pData, pData+1, pData+2, 2, 0);
                            colorMode = NDColorModeRGB1;
                            pImage->ndims = 3;
                            pImage->dims[0].size    = 3;
                            pImage->dims[0].offset  = 0;
                            pImage->dims[0].binning = 1;
                            pImage->dims[1].size   = pFrame->Width;
                            pImage->dims[1].offset = pFrame->RegionX;
                            pImage->dims[1].binning = binX;
                            pImage->dims[2].size   = pFrame->Height;
                            pImage->dims[2].offset = pFrame->RegionY;
                            pImage->dims[2].binning = binY;
                            break;
                        }

                        case PSBayerConvertRGB2: {
                            int rowSize = pFrame->Width;
                            PvUtilityColorInterpolate(pFrame, pData,  pData+rowSize, pData+2*rowSize, 
                                                      0, (unsigned long)(2*rowSize));
                            colorMode = NDColorModeRGB2;
                            pImage->ndims = 3;
                            pImage->dims[0].size   = pFrame->Width;
                            pImage->dims[0].offset = pFrame->RegionX;
                            pImage->dims[0].binning = binX;
                            pImage->dims[1].size    = 3;
                            pImage->dims[1].offset  = 0;
                            pImage->dims[1].binning = 1;
                            pImage->dims[2].size   = pFrame->Height;
                            pImage->dims[2].offset = pFrame->RegionY;
                            pImage->dims[2].binning = binY;
                            break;
                        }

                        case PSBayerConvertRGB3: {
                            int imageSize = pFrame->Width * pFrame->Height;
                            PvUtilityColorInterpolate(pFrame, pData,  pData+imageSize, pData+2*imageSize, 0, 0);
                            colorMode = NDColorModeRGB3;
                            pImage->ndims = 3;
                            pImage->dims[0].size   = pFrame->Width;
                            pImage->dims[0].offset = pFrame->RegionX;
                            pImage->dims[0].binning = binX;
                            pImage->dims[1].size   = pFrame->Height;
                            pImage->dims[1].offset = pFrame->RegionY;
                            pImage->dims[1].binning = binY;
                            pImage->dims[2].size    = 3;
                            pImage->dims[2].offset  = 0;
                            pImage->dims[2].binning = 1;
                            break;
                        }
                    }
                    pTempImage->release();
                }
                break;

            case ePvFmtBayer16:
                if (bayerConvert == PSBayerConvertNone) {
                    colorMode = NDColorModeBayer;
                    pImage->dataType = NDUInt16;
                    pImage->ndims = 2;
                    pImage->dims[0].size    = pFrame->Width;
                    pImage->dims[0].offset  = pFrame->RegionX;
                    pImage->dims[0].binning = binX;
                    pImage->dims[1].size    = pFrame->Height;
                    pImage->dims[1].offset  = pFrame->RegionY;
                    pImage->dims[1].binning = binY;
                } else {
                    pTempImage = pImage;
                    ndims = 3;
                    dims[0] = 3;
                    dims[1] = pFrame->Width;
                    dims[2] = pFrame->Height;
                    pImage = this->pNDArrayPool->alloc(ndims, dims, NDUInt16, this->maxFrameSize, NULL);
                    epicsUInt16 *pData = (epicsUInt16 *)pImage->pData;

                    switch (bayerConvert) {
                        case PSBayerConvertRGB1: {
                            PvUtilityColorInterpolate(pFrame, pData, pData+1, pData+2, 2, 0);
                            colorMode = NDColorModeRGB1;
                            pImage->ndims = 3;
                            pImage->dims[0].size    = 3;
                            pImage->dims[0].offset  = 0;
                            pImage->dims[0].binning = 1;
                            pImage->dims[1].size   = pFrame->Width;
                            pImage->dims[1].offset = pFrame->RegionX;
                            pImage->dims[1].binning = binX;
                            pImage->dims[2].size   = pFrame->Height;
                            pImage->dims[2].offset = pFrame->RegionY;
                            pImage->dims[2].binning = binY;
                            break;
                        }

                        case PSBayerConvertRGB2: {
                            int rowSize = pFrame->Width;
                            PvUtilityColorInterpolate(pFrame, pData,  pData+rowSize, pData+2*rowSize, 
                                                      0, (unsigned long)(2*rowSize));
                            colorMode = NDColorModeRGB2;
                            pImage->ndims = 3;
                            pImage->dims[0].size   = pFrame->Width;
                            pImage->dims[0].offset = pFrame->RegionX;
                            pImage->dims[0].binning = binX;
                            pImage->dims[1].size    = 3;
                            pImage->dims[1].offset  = 0;
                            pImage->dims[1].binning = 1;
                            pImage->dims[2].size   = pFrame->Height;
                            pImage->dims[2].offset = pFrame->RegionY;
                            pImage->dims[2].binning = binY;
                            break;
                        }

                        case PSBayerConvertRGB3: {
                            int imageSize = pFrame->Width * pFrame->Height;
                            PvUtilityColorInterpolate(pFrame, pData,  pData+imageSize, pData+2*imageSize, 0, 0);
                            colorMode = NDColorModeRGB3;
                            pImage->ndims = 3;
                            pImage->dims[0].size   = pFrame->Width;
                            pImage->dims[0].offset = pFrame->RegionX;
                            pImage->dims[0].binning = binX;
                            pImage->dims[1].size   = pFrame->Height;
                            pImage->dims[1].offset = pFrame->RegionY;
                            pImage->dims[1].binning = binY;
                            pImage->dims[2].size    = 3;
                            pImage->dims[2].offset  = 0;
                            pImage->dims[2].binning = 1;
                            break;
                        }
                    }
                    pTempImage->release();
                }
                break;

            case ePvFmtRgb24:
                colorMode = NDColorModeRGB1;
                pImage->dataType = NDUInt8;
                pImage->ndims = 3;
                pImage->dims[0].size    = 3;
                pImage->dims[0].offset  = 0;
                pImage->dims[0].binning = 1;
                pImage->dims[1].size    = pFrame->Width;
                pImage->dims[1].offset  = pFrame->RegionX;
                pImage->dims[1].binning = binX;
                pImage->dims[2].size    = pFrame->Height;
                pImage->dims[2].offset  = pFrame->RegionY;
                pImage->dims[2].binning = binY;
                break;

            case ePvFmtRgb48:
                colorMode = NDColorModeRGB1;
                pImage->dataType = NDUInt16;
                pImage->ndims = 3;
                pImage->dims[0].size    = 3;
                pImage->dims[0].offset  = 0;
                pImage->dims[0].binning = 1;
                pImage->dims[1].size    = pFrame->Width;
                pImage->dims[1].offset  = pFrame->RegionX;
                pImage->dims[1].binning = binX;
                pImage->dims[2].size    = pFrame->Height;
                pImage->dims[2].offset  = pFrame->RegionY;
                pImage->dims[2].binning = binY;
                break;

            default:
                 /* We don't support other formats yet */
                asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                    "%s:%s: error unsupported pixel format %d\n", 
                    driverName, functionName, pFrame->Format);
                break;
        }
        pImage->pAttributeList->add("BayerPattern", "Bayer Pattern", NDAttrInt32, &bayerPattern);
        pImage->pAttributeList->add("ColorMode", "Color Mode", NDAttrInt32, &colorMode);
        
        /* Now set timeStamp field in pImage */
        const double native_frame_ticks =  ((double)pFrame->TimestampLo + (double)pFrame->TimestampHi*4294967296.);

        /* Determine how to set the timeStamp */
        PSTimestampType_t timestamp_type = PSTimestampTypeNativeTicks;
        getIntegerParam(PSTimestampType, (int*)&timestamp_type);

        switch (timestamp_type) {
            case PSTimestampTypeNativeTicks:
                pImage->timeStamp = native_frame_ticks;
                break;

            case PSTimestampTypeNativeSeconds:
                if (this->timeStampFrequency == 0) this->timeStampFrequency = 1;
                pImage->timeStamp = native_frame_ticks / this->timeStampFrequency;
                break;

            case PSTimestampTypePOSIX: {
                    if (this->timeStampFrequency == 0) this->timeStampFrequency = 1;
                    epicsTimeStamp epics_frame_time = lastSyncTime;
                    epicsTimeAddSeconds(&epics_frame_time, 
                        native_frame_ticks/this->timeStampFrequency);
                    timespec ts;
                    epicsTimeToTimespec(&ts, &epics_frame_time);
                    pImage->timeStamp = (double)ts.tv_sec + ((double)ts.tv_nsec * 1.0e-9);
                }
                break;

            case PSTimestampTypeEPICS: {
                    if (this->timeStampFrequency == 0) this->timeStampFrequency = 1;
                    epicsTimeStamp epics_frame_time = lastSyncTime;
                    epicsTimeAddSeconds(&epics_frame_time, 
                        native_frame_ticks/this->timeStampFrequency);
                    pImage->timeStamp = (double)epics_frame_time.secPastEpoch + 
                      ((double)epics_frame_time.nsec * 1.0e-09);
                }
                break;

            case PSTimestampTypeIOC: {
                    pImage->timeStamp = (double)pImage->epicsTS.secPastEpoch 
                      + ((double)pImage->epicsTS.nsec * 1.0e-9);
                }
                break;

            default:
                pImage->timeStamp = native_frame_ticks;
        }

        /* Get any attributes that have been defined for this driver */        
        this->getAttributes(pImage->pAttributeList);
        
        getIntegerParam(NDArrayCallbacks, &arrayCallbacks);

        if (arrayCallbacks) {
            /* Call the NDArray callback */
            doCallbacksGenericPointer(pImage, NDArrayData, 0);
        }

        /* See if acquisition is done */
        if (this->framesRemaining > 0) this->framesRemaining--;
        if (this->framesRemaining == 0) {
            setShutter(0);
            setIntegerParam(ADAcquire, 0);
            setIntegerParam(ADStatus, ADStatusIdle);
        }

        /* Update the frame counter */
        getIntegerParam(NDArrayCounter, &imageCounter);
        imageCounter++;
        setIntegerParam(NDArrayCounter, imageCounter);

        asynPrintIO(this->pasynUserSelf, ASYN_TRACEIO_DRIVER, 
            (const char *)pImage->pData, pImage->dataSize,
            "%s:%s: frameId=%d, timeStamp=%f\n",
            driverName, functionName, pImage->uniqueId, pImage->timeStamp);

        /* We save the most recent good image buffer so it can be used in the
         * readADImage function.  Now release it. */
        if (this->pArrays[0]) this->pArrays[0]->release();
        this->pArrays[0] = pImage;

        /* Allocate a new image buffer, make the size be the maximum that the frames can be */
        ndims = 2;
        dims[0] = this->sensorWidth;
        dims[1] = this->sensorHeight;
        pImage = this->pNDArrayPool->alloc(ndims, dims, NDInt8, this->maxFrameSize, NULL);
        /* Put the pointer to this image buffer in the frame context[1] */
        pFrame->Context[1] = pImage;
        /* Reset the frame buffer data pointer be this image buffer data pointer */
        pFrame->ImageBuffer = pImage->pData;
    } else {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: ERROR, frame has error code %d\n",
            driverName, functionName, pFrame->Status);
        getIntegerParam(PSBadFrameCounter, &badFrameCounter);
        badFrameCounter++;
        setIntegerParam(PSBadFrameCounter, badFrameCounter);
    }

    /* Update any changed parameters */
    callParamCallbacks();
    
    /* Queue this frame to run again */
    PvCaptureQueueFrame(this->PvHandle, pFrame, frameCallbackC); 
    this->unlock();
}

asynStatus prosilica::setPixelFormat()
{
    int status = asynSuccess;
    int colorMode, dataType;
    static const char *functionName = "setPixelFormat";
    char pixelFormat[20];

    status |= getIntegerParam(NDColorMode, &colorMode);
    status |= getIntegerParam(NDDataType, &dataType);
    if      ((colorMode == NDColorModeMono)  && (dataType == NDUInt8))  strcpy(pixelFormat, "Mono8");
    else if ((colorMode == NDColorModeMono)  && (dataType == NDUInt16)) strcpy(pixelFormat, "Mono16");
    else if ((colorMode == NDColorModeRGB1)  && (dataType == NDUInt8))  strcpy(pixelFormat, "Rgb24");
    else if ((colorMode == NDColorModeRGB1)  && (dataType == NDUInt16)) strcpy(pixelFormat, "Rgb48");
    else if ((colorMode == NDColorModeBayer) && (dataType == NDUInt8))  strcpy(pixelFormat, "Bayer8");
    else if ((colorMode == NDColorModeBayer) && (dataType == NDUInt16)) strcpy(pixelFormat, "Bayer16");
    else {
         /* We don't support other formats yet */
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: error unsupported data type %d and/or color mode %d\n", 
            driverName, functionName, dataType, colorMode);
        return(asynError);
    }
    status |= PvAttrEnumSet(this->PvHandle, "PixelFormat", pixelFormat);
    return((asynStatus)status);
}      

asynStatus prosilica::setGeometry()
{
    int status = asynSuccess;
    int s;
    int binX, binY, minY, minX, sizeX, sizeY, maxSizeX, maxSizeY;
    static const char *functionName = "setGeometry";
    
    /* Get all of the current geometry parameters from the parameter library */
    status |= getIntegerParam(ADBinX, &binX);
    if (binX < 1) binX = 1;
    status |= getIntegerParam(ADBinY, &binY);
    if (binY < 1) binY = 1;
    status |= getIntegerParam(ADMinX, &minX);
    status |= getIntegerParam(ADMinY, &minY);
    status |= getIntegerParam(ADSizeX, &sizeX);
    status |= getIntegerParam(ADSizeY, &sizeY);
    status |= getIntegerParam(ADMaxSizeX, &maxSizeX);
    status |= getIntegerParam(ADMaxSizeY, &maxSizeY);

    if (minX + sizeX > maxSizeX) {
        sizeX = maxSizeX - minX;
        setIntegerParam(ADSizeX, sizeX);
    }
    if (minY + sizeY > maxSizeY) {
        sizeY = maxSizeY - minY;
        setIntegerParam(ADSizeY, sizeY);
    }
    
    /* CMOS cameras don't support binning, so ignore ePvErrNotFound errors */
    s = PvAttrUint32Set(this->PvHandle, "BinningX", binX);
    if (s != ePvErrNotFound) status |= s;
    s = PvAttrUint32Set(this->PvHandle, "BinningY", binY);
    if (s != ePvErrNotFound) status |= s;

    if(!status){
      status |= PvAttrUint32Set(this->PvHandle, "RegionX", minX/binX);
      status |= PvAttrUint32Set(this->PvHandle, "RegionY", minY/binY);
      status |= PvAttrUint32Set(this->PvHandle, "Width",   sizeX/binX);
      status |= PvAttrUint32Set(this->PvHandle, "Height",  sizeY/binY);
    }
    
    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                      "%s:%s: error, status=%d\n", 
                      driverName, functionName, status);
    return((asynStatus)status);
}

asynStatus prosilica::getGeometry()
{
    int status = asynSuccess;
    int s;
    tPvUint32 binX, binY, minY, minX, sizeX, sizeY;
    static const char *functionName = "getGeometry";

    /* CMOS cameras don't support binning, so ignore ePvErrNotFound errors */
    s = PvAttrUint32Get(this->PvHandle, "BinningX", &binX);
    if (s) binX = 1;
    if (s != ePvErrNotFound) status |= s;
    s = PvAttrUint32Get(this->PvHandle, "BinningY", &binY);
    if (s) binY = 1;
    if (s != ePvErrNotFound) status |= s;
    status |= PvAttrUint32Get(this->PvHandle, "RegionX",  &minX);
    status |= PvAttrUint32Get(this->PvHandle, "RegionY",  &minY);
    status |= PvAttrUint32Get(this->PvHandle, "Width",    &sizeX);
    status |= PvAttrUint32Get(this->PvHandle, "Height",   &sizeY);
    
    status |= setIntegerParam(ADBinX,  binX);
    status |= setIntegerParam(ADBinY,  binY);
    status |= setIntegerParam(ADMinX,  minX*binX);
    status |= setIntegerParam(ADMinY,  minY*binY);
    status |= setIntegerParam(ADSizeX, sizeX*binX);
    status |= setIntegerParam(ADSizeY, sizeY*binY);
    status |= setIntegerParam(NDArraySizeX, sizeX);
    status |= setIntegerParam(NDArraySizeY, sizeY);
    
    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                      "%s:%s: error, status=%d\n", 
                      driverName, functionName, status);
    return((asynStatus)status);
}

asynStatus prosilica::readStats()
{
    int status = asynSuccess;
    char buffer[50];
    unsigned long nchars;
    tPvUint32 uval;
    int i;
    float fval;
    static const char *functionName = "readStats";
    
    status |= PvAttrEnumGet      (this->PvHandle, "StatDriverType", buffer, sizeof(buffer), &nchars);
    if (status == ePvErrNotFound) {
        status = 0;
        strcpy(buffer, "Unsupported parameter");
    }
    status |= setStringParam (PSDriverType, buffer);    
    status |= PvAttrStringGet    (this->PvHandle, "StatFilterVersion", buffer, sizeof(buffer), &nchars);
    if (status == ePvErrNotFound) {
        status = 0;
        strcpy(buffer, "Unsupported parameter");
    }
    status |= setStringParam (PSFilterVersion, buffer);
    status |= PvAttrFloat32Get   (this->PvHandle, "StatFrameRate", &fval);
    status |= setDoubleParam (PSFrameRate, fval);
    status |= PvAttrUint32Get    (this->PvHandle, "StreamBytesPerSecond", &uval);
    status |= setIntegerParam(PSByteRate, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "PacketSize", &uval);
    status |= setIntegerParam(PSPacketSize, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatFramesCompleted", &uval);
    status |= setIntegerParam(PSFramesCompleted, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatFramesDropped", &uval);
    status |= setIntegerParam(PSFramesDropped, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatPacketsErroneous", &uval);
    status |= setIntegerParam(PSPacketsErroneous, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatPacketsMissed", &uval);
    status |= setIntegerParam(PSPacketsMissed, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatPacketsReceived", &uval);
    status |= setIntegerParam(PSPacketsReceived, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatPacketsRequested", &uval);
    status |= setIntegerParam(PSPacketsRequested, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "StatPacketsResent", &uval);
    status |= setIntegerParam(PSPacketsResent, (int)uval);
    status |= PvAttrUint32Get    (this->PvHandle, "SyncInLevels", &uval);
    status |= setIntegerParam(PSSyncIn1Level, uval&0x01 ? 1:0);
    status |= setIntegerParam(PSSyncIn2Level, uval&0x02 ? 1:0);
    status |= PvAttrUint32Get    (this->PvHandle, "SyncOutGpoLevels", &uval);
    status |= setIntegerParam(PSSyncOut1Level, uval&0x01 ? 1:0);
    status |= setIntegerParam(PSSyncOut2Level, uval&0x02 ? 1:0);
    status |= setIntegerParam(PSSyncOut3Level, uval&0x04 ? 1:0);
    status |= PvAttrUint32Get    (this->PvHandle, "FrameStartTriggerDelay", &uval);
    status |= setDoubleParam(PSTriggerDelay, uval/1.e6);
    status |= PvAttrEnumGet(this->PvHandle, "FrameStartTriggerEvent", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_TRIGGER_EVENT_MODES; i++) {
        if (strcmp(buffer, PSTriggerEventModes[i]) == 0) {
            status |= setIntegerParam(PSTriggerEvent, i);
            break;
        }
    }
    if (i == NUM_TRIGGER_EVENT_MODES) {
        status |= setIntegerParam(PSTriggerEvent, 0);
        status |= asynError;
    }    
    status |= PvAttrEnumGet(this->PvHandle, "FrameStartTriggerOverlap", buffer, sizeof(buffer), &nchars);
    /* This parameter can be not supported */
    if (status == ePvErrNotFound) {
        status = 0;
        status |= setIntegerParam(PSTriggerOverlap, 0);
    } else {
        for (i=0; i<NUM_TRIGGER_OVERLAP_MODES; i++) {
            if (strcmp(buffer, PSTriggerOverlapModes[i]) == 0) {
                status |= setIntegerParam(PSTriggerOverlap, i);
                break;
            }
        }
        if (i == NUM_TRIGGER_OVERLAP_MODES) {
            status |= setIntegerParam(PSTriggerOverlap, 0);
            status |= asynError;
        }
    }
    status |= PvAttrEnumGet(this->PvHandle, "SyncOut1Mode", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_SYNC_OUT_MODES; i++) {
        if (strcmp(buffer, PSSyncOutModes[i]) == 0) {
            status |= setIntegerParam(PSSyncOut1Mode, i);
            break;
        }
    }
    if (i == NUM_SYNC_OUT_MODES) {
        status |= setIntegerParam(PSSyncOut1Mode, 0);
        status |= asynError;
    }
    status |= PvAttrEnumGet(this->PvHandle, "SyncOut2Mode", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_SYNC_OUT_MODES; i++) {
        if (strcmp(buffer, PSSyncOutModes[i]) == 0) {
            status |= setIntegerParam(PSSyncOut2Mode, i);
            break;
        }
    }
    if (i == NUM_SYNC_OUT_MODES) {
        status |= setIntegerParam(PSSyncOut2Mode, 0);
        status |= asynError;
    }
    status |= PvAttrEnumGet(this->PvHandle, "SyncOut3Mode", buffer, sizeof(buffer), &nchars);
    /* This parameter can be not supported */
    if (status == ePvErrNotFound) {
        status = 0;
        status |= setIntegerParam(PSSyncOut3Mode, 0);
    } else {
        for (i=0; i<NUM_SYNC_OUT_MODES; i++) {
            if (strcmp(buffer, PSSyncOutModes[i]) == 0) {
                status |= setIntegerParam(PSSyncOut3Mode, i);
                break;
            }
        }
        if (i == NUM_SYNC_OUT_MODES) {
            status |= setIntegerParam(PSSyncOut3Mode, 0);
            status |= asynError;
        }
    }
    
    status |= PvAttrEnumGet(this->PvHandle, "SyncOut1Invert", buffer, sizeof(buffer), &nchars);
    if (strcmp(buffer, "Off") == 0) i = 0;
    else if (strcmp(buffer, "On") == 0) i = 1;
    else {
        i=0;
        status |= asynError;
    }
    status |= setIntegerParam(PSSyncOut1Invert, i);
    status |= PvAttrEnumGet(this->PvHandle, "SyncOut2Invert", buffer, sizeof(buffer), &nchars);
    if (strcmp(buffer, "Off") == 0) i = 0;
    else if (strcmp(buffer, "On") == 0) i = 1;
    else {
        i=0;
        status |= asynError;
    }
    status |= setIntegerParam(PSSyncOut2Invert, i);
    status |= PvAttrEnumGet(this->PvHandle, "SyncOut3Invert", buffer, sizeof(buffer), &nchars);
    if (status == ePvErrNotFound) {
        status = 0;
        i=0;
    } else {
        if (strcmp(buffer, "Off") == 0) i = 0;
        else if (strcmp(buffer, "On") == 0) i = 1;
        else {
            i=0;
            status |= asynError;
        }
    }
    status |= setIntegerParam(PSSyncOut3Invert, i);

    status |= PvAttrEnumGet(this->PvHandle, "Strobe1Mode", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_STROBE_MODES; i++) {
        if (strcmp(buffer, PSStrobeModes[i]) == 0) {
            status |= setIntegerParam(PSStrobe1Mode, i);
            break;
        }
    }
    if (i == NUM_STROBE_MODES) {
        status |= setIntegerParam(PSStrobe1Mode, 0);
        status |= asynError;
    }
    status |= PvAttrEnumGet(this->PvHandle, "Strobe1ControlledDuration", buffer, sizeof(buffer), &nchars);
    if (strcmp(buffer, "Off") == 0) i = 0;
    else if (strcmp(buffer, "On") == 0) i = 1;
    else {
        i=0;
        status |= asynError;
    }
    status |= setIntegerParam(PSStrobe1CtlDuration, i);

    status |= PvAttrUint32Get    (this->PvHandle, "Strobe1Delay", &uval);
    status |= setDoubleParam(PSStrobe1Delay, uval/1.e6);
    status |= PvAttrUint32Get    (this->PvHandle, "Strobe1Duration", &uval);
    status |= setDoubleParam(PSStrobe1Duration, uval/1.e6);

    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                      "%s:%s: error, status=%d\n", 
                      driverName, functionName, status);
    return(asynSuccess);
}

asynStatus prosilica::readParameters()
{
    int status = asynSuccess;
    tPvUint32 intVal;
    int i;
    int dataType=NDUInt8, colorMode=NDColorModeMono;
    tPvFloat32 fltVal;
    double dval;
    unsigned long nchars;
    char buffer[20];
    static const char *functionName = "readParameters";

    status |= PvAttrUint32Get(this->PvHandle, "TotalBytesPerFrame", &intVal);
    setIntegerParam(NDArraySize, intVal);

    status |= PvAttrEnumGet(this->PvHandle, "PixelFormat", buffer, sizeof(buffer), &nchars);
    if      (!strcmp(buffer, "Mono8")) {
        dataType = NDUInt8;
        colorMode = NDColorModeMono;
    }
    else if (!strcmp(buffer, "Mono16")) {
        dataType = NDUInt16;
        colorMode = NDColorModeMono;
    }
    else if (!strcmp(buffer, "Rgb24")) {
        dataType = NDUInt8;
        colorMode = NDColorModeRGB1;
    }
    else if (!strcmp(buffer, "Rgb48")) {
        dataType = NDUInt16;
        colorMode = NDColorModeRGB1;
    }
    else if (!strcmp(buffer, "Bayer8")) {
        dataType = NDUInt8;
        colorMode = NDColorModeBayer;
    }
    else if (!strcmp(buffer, "Bayer16")) { 
        dataType = NDUInt16;
        colorMode = NDColorModeBayer;
    }
    else {
         /* We don't support other formats yet */
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: error unsupported data type %d and/or color mode %d\n", 
            driverName, functionName, dataType, colorMode);
    }
    status |= setIntegerParam(NDDataType, dataType);
    status |= setIntegerParam(NDColorMode, colorMode);
    
    status |= getGeometry();

    status |= PvAttrUint32Get(this->PvHandle, "AcquisitionFrameCount", &intVal);
    status |= setIntegerParam(ADNumImages, intVal);

    status |= PvAttrEnumGet(this->PvHandle, "AcquisitionMode", buffer, sizeof(buffer), &nchars);
    if      (!strcmp(buffer, "SingleFrame")) i = ADImageSingle;
    else if (!strcmp(buffer, "MultiFrame"))  i = ADImageMultiple;
    else if (!strcmp(buffer, "Recorder"))    i = ADImageMultiple;
    else if (!strcmp(buffer, "Continuous"))  i = ADImageContinuous;
    else {i=0; status |= asynError;}
    status |= setIntegerParam(ADImageMode, i);

    status |= PvAttrEnumGet(this->PvHandle, "FrameStartTriggerMode", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_TRIGGER_START_MODES; i++) {
        if (strcmp(buffer, PSTriggerStartModes[i]) == 0) {
            status |= setIntegerParam(ADTriggerMode, i);
            break;
        }
    }
    if (i == NUM_TRIGGER_START_MODES) {
        status |= setIntegerParam(ADTriggerMode, 0);
        status |= asynError;
    }
    
    /* Prosilica does not support more than 1 exposure per frame */
    status |= setIntegerParam(ADNumExposures, 1);

    /* Prosilica uses integer microseconds */
    status |= PvAttrUint32Get(this->PvHandle, "ExposureValue", &intVal);
    dval = intVal / 1.e6;
    status |= setDoubleParam(ADAcquireTime, dval);

    /* Prosilica uses a frame rate in Hz */
    status |= PvAttrFloat32Get(this->PvHandle, "FrameRate", &fltVal);
    if (fltVal == 0.) fltVal = 1;
    dval = 1. / fltVal;
    status |= setDoubleParam(ADAcquirePeriod, dval);

    /* Prosilica uses an integer value */
    status |= PvAttrUint32Get(this->PvHandle, "GainValue", &intVal);
    dval = intVal;
    status |= setDoubleParam(ADGain, dval);

    /* Exposure mode can be maunal or auto */
    status |= PvAttrEnumGet(this->PvHandle, "ExposureMode", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_EXPOSURE_MODES; i++) {
        if (strcmp(buffer, PSExposureModes[i]) == 0) {
            status |= setIntegerParam(PSExposureMode, i);
            break;
        }
    }
    if (i == NUM_EXPOSURE_MODES) {
        status |= setIntegerParam(PSExposureMode, 0);
        status |= asynError;
    }
    
    /* Gain mode can be maunal or auto */
    status |= PvAttrEnumGet(this->PvHandle, "GainMode", buffer, sizeof(buffer), &nchars);
    for (i=0; i<NUM_GAIN_MODES; i++) {
        if (strcmp(buffer, PSGainModes[i]) == 0) {
            status |= setIntegerParam(PSGainMode, i);
            break;
        }
    }
    if (i == NUM_GAIN_MODES) {
        status |= setIntegerParam(PSGainMode, 0);
        status |= asynError;
    }
 
    /* Call the callbacks to update the values in higher layers */
    callParamCallbacks();
    
    if (status) asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                      "%s:%s: error, status=%d\n", 
                      driverName, functionName, status);
    return((asynStatus)status);
}

asynStatus prosilica::disconnectCamera()
{
    int status = asynSuccess;
    tPvFrame *pFrame;
    NDArray *pImage;
    static const char *functionName = "disconnectCamera";

    /* Ensure that PvAPI has been initialised */
    if (!PvApiInitialized) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: Disconnecting from camera %ld while the PvAPI is uninitialized.\n", 
            driverName, functionName, this->uniqueId);
        return asynError;
    }

    if (!this->PvHandle) return(asynSuccess);
    // We have the lock at this point, but these functions can block resulting in a deadlock
    //  Release the lock
    unlock();
    status |= PvCaptureQueueClear(this->PvHandle);
    status |= PvCaptureEnd(this->PvHandle);
    status |= PvCameraClose(this->PvHandle);
    lock();
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
          "%s:%s: disconnecting camera %ld\n", 
          driverName, functionName, this->uniqueId);
    if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unable to close camera %lu\n",
              driverName, functionName, this->uniqueId);
    }
    /* If we have allocated frame buffers, free them. */
    /* Must first free any image buffers they point to */
    for (int i = 0; i < maxPvAPIFrames_; i++) {
        pFrame = &(this->PvFrames[i]);
        if (! pFrame ) continue;

        pImage = (NDArray *)pFrame->Context[1];
        if (pImage) pImage->release();
        pFrame->Context[1] = 0;
    }

    this->PvHandle = NULL;
    /* We've disconnected the camera. Signal to asynManager that we are disconnected. */
    status = pasynManager->exceptionDisconnect(this->pasynUserSelf);
    if (status) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: error calling pasynManager->exceptionDisconnect, error=%s\n",
            driverName, functionName, pasynUserSelf->errorMessage);
    }
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s: Camera disconnected; unique id: %ld\n", 
        driverName, functionName, this->uniqueId);
    return((asynStatus)status);
}

asynStatus prosilica::connectCamera()
{
    int status = asynSuccess;
    unsigned long nchars;
    tPvFrame *pFrame;
    int i;
    int ndims;
    size_t dims[2];
    int bytesPerPixel;
    NDArray *pImage;
    struct in_addr ipAddr;
    unsigned long versionMajor, versionMinor;
    char versionString[20];
    bool isUniqueId;
    static const char *functionName = "connectCamera";

    /* Ensure that PvAPI has been initialised */
    if (!PvApiInitialized) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
            "%s:%s: Connecting to camera %ld while the PvAPI is uninitialized.\n", 
            driverName, functionName, this->uniqueId);
        return asynError;
    }

    /* First disconnect from the camera */
    disconnectCamera();
    
    /* Determine if we have been passed a uniqueID (all characters in cameraId are digits), 
     * or an IP address (anything else) */
    isUniqueId = true;
    for (i=0; i<(int)strlen(this->cameraId); i++) {
      if (!isdigit(this->cameraId[i])) isUniqueId = false;
    }
    
    if (isUniqueId) {
        this->uniqueId = atoi(this->cameraId);
        status = PvCameraInfoEx(this->uniqueId, &this->PvCameraInfo, sizeof(this->PvCameraInfo));
        if (status) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s:%s: Cannot find camera %lu\n", 
                  driverName, functionName, this->uniqueId);
            return asynError;
        }
    } else {
        /* We have been given an IP address or IP name */
        status = hostToIPAddr(this->cameraId, &ipAddr);
        if (status) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s:%s: Cannot find IP address %s\n", 
                  driverName, functionName, this->cameraId);
            return asynError;
        }
        this->uniqueIP = (unsigned long) ipAddr.s_addr;
        status = PvCameraInfoByAddrEx(ipAddr.s_addr, &this->PvCameraInfo, NULL, sizeof(this->PvCameraInfo));
        if (status) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s:%s: Cannot find camera %s\n", 
                  driverName, functionName, this->cameraId);
            return asynError;
        }
        this->uniqueId = this->PvCameraInfo.UniqueId;
    }

    // Here's where reconnect fails.
    // PermittedAccess flags are 0x0002 for around 5 seconds after
    // a hard IOC restart which didn't call disconnectCamera()
    unsigned retryCount = 0;
    while ( (this->PvCameraInfo.PermittedAccess & ePvAccessMaster) == 0 ) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
              "%s:%s: No RW access for camera %lu, retrying ...\n", 
              driverName, functionName, this->uniqueId);
        
        // Wait a second and fetch status again
        epicsThreadSleep(CONNECT_RETRY_INTERVAL);

        status = PvCameraInfoEx(this->uniqueId, &this->PvCameraInfo, sizeof(this->PvCameraInfo));
        if (status) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s:%s: Cannot read status for camera %lu\n", 
                  driverName, functionName, this->uniqueId);
            return asynError;
        }
        if ( ++retryCount >= CONNECT_RETRY_COUNT ) {
            break;
        }
    }

    if ((this->PvCameraInfo.PermittedAccess & ePvAccessMaster) == 0) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: Cannot get control of camera %lu, access flags=%lx\n", 
               driverName, functionName, this->uniqueId, this->PvCameraInfo.PermittedAccess);
        return asynError;
    }

    if (isUniqueId)
      status = PvCameraOpen(this->uniqueId, ePvAccessMaster, &this->PvHandle);
    else
      status = PvCameraOpenByAddr(ipAddr.s_addr, ePvAccessMaster, &this->PvHandle);
    
    if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unable to open camera %lu\n",
              driverName, functionName, this->uniqueId);
        this->PvHandle = NULL;
        return asynError;
    }
 
    /* Negotiate maximum frame size */
    status = PvCaptureAdjustPacketSize(this->PvHandle, MAX_PACKET_SIZE);
    if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unable to adjust packet size on camera %lu\n",
              driverName, functionName, this->uniqueId);
       return asynError;
    }
    
    /* Initialize the frame buffers and queue them */
    status = PvCaptureStart(this->PvHandle);
    if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unable to start capture on camera %lu\n",
              driverName, functionName, this->uniqueId);
        return asynError;
    }

    /* We allocate image buffers that are large enough for the biggest possible image.
       This is simpler than reallocating when readout parameters change.  It is also safer,
       since changing readout parameters happens instantly, but there will still be frames
       queued with the wrong size */
    /* Query the parameters of the image sensor */
    status = PvAttrEnumGet(this->PvHandle, "SensorType", this->sensorType, 
                             sizeof(this->sensorType), &nchars);
    status |= PvAttrUint32Get(this->PvHandle, "SensorBits", &this->sensorBits);
    status |= PvAttrUint32Get(this->PvHandle, "SensorWidth", &this->sensorWidth);
    status |= PvAttrUint32Get(this->PvHandle, "SensorHeight", &this->sensorHeight);
    status |= PvAttrUint32Get(this->PvHandle, "TimeStampFrequency", &this->timeStampFrequency);
    status |= PvAttrStringGet(this->PvHandle, "DeviceIPAddress", this->IPAddress, 
                              sizeof(this->IPAddress), &nchars);
    if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unable to get sensor data on camera %lu\n",
              driverName, functionName, this->uniqueId);
        return asynError;
    }
    
    bytesPerPixel = (this->sensorBits-1)/8 + 1;
    /* If the camera supports color then there can be 3 values per pixel */
    if (strcmp(this->sensorType, "Mono") != 0) bytesPerPixel *= 3;
    this->maxFrameSize = this->sensorWidth * this->sensorHeight * bytesPerPixel;    
    for (i=0; i<maxPvAPIFrames_; i++) {
        pFrame = &this->PvFrames[i];
        ndims = 2;
        dims[0] = this->sensorWidth;
        dims[1] = this->sensorHeight;
       /* Allocate a new image buffer, make the size be the maximum that the frames can be */
        pImage = this->pNDArrayPool->alloc(ndims, dims, NDInt8, this->maxFrameSize, NULL);
        if (!pImage) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s:%s: unable to allocate image %d on camera %lu\n",
                  driverName, functionName, i, this->uniqueId);
            return asynError;
        }
        /* Set the frame buffer data pointer be this image buffer data pointer */
        pFrame->ImageBuffer = pImage->pData;
        pFrame->ImageBufferSize = this->maxFrameSize;
        /* Put a pointer to ourselves in Context[0] */
        pFrame->Context[0] = (void *)this;
        /* Put the pointer to this image buffer in the frame context[1] */
        pFrame->Context[1] = pImage;
        status = PvCaptureQueueFrame(this->PvHandle, pFrame, frameCallbackC); 
        if (status) {
            asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
                  "%s:%s: unable to queue frame %d on camera %lu\n",
                  driverName, functionName, i, this->uniqueId);
            return asynError;
        }
    }

    /* Set some initial values for other parameters */
    status =  setStringParam (ADManufacturer, "Prosilica");
    status |= setStringParam (ADModel, this->PvCameraInfo.ModelName);
    status |= setStringParam (ADSerialNumber, this->PvCameraInfo.SerialNumber);
    status |= setStringParam (ADFirmwareVersion, this->PvCameraInfo.FirmwareVersion);
    PvVersion(&versionMajor, &versionMinor);
    epicsSnprintf(versionString, sizeof(versionString), "%ld.%ld", versionMajor, versionMinor);
    status |= setStringParam (ADSDKVersion, versionString);
    epicsSnprintf(versionString, sizeof(versionString), "%d.%d.%d", 
                  DRIVER_VERSION, DRIVER_REVISION, DRIVER_MODIFICATION);
    setStringParam(NDDriverVersion, versionString);
    status |= setIntegerParam(ADSizeX, this->sensorWidth);
    status |= setIntegerParam(ADSizeY, this->sensorHeight);
    status |= setIntegerParam(ADMaxSizeX, this->sensorWidth);
    status |= setIntegerParam(ADMaxSizeY, this->sensorHeight);
    status |= setIntegerParam(PSBadFrameCounter, 0);
    if (status) {
        asynPrint(this->pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unable to set camera parameters on camera %lu\n",
              driverName, functionName, this->uniqueId);
        return asynError;
    }
    
     /* Read the current camera settings */
    status = readParameters();
    if (status) return((asynStatus)status);

    /* Read the current camera statistics */
    status = readStats();
    if (status) return((asynStatus)status);

    /* Force acquisition to stop.  
     * With CMOS cameras if the camera is already acquiring when we connect there will be problems,
     * and this can happen if the camera was acquiring when the IOC previously exited. */
    PvCommandRun(this->PvHandle, "AcquisitionAbort");

    /* Now sync the timer on the camera with the IOC */

    this->syncTimer();
        
    /* We found the camera and everything is OK.  Signal to asynManager that we are connected. */
    status = pasynManager->exceptionConnect(this->pasynUserSelf);
    if (status) {
        asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
            "%s:%s: error calling pasynManager->exceptionConnect, error=%s\n",
            driverName, functionName, pasynUserSelf->errorMessage);
        return asynError;
    }
    asynPrint(this->pasynUserSelf, ASYN_TRACE_FLOW, 
        "%s:%s: Camera connected; unique id: %ld\n", 
        driverName, functionName, this->uniqueId);
    return asynSuccess;
}


/** Called when asyn clients call pasynInt32->write().
  * This function performs actions for some parameters, including ADAcquire, ADBinX, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus prosilica::writeInt32(asynUser *pasynUser, epicsInt32 value)
{
    int function = pasynUser->reason;
    int status = asynSuccess;
    tPvUint32 syncs;
    static const char *functionName = "writeInt32";

    /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status |= setIntegerParam(function, value);

    if ((function == ADBinX) ||
        (function == ADBinY) ||
        (function == ADMinX) ||
        (function == ADSizeX) ||
        (function == ADMinY) ||
        (function == ADSizeY)) {
        /* These commands change the chip readout geometry.  We need to cache them and apply them in the
         * correct order */
        status |= setGeometry();
    } else if (function == ADNumImages) {
        status |= PvAttrUint32Set(this->PvHandle, "AcquisitionFrameCount", value);
    } else if (function == ADImageMode) {
        switch(value) {
        case ADImageSingle:
            status |= PvAttrEnumSet(this->PvHandle, "AcquisitionMode", "SingleFrame");
            break;
        case ADImageMultiple:
            status |= PvAttrEnumSet(this->PvHandle, "AcquisitionMode", "MultiFrame");
            break;
        case ADImageContinuous:
            status |= PvAttrEnumSet(this->PvHandle, "AcquisitionMode", "Continuous");
            break;
       }
    } else if (function == ADAcquire) {
        if (value) {
            /* We need to set the number of images we expect to collect, so the frame callback function
               can know when acquisition is complete.  We need to find out what mode we are in and how
               many frames have been requested.  If we are in continuous mode then set the number of
               remaining frames to -1. */
            int imageMode, numImages;
            status |= getIntegerParam(ADImageMode, &imageMode);
            status |= getIntegerParam(ADNumImages, &numImages);
            switch(imageMode) {
            case ADImageSingle:
                this->framesRemaining = 1;
                break;
            case ADImageMultiple:
                this->framesRemaining = numImages;
                break;
            case ADImageContinuous:
                this->framesRemaining = -1;
                break;
           }
            setIntegerParam(ADStatus, ADStatusAcquire);
            setShutter(1);
            status |= PvCommandRun(this->PvHandle, "AcquisitionStart");
        } else {
            setIntegerParam(ADStatus, ADStatusIdle);
            setShutter(0);
            status |= PvCommandRun(this->PvHandle, "AcquisitionAbort");
        }
    } else if (function == ADTriggerMode) {
        if ((value < 0) || (value > (NUM_TRIGGER_START_MODES-1))) {
            status = asynError;
        } else {
            status |= PvAttrEnumSet(this->PvHandle, "FrameStartTriggerMode", 
                                    PSTriggerStartModes[value]);
        }
    } else if (function == PSByteRate) {
            status |= PvAttrUint32Set(this->PvHandle, "StreamBytesPerSecond", value);
    } else if (function == PSReadStatistics) {
            readStats();
    } else if (function == PSTriggerEvent) {
            status |= PvAttrEnumSet(this->PvHandle, "FrameStartTriggerEvent", PSTriggerEventModes[value]);
    } else if (function == PSTriggerOverlap) {
            status |= PvAttrEnumSet(this->PvHandle, "FrameStartTriggerOverlap", PSTriggerOverlapModes[value]);
    } else if (function == PSTriggerSoftware) {
            status |= PvCommandRun(this->PvHandle, "FrameStartTriggerSoftware");
    } else if (function == PSSyncOut1Mode) {
            status |= PvAttrEnumSet(this->PvHandle, "SyncOut1Mode", PSSyncOutModes[value]);
    } else if (function == PSSyncOut2Mode) {
            status |= PvAttrEnumSet(this->PvHandle, "SyncOut2Mode", PSSyncOutModes[value]);
    } else if (function == PSSyncOut3Mode) {
            status |= PvAttrEnumSet(this->PvHandle, "SyncOut3Mode", PSSyncOutModes[value]);
            if (status == ePvErrNotFound) status = 0;
    } else if (function == PSSyncOut1Level) {
            status |= PvAttrUint32Get(this->PvHandle, "SyncOutGpoLevels", &syncs);
            syncs = (syncs & ~0x01) | ((value<<0) & 0x01);
            status |= PvAttrUint32Set(this->PvHandle, "SyncOutGpoLevels", syncs);
    } else if (function == PSSyncOut2Level) {
            status |= PvAttrUint32Get(this->PvHandle, "SyncOutGpoLevels", &syncs);
            syncs = (syncs & ~0x02) | ((value<<1) & 0x02);
            status |= PvAttrUint32Set(this->PvHandle, "SyncOutGpoLevels", syncs);
    } else if (function == PSSyncOut3Level) {
            status |= PvAttrUint32Get(this->PvHandle, "SyncOutGpoLevels", &syncs);
            syncs = (syncs & ~0x04) | ((value<<2) & 0x04);
            status |= PvAttrUint32Set(this->PvHandle, "SyncOutGpoLevels", syncs);
    } else if (function == PSSyncOut1Invert) {
            status |= PvAttrEnumSet(this->PvHandle, "SyncOut1Invert", value ? "On":"Off");
    } else if (function == PSSyncOut2Invert) {
            status |= PvAttrEnumSet(this->PvHandle, "SyncOut2Invert", value ? "On":"Off");
    } else if (function == PSSyncOut3Invert) {
            status |= PvAttrEnumSet(this->PvHandle, "SyncOut3Invert", value ? "On":"Off");
            if (status == ePvErrNotFound) status = 0;
    } else if (function == PSStrobe1Mode) {
            status |= PvAttrEnumSet(this->PvHandle, "Strobe1Mode", PSStrobeModes[value]);
    } else if (function == PSStrobe1CtlDuration) {
            status |= PvAttrEnumSet(this->PvHandle, "Strobe1ControlledDuration", value ? "On":"Off");
    } else if ((function == NDDataType) ||
               (function == NDColorMode)) {
            status = setPixelFormat();
    } else if (function == PSResetTimer) {
            status = syncTimer();
    } else if ( function == PSExposureMode ) {
            status = PvAttrEnumSet(this->PvHandle, "ExposureMode", PSExposureModes[value]);
    } else if ( function == PSGainMode ) {
            status = PvAttrEnumSet(this->PvHandle, "GainMode", PSGainModes[value]);
    } else {
            /* If this is not a parameter we have handled call the base class */
            if (function < FIRST_PS_PARAM) status = ADDriver::writeInt32(pasynUser, value);
    }
    
    /* Read the camera parameters and do callbacks */
    status |= readParameters();    
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s: error, status=%d function=%d, value=%d\n", 
              driverName, functionName, status, function, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, value=%d\n", 
              driverName, functionName, function, value);
    return((asynStatus)status);
}

/** Called when asyn clients call pasynFloat64->write().
  * This function performs actions for some parameters, including ADAcquireTime, ADGain, etc.
  * For all parameters it sets the value in the parameter library and calls any registered callbacks..
  * \param[in] pasynUser pasynUser structure that encodes the reason and address.
  * \param[in] value Value to write. */
asynStatus prosilica::writeFloat64(asynUser *pasynUser, epicsFloat64 value)
{
    int function = pasynUser->reason;
    int status = asynSuccess;
    const char *paramName;
    static const char *functionName = "writeFloat64";

    getParamName(function, &paramName);
    /* Set the parameter and readback in the parameter library.  This may be overwritten when we read back the
     * status at the end, but that's OK */
    status |= setDoubleParam(function, value);

    if (function == ADAcquireTime) {
        /* Prosilica uses integer microseconds */
        status |= PvAttrUint32Set(this->PvHandle, "ExposureValue", (tPvUint32)(value * 1e6));
    } else if (function == ADAcquirePeriod) {
        /* Prosilica uses a frame rate in Hz */
        if (value == 0.) value = .01;
        status |= PvAttrFloat32Set(this->PvHandle, "FrameRate", (tPvFloat32)(1./value));
    } else if (function == ADGain) {
        /* Prosilica uses an integer value */
        status |= PvAttrUint32Set(this->PvHandle, "GainValue", (tPvUint32)(value));
    } else if (function == PSTriggerDelay) {
        /* Prosilica uses integer microseconds */
        status |= PvAttrUint32Set(this->PvHandle, "FrameStartTriggerDelay", (tPvUint32)(value*1e6));
    } else if (function == PSStrobe1Delay) {
        /* Prosilica uses integer microseconds */
        status |= PvAttrUint32Set(this->PvHandle, "Strobe1Delay", (tPvUint32)(value*1e6));
    } else if (function == PSStrobe1Duration) {
        /* Prosilica uses integer microseconds */
        status |= PvAttrUint32Set(this->PvHandle, "Strobe1Duration", (tPvUint32)(value*1e6));
    } else {
        /* If this is not a parameter we have handled call the base class */
        if (function < NUM_PS_PARAMS) status = ADDriver::writeFloat64(pasynUser, value);
    }

    /* Read the camera parameters and do callbacks */
    status |= readParameters();
    if (status) 
        asynPrint(pasynUser, ASYN_TRACE_ERROR, 
              "%s:%s: error, status=%d function=%d, name=%s, value=%f\n", 
              driverName, functionName, status, function, paramName, value);
    else        
        asynPrint(pasynUser, ASYN_TRACEIO_DRIVER, 
              "%s:%s: function=%d, name=%s, value=%f\n", 
              driverName, functionName, function, paramName, value);
    return((asynStatus)status);
}

/** Report status of the driver.
  * Prints details about the driver if details>0.
  * It then calls the ADDriver::report() method.
  * \param[in] fp File pointed passed by caller where the output is written to.
  * \param[in] details If >0 then driver details are printed.
  */
void prosilica::report(FILE *fp, int details)
{
    tPvCameraInfoEx cameraInfo[20], *pInfo; 
    int i;
    unsigned long numReturned, numTotal, versionMajor, versionMinor;
    
    numReturned = PvCameraListEx(cameraInfo, 20, &numTotal, sizeof(tPvCameraInfoEx));

    fprintf(fp, "Prosilica camera %s Unique ID=%d\n", 
            this->portName, (int)this->uniqueId);
    pInfo = &this->PvCameraInfo;
    if (details > 0) {
        PvVersion(&versionMajor, &versionMinor);
        fprintf(fp, "  PvAPI version:     %ld.%ld\n", versionMajor, versionMinor);
        fprintf(fp, "  ID:                %lu\n", pInfo->UniqueId);
        fprintf(fp, "  IP address:        %s\n",  this->IPAddress);
        fprintf(fp, "  Serial number:     %s\n",  pInfo->SerialNumber);
        fprintf(fp, "  Camera name:       %s\n",  pInfo->CameraName);
        fprintf(fp, "  Model:             %s\n",  pInfo->ModelName);
        fprintf(fp, "  Firmware version:  %s\n",  pInfo->FirmwareVersion);
        fprintf(fp, "  Access flags:      %lx\n", pInfo->PermittedAccess);
        fprintf(fp, "  Sensor type:       %s\n",  this->sensorType);
        fprintf(fp, "  Sensor bits:       %d\n",  (int)this->sensorBits);
        fprintf(fp, "  Sensor width:      %d\n",  (int)this->sensorWidth);
        fprintf(fp, "  Sensor height:     %d\n",  (int)this->sensorHeight);
        fprintf(fp, "  Frame buffer size: %d\n",  (int)this->PvFrames[0].ImageBufferSize);
        fprintf(fp, "  Time stamp freq:   %d\n",  (int)this->timeStampFrequency);
        fprintf(fp, "  maxPvAPIFrames:    %d\n",  (int)this->maxPvAPIFrames_);
        fprintf(fp, "\n");
        fprintf(fp, "List of all Prosilica cameras found (total=%d):\n", (int)numReturned);
        for (i=0; i<(int)numReturned; i++) {
            pInfo = &cameraInfo[i];
            fprintf(fp, "  ID:                %lu\n", pInfo->UniqueId);
            fprintf(fp, "  Serial number:     %s\n",  pInfo->SerialNumber);
            fprintf(fp, "  Camera name:       %s\n",  pInfo->CameraName);
            fprintf(fp, "  Model:             %s\n",  pInfo->ModelName);
            fprintf(fp, "  Firmware version:  %s\n",  pInfo->FirmwareVersion);
            fprintf(fp, "  Access flags:      %lx\n", pInfo->PermittedAccess);
            fprintf(fp, "\n");
        }
    }

    /* Call the base class method */
    ADDriver::report(fp, details);
}


extern "C" int prosilicaConfig(char *portName, /* Port name */
                               const char *cameraId,   /* Unique ID #, or IP address or IP name of this camera. */
                               int maxBuffers, size_t maxMemory,
                               int priority, int stackSize, int maxPvAPIFrames)
{
    new prosilica(portName, cameraId, maxBuffers, maxMemory, priority, stackSize, maxPvAPIFrames);
    return(asynSuccess);
}   


/** Constructor for Prosilica driver; most parameters are simply passed to ADDriver::ADDriver.
  * After calling the base class constructor this method creates a thread to collect the detector data, 
  * and sets reasonable default values for the parameters defined in this class, asynNDArrayDriver and ADDriver.
  * \param[in] portName The name of the asyn port driver to be created.
  * \param[in] cameraId The uniqueId, IP address or IP DNS name  of the camera to be connected to this driver.
  * \param[in] maxBuffers The maximum number of NDArray buffers that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited number of buffers.
  * \param[in] maxMemory The maximum amount of memory that the NDArrayPool for this driver is 
  *            allowed to allocate. Set this to -1 to allow an unlimited amount of memory.
  * \param[in] priority The thread priority for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] stackSize The stack size for the asyn port driver thread if ASYN_CANBLOCK is set in asynFlags.
  * \param[in] maxPvAPIFrames The number of frame buffers to use in the PvAPI library driver. Default=MAX_PVAPI_FRAMES=2.
  */
prosilica::prosilica(const char *portName, const char *cameraId, int maxBuffers, size_t maxMemory,
                     int priority, int stackSize, int maxPvAPIFrames)
    : ADDriver(portName, 1, NUM_PS_PARAMS, maxBuffers, maxMemory, 
               0, 0,               /* No interfaces beyond those set in ADDriver.cpp */
               ASYN_CANBLOCK, 0,   /* ASYN_CANBLOCK=1, ASYN_MULTIDEVICE=0, autoConnect=1 */
               priority, stackSize), 
      PvHandle(NULL), maxPvAPIFrames_(maxPvAPIFrames), framesRemaining(0) 

{
    int status = asynSuccess;
    static const char *functionName = "prosilica";
    cameraNode *pNode = new cameraNode;

    this->cameraId = epicsStrDup(cameraId);
    
    // If this is the first camera we need to initialize the camera list
    if (!cameraList) {
       cameraList = new ELLLIST;
       ellInit(cameraList);
    }
    pNode->pCamera = this;
    ellAdd(cameraList, (ELLNODE *)pNode);
    
    createParam(PSReadStatisticsString,    asynParamInt32,    &PSReadStatistics);
    createParam(PSBayerConvertString,      asynParamInt32,    &PSBayerConvert);
    createParam(PSGainModeString,          asynParamInt32,    &PSGainMode);
    createParam(PSExposureModeString,      asynParamInt32,    &PSExposureMode);
    createParam(PSDriverTypeString,        asynParamOctet,    &PSDriverType);
    createParam(PSFilterVersionString,     asynParamOctet,    &PSFilterVersion);
    createParam(PSTimestampTypeString,     asynParamInt32,    &PSTimestampType);
    createParam(PSResetTimerString,        asynParamInt32,    &PSResetTimer);
    createParam(PSFrameRateString,         asynParamFloat64,  &PSFrameRate);
    createParam(PSByteRateString,          asynParamInt32,    &PSByteRate);
    createParam(PSPacketSizeString,        asynParamInt32,    &PSPacketSize);
    createParam(PSFramesCompletedString,   asynParamInt32,    &PSFramesCompleted);
    createParam(PSFramesDroppedString,     asynParamInt32,    &PSFramesDropped);
    createParam(PSPacketsErroneousString,  asynParamInt32,    &PSPacketsErroneous);
    createParam(PSPacketsMissedString,     asynParamInt32,    &PSPacketsMissed);
    createParam(PSPacketsReceivedString,   asynParamInt32,    &PSPacketsReceived);
    createParam(PSPacketsRequestedString,  asynParamInt32,    &PSPacketsRequested);
    createParam(PSPacketsResentString,     asynParamInt32,    &PSPacketsResent);
    createParam(PSBadFrameCounterString,   asynParamInt32,    &PSBadFrameCounter);
    createParam(PSTriggerDelayString,      asynParamFloat64,  &PSTriggerDelay);
    createParam(PSTriggerEventString,      asynParamInt32,    &PSTriggerEvent);
    createParam(PSTriggerOverlapString,    asynParamInt32,    &PSTriggerOverlap);
    createParam(PSTriggerSoftwareString,   asynParamInt32,    &PSTriggerSoftware);
    createParam(PSSyncIn1LevelString,      asynParamInt32,    &PSSyncIn1Level);
    createParam(PSSyncIn2LevelString,      asynParamInt32,    &PSSyncIn2Level);
    createParam(PSSyncOut1ModeString,      asynParamInt32,    &PSSyncOut1Mode);
    createParam(PSSyncOut1LevelString,     asynParamInt32,    &PSSyncOut1Level);
    createParam(PSSyncOut1InvertString,    asynParamInt32,    &PSSyncOut1Invert);
    createParam(PSSyncOut2ModeString,      asynParamInt32,    &PSSyncOut2Mode);
    createParam(PSSyncOut2LevelString,     asynParamInt32,    &PSSyncOut2Level);
    createParam(PSSyncOut2InvertString,    asynParamInt32,    &PSSyncOut2Invert);
    createParam(PSSyncOut3ModeString,      asynParamInt32,    &PSSyncOut3Mode);
    createParam(PSSyncOut3LevelString,     asynParamInt32,    &PSSyncOut3Level);
    createParam(PSSyncOut3InvertString,    asynParamInt32,    &PSSyncOut3Invert);
    createParam(PSStrobe1ModeString,       asynParamInt32,    &PSStrobe1Mode);
    createParam(PSStrobe1DelayString,      asynParamFloat64,  &PSStrobe1Delay);
    createParam(PSStrobe1CtlDurationString,asynParamInt32,    &PSStrobe1CtlDuration);
    createParam(PSStrobe1DurationString,   asynParamFloat64,  &PSStrobe1Duration);

    /* There is a conflict with readline use of signals, don't use readline signal handlers */
#ifdef linux
    rl_catch_signals = 0;
#endif

    /* Set default value of maxPvAPIFrames_ if it is zero */
    if (maxPvAPIFrames_ == 0) maxPvAPIFrames_ = MAX_PVAPI_FRAMES;
    /* Create the PvFrames buffer.  Note that these structures must be set to 0! */
    PvFrames = (tPvFrame *)calloc(maxPvAPIFrames_, sizeof(tPvFrame));
    

    /* Initialize the Prosilica PvAPI library 
     * We get an error if we call this twice, so we need a global flag to see if 
     * it's already been done.*/
    if (!PvApiInitialized) {
        status = PvInitialize();
        if (status) {
            printf("%s:%s: ERROR: PvInitialize failed, status=%d\n", 
            driverName, functionName, status);
            return;
        }

        tPvErr errCode;
        // register camera connection callback
        if ((errCode = PvLinkCallbackRegister(cameraLinkCallback,ePvLinkAdd,NULL)) != ePvErrSuccess)
           printf("PvLinkCallbackRegister err: %u\n", errCode);

       // register camera disconnection callback
       if((errCode = PvLinkCallbackRegister(cameraLinkCallback,ePvLinkRemove,NULL)) != ePvErrSuccess)
           printf("PvLinkCallbackRegister err: %u\n", errCode);

        PvApiInitialized = 1;
    }

    /* Need to wait a short while for the PvAPI library to find the cameras */
    /* (0.2 seconds is not long enough in 1.24) */
    epicsThreadSleep(1.0);
 
    if ( this->PvHandle == NULL ) {
        /* Try to connect to the camera.  
         * It is not a fatal error if we cannot now, the camera may be off or owned by
         * someone else.  It may connect later. */
        this->lock();
        status = connectCamera();
        this->unlock();
        if (status) {
            printf("%s:%s: cannot connect to camera %s, manually connect when available.\n", 
                   driverName, functionName, cameraId);
        }
    }
 
    /* Register the shutdown function for epicsAtExit */
    epicsAtExit(shutdown, (void*)this);
}

/* Code for iocsh registration */
static const iocshArg prosilicaConfigArg0 = {"Port name", iocshArgString};
static const iocshArg prosilicaConfigArg1 = {"Camera Id (unique ID, IP address, or IP name", iocshArgString};
static const iocshArg prosilicaConfigArg2 = {"maxBuffers", iocshArgInt};
static const iocshArg prosilicaConfigArg3 = {"maxMemory", iocshArgInt};
static const iocshArg prosilicaConfigArg4 = {"priority", iocshArgInt};
static const iocshArg prosilicaConfigArg5 = {"stackSize", iocshArgInt};
static const iocshArg prosilicaConfigArg6 = {"maxPvAPIFrames", iocshArgInt};
static const iocshArg * const prosilicaConfigArgs[] = {&prosilicaConfigArg0,
                                                       &prosilicaConfigArg1,
                                                       &prosilicaConfigArg2,
                                                       &prosilicaConfigArg3,
                                                       &prosilicaConfigArg4,
                                                       &prosilicaConfigArg5,
                                                       &prosilicaConfigArg6};
static const iocshFuncDef configprosilica = {"prosilicaConfig", 7, prosilicaConfigArgs};
static void configprosilicaCallFunc(const iocshArgBuf *args)
{
    prosilicaConfig(args[0].sval, args[1].sval, args[2].ival, 
                    args[3].ival, args[4].ival, args[5].ival, args[6].ival);
}


static void prosilicaRegister(void)
{

    iocshRegister(&configprosilica, configprosilicaCallFunc);
}

extern "C" {
epicsExportRegistrar(prosilicaRegister);
}
