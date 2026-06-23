/*
 * AppleIntelTGLIOAccelerator.h - IOAccelerator Service Interface
 * Week 41: IOAccelerator Base
 *
 * IOAccelerator is the bridge between userspace Metal applications and the kernel driver.
 * It provides:
 * - User client creation for Metal framework
 * - Command queue management
 * - Shared memory regions for textures/buffers
 * - Resource synchronization
 *
 * This subclasses IOAcceleratorFamily2, Apple's GPU acceleration framework.
 */

#ifndef IntelIOAccelerator_h
#define IntelIOAccelerator_h

#include <IOKit/graphics/IOAccelerator.h>

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMapper.h>

// Forward declarations
class AppleIntelTGLController;
class IntelGuC;
class IntelGuCSubmission;
class IntelIOAcceleratorClientBase;
class IntelContext;
class IntelContext;


// MARK: - Constants


// IOAccelerator service names
#define kIntelIOAcceleratorClassName    "IntelIOAccelerator"

// Command queue limits
#define MAX_COMMAND_QUEUES              64      // Max queues per device
#define MAX_CLIENTS                     128     // Max concurrent clients
#define COMMAND_QUEUE_SIZE              (64 * 1024)  // 64KB per queue

// Shared memory regions
#define SHARED_MEMORY_SIZE              (16 * 1024 * 1024)  // 16MB per client
#define SHARED_MEMORY_REGIONS           8       // Max regions per client

// Resource limits
#define MAX_TEXTURES                    4096    // Max textures per client
#define MAX_BUFFERS                     2048    // Max buffers per client
#define MAX_SAMPLERS                    256     // Max samplers per client


// MARK: - Types


// Command queue types
enum IOAccelCommandQueueType {
    kIOAccelCommandQueueTypeRender      = 0,    // Graphics rendering
    kIOAccelCommandQueueTypeCompute     = 1,    // Compute/GPGPU
    kIOAccelCommandQueueTypeBlit        = 2,    // Memory transfer/blit
    kIOAccelCommandQueueTypeVideo       = 3,    // Video encode/decode
};

// Command buffer priority
enum IOAccelCommandPriority {
    kIOAccelCommandPriorityLow          = 0,
    kIOAccelCommandPriorityNormal       = 1,
    kIOAccelCommandPriorityHigh         = 2,
    kIOAccelCommandPriorityRealtime     = 3,
};

// Resource types
enum IOAccelResourceType {
    kIOAccelResourceTypeTexture         = 0,
    kIOAccelResourceTypeBuffer          = 1,
    kIOAccelResourceTypeSampler         = 2,
    kIOAccelResourceTypeHeap            = 3,
};

// Synchronization types
enum IOAccelSyncType {
    kIOAccelSyncTypeFence               = 0,    // CPU-GPU fence
    kIOAccelSyncTypeSemaphore           = 1,    // GPU-GPU semaphore
    kIOAccelSyncTypeEvent               = 2,    // Metal event
};


// MARK: - Structures


// Command queue descriptor
struct IOAccelCommandQueueDescriptor {
    uint32_t queueType;         // IOAccelCommandQueueType
    uint32_t priority;          // IOAccelCommandPriority
    uint32_t size;              // Queue size in bytes
    uint32_t flags;             // Queue flags
    uint64_t clientID;          // Owning client ID
} __attribute__((packed));

// Shared memory descriptor
struct IOAccelSharedMemoryDescriptor {
    uint64_t address;           // Virtual address in client
    uint64_t size;              // Size in bytes
    uint32_t flags;             // Memory flags
    uint32_t protection;        // Memory protection (r/w/x)
} __attribute__((packed));

// Resource descriptor
struct IOAccelResourceDescriptor {
    uint32_t resourceType;      // IOAccelResourceType
    uint32_t resourceID;        // Unique resource ID
    uint64_t gpuAddress;        // GPU virtual address
    uint64_t size;              // Size in bytes
    uint32_t width;             // Width (for textures)
    uint32_t height;            // Height (for textures)
    uint32_t format;            // Pixel format / buffer type
    uint32_t flags;             // Resource flags
} __attribute__((packed));


// Statistics
struct IOAccelStatistics {
    uint64_t commandBuffersSubmitted;
    uint64_t commandBuffersCompleted;
    uint64_t commandBuffersFailed;
    uint64_t texturesCreated;
    uint64_t buffersCreated;
    uint64_t memoryAllocated;
    uint64_t memoryMapped;
    uint32_t activeClients;
    uint32_t activeQueues;
    uint32_t activeResources;
} __attribute__((packed));


