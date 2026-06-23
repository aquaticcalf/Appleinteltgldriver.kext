/*
 * AppleIntelTGLIOAcceleratorClients.h - EXACT Apple Client Architecture
 *
 * This implements the REAL Apple IOAcceleratorFamily2 architecture
 * based on reverse engineering of Apple's actual code.
 *
 * Client Types (from newUserClient() at offset 0x3f27a in IOAcceleratorFamily2):
 * Type 0:  IOAccelSurface
 * Type 1:  IOAccelContext2 (GL/Metal Context)
 * Type 2:  IOAccelGLContext2 (OpenGL Context)
 * Type 3:  IOAccel2DContext2 (WindowServer 2D Context)
 * Type 4:  IOAccelDisplayPipeUserClient2
 * Type 5:  IOAccelDevice2 (Device Configuration)
 * Type 6:  IOAccelSharedUserClient2 (Shared Resources)
 * Type 7:  IOAccelVideoContext (Video Context)
 * Type 8:  IOAccelCommandQueue (Metal Command Queues)
 * Type 9:  IOAccelGLDrawableUserClient
 * Type 32: IOAccelSurfaceMTL (Metal Surface)
 */

#ifndef IntelIOAcceleratorClients_h
#define IntelIOAcceleratorClients_h

#include <IOKit/IOUserClient.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTypes.h>
#include <libkern/OSAtomic.h>
#include <sys/types.h>


// MARK: - Forward Declarations


class IntelIOAccelerator;
class AppleIntelTGLController;
class IntelContext;
class IntelRequest;
class IntelGEMObject;
class IOMemoryDescriptor;
class IntelIOFramebuffer;
class IntelBlitter;
class IntelRingBuffer;


// MARK: - Apple Client Type Enum (Exact)


enum IOAccelClientType {
    kIOAccelClientTypeSurface          = 0,   // IOAccelSurface
    kIOAccelClientTypeContext          = 1,   // IOAccelContext2 (GL/Metal)
    kIOAccelClientTypeGLContext        = 2,   // IOAccelGLContext2 (OpenGL)
    kIOAccelClientType2DContext        = 3,   // IOAccel2DContext2 (WindowServer!)
    kIOAccelClientTypeDisplayPipe      = 4,   // IOAccelDisplayPipeUserClient2
    kIOAccelClientTypeDevice           = 5,   // IOAccelDevice2 
    kIOAccelClientTypeShared           = 6,   // IOAccelSharedUserClient2
    kIOAccelClientTypeVideoContext     = 7,   // IOAccelVideoContext
    kIOAccelClientTypeCommandQueue     = 8,   // IOAccelCommandQueue 
    kIOAccelClientTypeGLDrawable      = 9,   // IOAccelGLDrawableUserClient
    kIOAccelClientTypeSurfaceMTL       = 32,  // IOAccelSurfaceMTL
};


// MARK: - Apple Data Structures (Exact from Reverse Engineering)



// MARK: - IntelHwInfoRec (EXACT struct Apple Metal bundle reads)

// From reverse engineering AppleIntelTGLGraphicsMTLDriver binary
// This is what selector 0 (get_config) MUST return for Metal to work

struct IntelHwCapsInfo {
    uint32_t fRevisionID;           // GPU revision ID
    uint32_t fRevisionID_PCH;       // PCH revision ID
    uint32_t fDeviceID;             // PCI device ID (0x9A49 for TGL)
    uint32_t fDeviceID_PCH;         // PCH device ID
    uint32_t fGpuSku;              // SKU identifier (GT2 = 0x0B)
    uint32_t fExecutionUnitCount;   // EU count (80 for TGL-U GT2)
    uint32_t fPixelShaderMinThreads;
    uint32_t fPoshShaderMaxThreads;
    uint32_t fVertexShaderMaxThreads;
    uint32_t fHullShaderMaxThreads;
    uint32_t fDomainShaderMaxThreads;
    uint32_t fGeometryShaderMaxThreads;
    uint32_t fPixelShaderMaxThreads;
    uint32_t fPixelShaderMinThreadsReserved;
    uint32_t fMaxPixelShaderDispatchers;
    uint32_t fPixelShaderDispatchers;
    uint32_t fMaxFrequencyInMhz;    // GPU max clock MHz (1300 for TGL)
    uint32_t fMinFrequencyInMhz;    // GPU min clock MHz
    uint32_t fNumSubSlices;        // subslice count (10 for TGL GT2)
    uint32_t fNumSlices;           // slice count (1 for TGL)
    uint32_t fL3CacheSizeInKb;     // L3 cache size in KB (16384 = 16MB)
    uint32_t fL3BankCount;
    uint32_t fMaxFillRate;
    uint32_t fMaxEuPerSubSlice;    // EUs per subslice (8 for TGL)
    uint32_t fDisplayTileMode;
    uint32_t fNumThreadsPerEU;     // threads per EU (7 for Gen12)
    uint32_t fMinThreadsPerPSD;
    uint32_t fMaxThreadsPerPSD;
    uint32_t fMaxFillRatePerSlice;
    struct {
        uint32_t fHasEDRAM     : 1;
        uint32_t fSliceShutdown: 1;
        uint32_t fCanSampleHiZ : 1;
    } fFlags;
} __attribute__((packed));


// MARK: - _WA_TABLE (Tiger Lake Gen12 Workaround Table)

// Workaround table for Gen12 hardware issues
// Based on reverse engineering of AppleIntelTGLGraphicsMTLDriver
// Each uint32 represents a group of workarounds as bitfields

struct IntelWATable {
    uint32_t flags[48];  // 48 x 32-bit = 192 bytes of WA flags
} __attribute__((packed));


// MARK: - _SKU_FEATURE_TABLE (Tiger Lake SKU Features)

// SKU-specific feature enablement table
// Contains feature flags per SKU configuration

struct IntelSKUFeatureTable {
    uint32_t features[48];  // 48 x 32-bit = 192 bytes of feature flags
} __attribute__((packed));


// MARK: - _SKU_FEATURE_TABLE_EX (Extended SKU Features)

// Extended SKU feature table for additional capabilities
// Contains extended feature flags

struct IntelSKUFeatureTableEx {
    uint32_t features[25];  // 25 x 32-bit = 100 bytes (0x64)
} __attribute__((packed));


// MARK: - IntelHwInfoRec (EXACT binary layout from reverse engineering)

// Total size: 0x260 (608 bytes)
// Layout from AppleIntelTGLGraphicsMTLDriver binary:
//   0x00: hwCapsInfo (IntelHwCapsInfo) - 120 bytes
//   0x78: skuFeatureTable (IntelSKUFeatureTable) - 192 bytes
//   0x138: waTable (IntelWATable) - 192 bytes
//   0x1f8: skuFeatureTableEx (IntelSKUFeatureTableEx) - 100 bytes

struct IntelHwInfoRec {
    IntelHwCapsInfo hwCapsInfo;                    // 120 bytes at offset 0x00
    uint8_t reserved1[0x78 - sizeof(IntelHwCapsInfo)];  // Pad to 0x78 (36 bytes)
    IntelSKUFeatureTable skuFeatureTable;          // 192 bytes at offset 0x78
    IntelWATable waTable;                         // 192 bytes at offset 0x138
    IntelSKUFeatureTableEx skuFeatureTableEx;    // 100 bytes at offset 0x1f8
} __attribute__((packed));

// Type 5 (Device) - Selector 0: get_config output
// Returns 0x260 (608) bytes of hardware info
struct IOAccelDeviceConfigData {
    IntelHwInfoRec info;
} __attribute__((packed));

// Type 5 (Device) - Selector 7: get_device_info output (24 bytes)
struct IOAccelDeviceInfoData {
    uint32_t vendorID;           // PCI vendor ID
    uint32_t deviceID;           // PCI device ID
    uint32_t revisionID;         // PCI revision ID
    uint32_t reserved[3];        // Padding
} __attribute__((packed));

// Type 1/3/7 (Context) - Selector 2: submit_data_buffers input (136+ bytes)
struct IOAccelContextSubmitDataBuffersIn {
    uint64_t bufferAddress;      // GPU command buffer address
    uint32_t bufferSize;         // Command buffer size
    uint32_t contextID;         // GPU context ID
    uint64_t fenceAddress;       // Fence address
    uint32_t fenceValue;         // Fence value
    uint32_t flags;             // Submission flags
    uint32_t queueID;           // Command queue ID
    uint32_t priority;          // Command priority
    uint64_t timestamp;         // Submission timestamp
    uint32_t commandCount;       // Number of commands
    uint32_t reserved[21];      // Padding to 136 bytes
} __attribute__((packed));

// Type 1/3/7 (Context) - Selector 2: submit_data_buffers output (variable)
struct IOAccelContextSubmitDataBuffersOut {
    uint32_t status;            // Submission status
    uint32_t sequenceNumber;    // GPU sequence number
    uint32_t fenceID;          // Fence ID
    uint64_t completionTime;    // Estimated completion time
    uint32_t reserved[3];      // Padding
} __attribute__((packed));

// Type 1/3/7 (Context) - Selector 1: set_client_info input (variable)
struct IOAccelClientInfo {
    uint32_t clientType;        // Client type
    uint32_t processID;         // Process ID
    uint32_t threadID;          // Thread ID
    uint64_t clientID;         // Client ID
    char processName[32];       // Process name
    uint32_t priority;          // Client priority
    uint32_t reserved[7];       // Padding
} __attribute__((packed));

// Type 6 (Shared) - Shared resource structure
struct IOAccelSharedResource {
    uint32_t resourceID;        // Resource identifier
    uint32_t resourceType;      // Resource type (surface, buffer, etc.)
    uint64_t gpuAddress;        // GPU address of resource
    uint64_t size;              // Size in bytes
    uint32_t width;             // Width (for surfaces)
    uint32_t height;            // Height (for surfaces)
    uint32_t format;            // Pixel format (FourCC)
    uint32_t flags;             // Resource flags
    uint32_t refCount;          // Reference count
    uint32_t ownerPID;          // Owner process ID
    char ownerName[32];         // Owner process name
    uint32_t reserved[4];       // Padding
} __attribute__((packed));