// MARK: - IntelIOAccelerator Class


class IntelIOAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(IntelIOAccelerator)
    
public:

    // MARK: - IOService Overrides

    
    virtual bool init(OSDictionary* dictionary = NULL) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService* provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService* provider) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    

    // MARK: - Client Management

    
    // Set the controller (called before start())
    void setController(AppleIntelTGLController* ctrl);
    
    virtual IOReturn newUserClient(task_t owningTask, void* securityID,
                                   UInt32 type, IOUserClient** handler);
    
    bool registerClient(IntelIOAcceleratorClientBase* client);
    void unregisterClient(IntelIOAcceleratorClientBase* client);
    uint32_t getClientCount();
    uint32_t getNextClientID();  // Get unique client ID for GuC scheduling
    

    // MARK: - Command Queue Management

    
    IOReturn createCommandQueue(IOAccelCommandQueueDescriptor* desc,
                               uint32_t* outQueueID);
    IOReturn destroyCommandQueue(uint32_t queueID);
    IOReturn submitCommandBuffer(uint32_t queueID, void* buffer, uint32_t size);
    

    // MARK: - Shared Memory Management

    
    IOReturn allocateSharedMemory(uint64_t size, uint32_t flags,
                                 IOMemoryDescriptor** outMemory);
    IOReturn mapSharedMemory(IOMemoryDescriptor* memory, task_t task,
                            uint64_t* outAddress);
    IOReturn unmapSharedMemory(IOMemoryDescriptor* memory, task_t task);
    

    // MARK: - Resource Management

    
    IOReturn createResource(IOAccelResourceDescriptor* desc,
                           uint32_t* outResourceID);
    IOReturn destroyResource(uint32_t resourceID);
    IOReturn mapResource(uint32_t resourceID, task_t task,
                        uint64_t* outAddress);
    

    // MARK: - Synchronization

    
    IOReturn createFence(uint64_t initialValue, uint32_t* outFenceID);
    IOReturn waitForFence(uint32_t fenceID, uint64_t value, uint32_t timeoutMs);
    IOReturn signalFence(uint32_t fenceID, uint64_t value);
    

    // MARK: - Statistics

    
    void getStatistics(IOAccelStatistics* outStats);
    void resetStatistics();
    

    // MARK: - Completion Handling (called by GT interrupts)

    
    void handleCommandCompletion(uint32_t engine, uint32_t seqno);
    

    // MARK: - Hardware Access

    
    AppleIntelTGLController* getController() { return controller; }
    IntelGuC* getGuC() { return guc; }
    IntelGuCSubmission* getSubmission() { return submission; }
    IOWorkLoop* getWorkLoop();
    
    // Metal Context Support
    IntelContext* getMetalContext();
    

    // MARK: - VRAM/Aperture Access (Hardware Acceleration)

    
    // Get VRAM range for Metal/WindowServer direct GPU memory access
    IODeviceMemory* getVRAMRange();
    
    // Get GPU aperture (BAR) for texture/buffer mapping
    IOReturn getGPUAperture(uint64_t* outPhysicalAddress, uint64_t* outSize);
    
    // Map GPU memory into client's address space
    IOReturn mapGPUMemory(uint64_t gpuAddress, uint64_t size, task_t task,
                         uint64_t* outVirtualAddress);
    
private:
    // Hardware references
    AppleIntelTGLController*            controller;
    IntelGuC*                       guc;
    IntelGuCSubmission*             submission;
    
    // Client tracking
    OSArray*                        clients;
    IOLock*                         clientsLock;
    uint32_t                        nextClientID;
    
    // Command queue tracking
    OSArray*                        commandQueues;
    IOLock*                         queuesLock;
    uint32_t                        nextQueueID;
    
    // Resource tracking
    OSArray*                        resources;
    IOLock*                         resourcesLock;
    uint32_t                        nextResourceID;
    
    // Fence tracking - CRITICAL: Store fence buffers to prevent memory leaks
    OSArray*                        fenceBuffers;     // Array of IOBufferMemoryDescriptor*
    IOLock*                         fencesLock;
    uint32_t                        nextFenceID;
    
    // Statistics
    IOAccelStatistics               stats;
    IOLock*                         statsLock;
    
    // Initialization state
    bool                            initialized;
};

#endif /* IntelIOAccelerator_h */