// Type 6 (Shared) - Surface lock request
struct IOAccelSharedSurfaceLock {
    uint32_t surfaceID;         // Surface to lock
    uint32_t lockType;          // Lock type (read/write)
    uint32_t timeoutMs;         // Timeout in milliseconds
    uint32_t requestingPID;     // Requesting process ID
    char requestingProcess[32]; // Requesting process name
    uint32_t reserved[4];       // Padding
} __attribute__((packed));


// MARK: - Base Class for All Apple Clients


class IntelIOAcceleratorClientBase : public IOUserClient {
    OSDeclareDefaultStructors(IntelIOAcceleratorClientBase)
    
protected:
    //  CRITICAL: Apple's Atomic Safety Wrappers (from binary offset 0x52680, 0x44978)
    // These prevent kernel panics when client crashes during GPU operation
    volatile SInt32 activeCallCount;     // Offset 0x1328 (Surface) / 0x160 (Shared) - OSIncrementAtomic counter
    volatile bool isTerminated;          // Offset 0x1334 (Surface) / 0x10d (Shared) - Termination flag
    volatile bool shouldDetach;          // Offset 0x1335 (Surface) / 0x10e (Shared) - Cleanup pending flag
    IOLock* terminationLock;             // Offset 0x1320 (Surface) / 0x158 (Shared) - Cleanup coordination
    
public:
    IntelIOAccelerator* accelerator;
    AppleIntelTGLController* controller;
    task_t owningTask;
    uint32_t clientType;
    uint32_t clientID;
    
    virtual bool initWithTask(task_t owningTask, void* securityID, UInt32 type) override;
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual void free() override;
    
    //  CRITICAL: Client termination and cleanup (Apple's pattern)
    virtual IOReturn clientClose() override;
    virtual IOReturn clientDied() override;
    
    // Accessors
    IntelIOAccelerator* getAccelerator() { return accelerator; }
    AppleIntelTGLController* getController() { return controller; }
    uint32_t getClientType() { return clientType; }
    uint32_t getClientID() { return clientID; }
    
    // THE CRITICAL METHOD: Apple's dispatch mechanism
    // NOTE: Returns IOExternalMethod* (not Dispatch*) - Apple's signature
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override = 0;
    
protected:
    //  CRITICAL: Subclass must implement cleanup logic (detach_surface / detach_shared)
    virtual void performTerminationCleanup() = 0;
};


// MARK: - Type 5: IOAccelDevice2 Client (Device Configuration)


class IntelDeviceClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelDeviceClient)
    
private:
    static IOExternalMethodDispatch sDeviceMethods[25];  // Selectors 0-24 (TGL: 0-9 base + 10-24 vendor)
    
public:
    // CRITICAL: Apple uses externalMethod dispatch
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments* arguments,
                                    IOExternalMethodDispatch* dispatch,
                                    OSObject* target,
                                    void* reference) override;
    
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // Apple's exact selector methods (from sDeviceMethods at 0x70bb0)
    static IOReturn s_get_config(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_name(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_event_machine(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_surface_info(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_stereo(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_next_global_object_id(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_current_trace_filter(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_device_info(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_next_gid_group(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_api_property(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
protected:
    // Device client has minimal cleanup (no GPU state)
    virtual void performTerminationCleanup() override { /* No GPU state to clean */ }
    
private:
    IOReturn doGet_config(IOAccelDeviceConfigData* output);
    IOReturn doGet_name(char* output, uint32_t maxSize);
    IOReturn doGet_device_info(IOAccelDeviceInfoData* output);
};


// MARK: - Type 1/3/7: IOAccelContext2 Client (GPU Command Submission)


class IntelContextClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelContextClient)
    
protected:
    static IOExternalMethodDispatch sContextMethods[8];   // Selectors 0-7
    static IOExternalMethodDispatch sEnableBlockFencesDispatch;  //  Selector 6 special handling
    
    //  GPU Hang Recovery and Request Tracking
    struct TrackedRequest {
        IntelRequest* request;
        uint32_t fence;
        uint64_t completionTag;
        uint64_t submissionTime;
        uint32_t timeoutMs;
        uint32_t priority;
    };
    
    OSArray* activeRequests;        // Active submitted requests
    IOLock* requestsLock;           // Lock for request tracking
    uint32_t nextRequestID;         // Next request tracking ID
    IntelContext* gpuContext;       // Per-client GPU context (protected for subclass access)
    bool backgroundRendering;
    uint32_t currentPriority;
    IOAccelClientInfo cachedClientInfo;
    bool hasClientInfo;
    
    //  CRITICAL: Apple's context enabled flag (offset 0x698 in binary)
    volatile bool contextEnabled;

public:
    virtual bool start(IOService* provider) override;
    virtual void stop(IOService* provider) override;
    virtual void free() override;

    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // CRITICAL: Override externalMethod for proper IOExternalMethodDispatch handling
    virtual IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments* arguments,
        IOExternalMethodDispatch* dispatch = 0,
        OSObject* target = 0,
        void* reference = 0) override;
    
    // Apple's exact selector methods (from sContextMethods at 0x6f3a0)
    static IOReturn s_finish(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_client_info(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_submit_data_buffers(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_data_buffer(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_reclaim_resources(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_finish_fence_event(OSObject* target, void* ref, IOExternalMethodArguments* args);
    //  CRITICAL: Selector 6 is enable_block_fences, NOT set_background_rendering!
    static IOReturn s_enable_block_fences(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_background_rendering(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_submit_data_buffers_fg(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
protected:
    //  CRITICAL: Implement termination cleanup (Apple's pattern)
    virtual void performTerminationCleanup() override;
    
private:
    //  THE CRITICAL METHOD: GPU Command Submission
    IOReturn doSubmitDataBuffers(const IOAccelContextSubmitDataBuffersIn* input,
                                IOAccelContextSubmitDataBuffersOut* output,
                                uint32_t inputSize, uint32_t outputSize);
    
    //  WindowServer 2D Command Submission (Type 3 Client)
    IOReturn doSubmit2DCommands(const IOAccelContextSubmitDataBuffersIn* input,
                                IOAccelContextSubmitDataBuffersOut* output,
                                uint32_t inputSize, uint32_t outputSize);
    
    IOReturn doSetClientInfo(const IOAccelClientInfo* clientInfo, uint32_t size);
    IOReturn doFinish();
    IOReturn doReclaimResources();
    IOReturn doEnableBlockFences(io_user_reference_t* asyncRef);
    
    //  GPU Hang Recovery Methods (protected for subclass access)
    IOReturn trackSubmittedRequest(IntelRequest* request, uint32_t fence, uint64_t completionTag);
    IOReturn removeCompletedRequest(uint32_t fence);
    IOReturn detectAndHandleHungRequests();
    IOReturn recoverFromGPUHang();
    void cleanupHungRequests();
    bool isRequestHung(TrackedRequest* tracked);
    
public:
    // Periodic maintenance
    IOReturn performMaintenance();
};


// MARK: - Type 6: IOAccelSharedUserClient2 Client (Shared Resources)


class IntelSharedClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelSharedClient)
    
private:
    static IOExternalMethodDispatch sSharedMethods[29];  // Selectors 0-28 (TGL: 0-19 base + 20-28 vendor)
    
public:
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // CRITICAL: Override externalMethod for proper IOExternalMethodDispatch handling
    virtual IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments* arguments,
        IOExternalMethodDispatch* dispatch = 0,
        OSObject* target = 0,
        void* reference = 0) override;
    
    // Shared resource management methods
    static IOReturn s_create_shared_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_destroy_shared_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_lock_shared_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_unlock_shared_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_shared_surface_info(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_shared_surface_priority(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_duplicate_shared_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_transfer_shared_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    // ... more shared methods
    
protected:
    //  CRITICAL: Implement termination cleanup (Apple's detach_shared pattern)
    virtual void performTerminationCleanup() override;
    
private:
    // Implementation methods
    IOReturn doCreateSharedSurface(IOAccelSharedResource* resource);
    IOReturn doDestroySharedSurface(uint32_t surfaceID);
    IOReturn doLockSharedSurface(const IOAccelSharedSurfaceLock* lock);
    IOReturn doUnlockSharedSurface(uint32_t surfaceID);
    IOReturn doGetSharedSurfaceInfo(uint32_t surfaceID, IOAccelSharedResource* info, uint32_t infoSize);
};


// MARK: - Type 8: IOAccelCommandQueue Client (Metal Command Queues)


// Command queue state structure (Apple's IGAccelCommandQueue)
struct IOAccelCommandQueueState {
    uint32_t queueID;              // Queue identifier
    uint32_t status;               // Queue status (0=idle, 1=active, 2=error)
    uint64_t commandBufferAddress;  // Command buffer GPU address
    uint32_t commandBufferSize;      // Command buffer size
    uint32_t maxCommands;          // Max pending commands
    uint32_t pendingCommands;       // Current pending count
    uint64_t fenceAddress;         // Fence status buffer address
    uint32_t fenceValue;           // Current fence value
    uint32_t flags;                // Queue flags
    uint32_t reserved[6];          // Padding
} __attribute__((packed));

// Command buffer submission structure
struct IOAccelCommandBufferSubmit {
    uint64_t commandBuffer;        // Command buffer address
    uint32_t commandSize;          // Command buffer size
    uint32_t commandCount;         // Number of commands
    uint64_t fenceAddress;         // Fence address
    uint32_t fenceValue;           // Fence initial value
    uint32_t flags;                // Submission flags
    uint32_t priority;             // Command priority
    uint64_t timestamp;            // Submission timestamp
    uint32_t reserved[5];         // Padding to 64 bytes
} __attribute__((packed));

class IntelCommandQueueClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelCommandQueueClient)
    
public:
    //  EXACT Apple command queue method table (from IOAcceleratorFamily2)
    // Apple binary shows 6 selectors with stride 3 = 18 entries (not 24!)
    static IOExternalMethodDispatch sCommandQueueMethods[18];  // 6 selectors x 3 stride
    
    // Queue state
    bool commandQueueEnabled;        // Command queue enabled flag (offset 0x588 in Apple code)
    IOAccelCommandQueueState queueState;
    IOBufferMemoryDescriptor* sharedStateBuffer;  // Shared memory with userspace
    IOBufferMemoryDescriptor* commandBuffer;     // Command buffer memory
    IOLock* queueLock;
    
    // Command tracking
    OSArray* pendingCommands;
    uint32_t nextSequenceNumber;
    mach_port_t notificationPort;
    uint32_t lastSubmittedSeqno;
    uint32_t lastSubmittedFence;
    uint32_t lastSubmittedStatus;
    uint64_t completionCallback;
    
    // Lifecycle
    virtual bool start(IOService* provider) override;
    
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // CRITICAL: Override externalMethod for proper IOExternalMethodDispatch handling
    virtual IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments* arguments,
        IOExternalMethodDispatch* dispatch = 0,
        OSObject* target = 0,
        void* reference = 0) override;
    
    //  EXACT Apple command queue selectors (from reverse engineering)
    static IOReturn s_set_notification_port(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_submit_command_buffer(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_wait_for_completion(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_command_buffer_status(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_priority_band(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_background(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_completion_callback(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_signal_completion(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_create_command_buffer(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_destroy_command_buffer(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_queue_state(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_flush_queue(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_debug_marker(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_performance_counters(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_hang_timeout(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_recover_from_hang(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Implementation methods
    IOReturn doSetNotificationPort(mach_port_t port);
    IOReturn doSubmitCommandBuffer(const IOAccelCommandBufferSubmit* submit,
                                   uint32_t* outStatus,
                                   uint32_t* outSeqno,
                                   uint32_t* outFence);
    IOReturn doWaitForCompletion(uint32_t fenceID, uint32_t timeoutMs);
    IOReturn doGetCommandBufferStatus(uint32_t bufferID, uint32_t* status, uint32_t* progress);
    IOReturn doSetPriorityBand(uint32_t bandwidth);
    
    //  Metal Command Parsing and Validation
    IOReturn parseMetalCommands(uint64_t commandBuffer, uint32_t commandSize, uint32_t commandCount);
    IOReturn validateMetalCommand(uint64_t commandPtr, uint32_t commandSize);
    IOReturn doSetBackground(uint32_t enable);
    IOReturn doSetCompletionCallback(uint64_t callback);
    IOReturn doSignalCompletion(uint32_t bufferID);
    
protected:
    // Command queue has minimal cleanup (no GPU state persistence)
    virtual void performTerminationCleanup() override;
};


// MARK: - Type 0: IOAccelSurface Client (Surface Management)


// Surface creation structure (Apple's exact format)
struct IOAccelSurfaceCreateData {
    uint32_t width;              // Surface width
    uint32_t height;             // Surface height
    uint32_t format;             // Pixel format (FourCC)
    uint32_t flags;              // Surface flags
    uint64_t size;               // Surface size in bytes
    uint32_t stride;             // Bytes per row
    uint32_t planeCount;          // Number of planes
    uint32_t reserved[6];        // Padding
} __attribute__((packed));

class IntelSurfaceClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelSurfaceClient)
    
private:
    static IOExternalMethodDispatch sSurfaceMethods[19];  // Surface methods (0-18 like Apple)
    static IOExternalMethodDispatch sSurfaceCreate2Method;  // Selector 17 (legacy compat)
    static const uint32_t kMaxSurfaces = 256;
    
    struct SurfaceRecord {
        uint32_t handle;
        uint32_t iosurfaceID;
        OSObject* iosurfaceObj;          //  APPLE: IOSurface object pointer (passed in selector 9)
        mach_port_t iosurfacePort;
        IOMemoryDescriptor* memDesc;    // CRITICAL: Retain for complete() on destroy
        // Scanout mapping cache (to avoid remap/unmap blinking on triple buffering)
        static const uint32_t kScanoutCacheSlots = 3;
        uint32_t scanoutCacheNext;
        uint64_t scanoutCachePhys[kScanoutCacheSlots];
        IOMemoryDescriptor* scanoutCacheMemDesc[kScanoutCacheSlots];
        uint32_t scanoutCacheGttOffset[kScanoutCacheSlots];
        size_t   scanoutCacheGttSize[kScanoutCacheSlots];
        bool     scanoutCachePrepared[kScanoutCacheSlots];
        bool     scanoutCacheHasBinding[kScanoutCacheSlots];

        // Current scanout selection
        uint32_t scanoutGttOffset;      // GGTT offset used for scanout programming
        size_t   scanoutGttSize;        // Size of scanout GGTT mapping
        uint64_t gpuAddress;
        uint64_t physicalAddr;
        uint32_t gttOffset;      // GGTT offset for direct scanout
        size_t gttSize;          // Size of GGTT binding
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint32_t stride;
        uint32_t tiling;         // 0=linear, 1=X, 2=Y (best-effort from Selector 17)
        uint64_t size;
        bool isMapped;
        bool isPrepared;         // True if memDesc->prepare() was called
        bool hasGttBinding;      // True if bound to GGTT for scanout
    };
    
    // Surface tracking
    SurfaceRecord* surfaceArray[kMaxSurfaces];
    uint32_t surfaceCount;
    IOLock* surfacesLock;
    uint32_t nextSurfaceID;

    // Last surface IDs observed on this client instance
    uint32_t lastTrackedSurfaceID;     // From Selector 7
    uint32_t lastTrackedIOSurfaceID;   // From Selector 7
    // Selector 17 updates geometry/backing only; surface selection comes from Selector 7.
    
    //  NEW: Last geometry from Selector 17 for Selector 3
    uint32_t lastWidth;
    uint32_t lastHeight;
    uint32_t lastStride;
    uint32_t lastFormat;
    uint64_t lastGpuAddr;
    
public:
    // Lifecycle
    virtual bool start(IOService* provider) override;
    virtual void free() override;
    
    // CRITICAL: Apple uses externalMethod dispatch, NOT getTargetAndMethodForIndex!
    virtual IOReturn externalMethod(uint32_t selector,
                                    IOExternalMethodArguments* arguments,
                                    IOExternalMethodDispatch* dispatch,
                                    OSObject* target,
                                    void* reference) override;
    
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // Surface methods
    static IOReturn s_create_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_destroy_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_surface_info(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_lock_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_unlock_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_read_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_shape_backing_and_length(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 6
    static IOReturn s_set_shape_backing(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 7
    static IOReturn s_write_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 8
    static IOReturn s_new_resource(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 9
    static IOReturn s_surface_flush(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 10
    static IOReturn s_set_purgeable_state(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_ownership_identity(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_allocation_size(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_state(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_copy_client_surface_list(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_surface_blocking(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 16
    static IOReturn s_set_shape_backing_length_ext(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 17
    static IOReturn s_signal_event(OSObject* target, void* ref, IOExternalMethodArguments* args);  // Selector 18
    
    // Legacy/deprecated selectors (not in Apple's current implementation)
    static IOReturn s_finish_all(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_increment_use_count(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_decrement_use_count(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_get_surface_residency(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_read_lockdown_config(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_set_displayed_vs_rendered_offset(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_create_surface_2(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
private:
    IOReturn doCreateSurface(const IOAccelSurfaceCreateData* create, uint32_t* surfaceID);
    IOReturn doCreateSurface2(IOMemoryDescriptor* memDesc, IOExternalMethodArguments* args, uint32_t* surfaceID);
    IOReturn doDestroySurface(uint32_t surfaceID);
    IOReturn doGetSurfaceInfo(uint32_t surfaceID, void* info, uint32_t infoSize);
    IOReturn doLockSurface(uint32_t surfaceID, uint32_t lockType);
    IOReturn doUnlockSurface(uint32_t surfaceID);
    IOReturn doFinishAll();
    IOReturn doSetShapeBacking(const void* shapeData, uint32_t shapeDataSize);
    IOReturn doSetShapeBackingWithScalars(uint32_t surfaceID, uint32_t iosurfaceID);
    
    //  CRITICAL: IOMemoryDescriptor -> GGTT binding for direct scanout
    uint64_t bindMemoryDescriptorToGGTT(IOMemoryDescriptor* memDesc,
                                        uint32_t* outGttOffset,
                                        size_t* outSize);
    
    uint32_t allocateSurfaceHandle(SurfaceRecord* surface, uint32_t preferredHandle);
    SurfaceRecord* getSurfaceRecord(uint32_t surfaceID);
    void destroySurfaceRecord(uint32_t surfaceID);
    IntelIOFramebuffer* findFramebuffer();
    
    //  NEW: Save surface info from Selector 17 for Selector 3 to retrieve
    void registerSurface(uint32_t surfaceID, uint64_t gpuAddr, uint32_t width,
                         uint32_t height, uint32_t stride, uint32_t format,
                         uint32_t tiling);
    
protected:
    // Surface client cleanup (Apple's detach_surface pattern)
    virtual void performTerminationCleanup() override;
};


// MARK: - Type 2: IOAccelGLContext Client (OpenGL Specific)


class IntelGLContextClient : public IntelContextClient {
    OSDeclareDefaultStructors(IntelGLContextClient)
    
private:
    // GL context state (matching Apple's offsets)
    bool glContextEnabled;  // GL context flag at offset 0x698 in Apple code
    uint32_t swapRectX;     // Swap rectangle at offsets 0x10a8-0x10ae
    uint32_t swapRectY;
    uint32_t swapRectWidth;
    uint32_t swapRectHeight;
    uint32_t swapInterval0; // Swap interval at offsets 0x10b0-0x10b2
    uint32_t swapInterval1;
    
public:
    virtual bool start(IOService* provider) override;
    
    // CRITICAL: Apple's externalMethod with selector offset (selector - 0x100)
    // Apple checks: adjustedSelector = selector - 0x100; if (adjustedSelector > 5) fallback
    virtual IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments* arguments,
        IOExternalMethodDispatch* dispatch = 0,
        OSObject* target = 0,
        void* reference = 0) override;
    
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // Apple's 6 GL-specific selectors (0x100-0x105 = 256-261 decimal)
    // From IOAcceleratorFamily2 offset 0x28133-0x28290, dispatch table at 0x75190
    static IOReturn s_set_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);                      // 0x100 (256)
    static IOReturn s_set_surface_get_config_status(OSObject* target, void* ref, IOExternalMethodArguments* args);    // 0x101 (257)
    static IOReturn s_set_swap_rect(OSObject* target, void* ref, IOExternalMethodArguments* args);                    // 0x102 (258)
    static IOReturn s_set_swap_interval(OSObject* target, void* ref, IOExternalMethodArguments* args);                // 0x103 (259)
    static IOReturn s_set_surface_volatile_state(OSObject* target, void* ref, IOExternalMethodArguments* args);       // 0x104 (260)
    static IOReturn s_read_buffer(OSObject* target, void* ref, IOExternalMethodArguments* args);                      // 0x105 (261)
    
    // GL dispatch table (6 entries at 24-byte stride, matching sGLContextMethodsDispatch)
    static IOExternalMethodDispatch sGLContextMethods[6];
};


// MARK: - Type 4: IOAccelDisplayPipeUserClient2 Client (Display Pipeline)


// Display pipe configuration structure (Apple's exact format)
struct IOAccelDisplayPipeConfig {
    uint32_t displayID;          // Display identifier
    uint32_t width;              // Display width
    uint32_t height;             // Display height
    uint32_t refreshRate;         // Refresh rate in Hz
    uint32_t colorDepth;         // Color depth (bpp)
    uint32_t flags;              // Display flags
    uint64_t framebufferAddress;  // Framebuffer GPU address
    uint32_t framebufferSize;     // Framebuffer size in bytes
    uint32_t reserved[5];        // Padding to 48 bytes
} __attribute__((packed));

// Display mode structure
struct IOAccelDisplayMode {
    uint32_t width;              // Mode width
    uint32_t height;             // Mode height
    uint32_t refreshRate;         // Refresh rate
    uint32_t colorDepth;         // Color depth
    uint32_t flags;              // Mode flags
    uint32_t reserved[3];        // Padding to 32 bytes
} __attribute__((packed));

// MARK: - Type 2/4: IOAccelDisplayPipeUserClient2 (Display Pipe Control)
// Apple's IOAccelDisplayPipeUserClient2 from IOAcceleratorFamily2
// 14 selectors (0-13) for display pipeline configuration, gamma, transactions

class IntelDisplayPipeClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelDisplayPipeClient)
    
private:
    static IOExternalMethodDispatch sDisplayPipeMethods[14];  // Apple's exact 14 selectors
    
    // Display pipe state
    uint32_t pipeIndex;              // Current pipe index (set by selector 0)
    uint32_t currentDisplayMode;
    bool hasEntitlement;             // Display pipe access entitlement
    
public:
    // Lifecycle methods
    virtual bool start(IOService* provider) override;
    virtual void free() override;
    
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // CRITICAL: Override externalMethod for proper IOExternalMethodDispatch handling
    virtual IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments* arguments,
        IOExternalMethodDispatch* dispatch = 0,
        OSObject* target = 0,
        void* reference = 0) override;
    

    // Display Pipe Selector Methods (Apple's EXACT IOAccelDisplayPipeUserClient2)

    
    // Selector 0: set_pipe_index - Set which display pipe to control
    static IOReturn s_set_pipe_index(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 1: get_display_mode_scaler - Get display mode scaler info
    static IOReturn s_get_display_mode_scaler(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 2: get_capabilities_data - Get display capabilities
    static IOReturn s_get_capabilities_data(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 3: request_notify - Request display change notification
    static IOReturn s_request_notify(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 4: transaction_begin - Begin display configuration transaction
    static IOReturn s_transaction_begin(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 5: transaction_set_plane_gamma_table - Set plane gamma table
    static IOReturn s_transaction_set_plane_gamma_table(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 6: transaction_set_pipe_pregamma_table - Set pipe pre-gamma table
    static IOReturn s_transaction_set_pipe_pregamma_table(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 7: transaction_set_pipe_postgamma_table - Set pipe post-gamma table
    static IOReturn s_transaction_set_pipe_postgamma_table(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 8: transaction_end - End display configuration transaction
    static IOReturn s_transaction_end(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 9: transaction_wait - Wait for transaction completion
    static IOReturn s_transaction_wait(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 10: transaction_set_pipe_precsclinearization_vid - Set pre-CSC linearization (video)
    static IOReturn s_transaction_set_pipe_precsclinearization_vid(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 11: transaction_set_pipe_postcscgamma_vid - Set post-CSC gamma (video)
    static IOReturn s_transaction_set_pipe_postcscgamma_vid(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 12: copy_surface - Copy surface data for display
    static IOReturn s_copy_surface(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
    // Selector 13: triage - Diagnostic/triage command
    static IOReturn s_triage(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
private:
    IOReturn doGetDisplayInfo(uint32_t displayID, IOAccelDisplayPipeConfig* info);
    IOReturn doSetDisplayMode(uint32_t displayID, const IOAccelDisplayMode* mode);
    IOReturn doGetDisplayModes(uint32_t displayID, IOAccelDisplayMode* modes, uint32_t* count);
    IOReturn doCreateDisplayPipe(const IOAccelDisplayPipeConfig* config, uint32_t* pipeID);
    IOReturn doDestroyDisplayPipe(uint32_t pipeID);
    IOReturn doSetGammaTable(uint32_t displayID, const uint32_t* gammaTable, uint32_t size);
    IOReturn doGetGammaTable(uint32_t displayID, uint32_t* gammaTable, uint32_t* size);
    IOReturn doSetDisplayCursor(uint32_t displayID, uint32_t x, uint32_t y, uint32_t hotspotX, uint32_t hotspotY);
    IOReturn doGetDisplayCursor(uint32_t displayID, uint32_t* x, uint32_t* y, uint32_t* hotspotX, uint32_t* hotspotY);
    IOReturn doSetVSync(uint32_t displayID, uint32_t enable);
    IOReturn doGetVSyncStatus(uint32_t displayID, uint32_t* enabled);
    IOReturn doSwapDisplayBuffers(uint32_t displayID);
    
protected:
    // Display pipe cleanup (release gamma tables, pipe resources)
    virtual void performTerminationCleanup() override;
};


// MARK: - Type 9: IOAccelGLDrawableUserClient Client (GL Drawable)


class IntelGLDrawableClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelGLDrawableClient)
    
public:
    // Drawable surface tracking
    OSArray* drawableSurfaces;
    IOLock* drawablesLock;
    uint32_t nextDrawableID;
    
    // Lifecycle methods
    virtual bool start(IOService* provider) override;
    virtual void free() override;
    
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
    
    // CRITICAL: Override externalMethod for proper IOExternalMethodDispatch handling
    virtual IOReturn externalMethod(
        uint32_t selector,
        IOExternalMethodArguments* arguments,
        IOExternalMethodDispatch* dispatch = 0,
        OSObject* target = 0,
        void* reference = 0) override;
    
    // Drawable methods
    static IOReturn s_create_drawable(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_destroy_drawable(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_bind_drawable(OSObject* target, void* ref, IOExternalMethodArguments* args);
    static IOReturn s_update_drawable(OSObject* target, void* ref, IOExternalMethodArguments* args);
    
protected:
    // GL drawable cleanup (minimal - drawables released by surfaces)
    virtual void performTerminationCleanup() override;
};


// MARK: - Type 33: IOAccelMemoryInfoUserClient (Memory Debugging)


class IntelMemoryInfoClient : public IntelIOAcceleratorClientBase {
    OSDeclareDefaultStructors(IntelMemoryInfoClient)
    
public:
    virtual IOExternalMethod* getTargetAndMethodForIndex(
        IOService** target, UInt32 selector) override;
        
protected:
    // Memory info has no cleanup (read-only client)
    virtual void performTerminationCleanup() override { /* No state to clean */ }
};


// MARK: - Client Factory Functions (Apple Style)


class IntelClientFactory {
public:
    // Apple-style client creation functions
    static IOUserClient* createSurfaceClient();                    // Type 0
    static IOUserClient* createContextClient(UInt32 contextType);  // Types 1,3,7
    static IOUserClient* createGLContextClient();                   // Type 2
    static IOUserClient* createDisplayPipeClient();                // Type 4 
    static IOUserClient* createDeviceClient();                      // Type 5 
    static IOUserClient* createSharedClient();                      // Type 6
    static IOUserClient* createCommandQueueClient();                // Type 8 
    static IOUserClient* createGLDrawableClient();                  // Type 9
    static IOUserClient* createSurfaceMTLClient();                 // Type 32
    static IOUserClient* createMemoryInfoClient();                  // Type 33 (0x21)
    
    // Helper functions
    static const char* getClientTypeName(UInt32 clientType);
    static bool isValidClientType(UInt32 clientType);
    static UInt32 getClientPriority(UInt32 clientType);
};

#endif /* IntelIOAcceleratorClients_h */
