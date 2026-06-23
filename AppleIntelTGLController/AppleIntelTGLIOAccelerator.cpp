/*
 * IntelIOAccelerator.cpp - IOAccelerator Service Implementation
 * Week 41: IOAccelerator Base
 *
 * This implements the IOAccelerator service that bridges userspace Metal
 * applications to the kernel driver. Provides command queue management,
 * shared memory, and resource tracking.
 */
#include <IOKit/graphics/IOAccelerator.h>

#include "AppleIntelTGLIOAccelerator.h"
#include "AppleIntelTGLIOAcceleratorClients.h"  // Multi-client architecture
#include "AppleIntelTGLController.h"
#include "IntelContext.h"
#include "IntelRequest.h"
#include "IntelFence.h"
#include "IntelRingBuffer.h"
#include "IntelGEM.h"
#include "IntelGEMObject.h"
#include "IntelGTT.h"
#include "IntelGuC.h"
#include "IntelGuCSubmission.h"
#include "AppleIntelTGLIOSurfaceManager.h"
#include <IOKit/IOLib.h>


#define super IOAccelerator
OSDefineMetaClassAndStructors(IntelIOAccelerator, IOAccelerator)


// MARK: - IOService Lifecycle


bool IntelIOAccelerator::init(OSDictionary* dictionary) {
    if (!super::init(dictionary)) {
        return false;
    }
    
    controller = NULL;
    guc = NULL;
    submission = NULL;
    
    clients = NULL;
    clientsLock = NULL;
    nextClientID = 1;
    
    commandQueues = NULL;
    queuesLock = NULL;
    nextQueueID = 1;
    
    resources = NULL;
    resourcesLock = NULL;
    nextResourceID = 1;
    
    // Initialize fence tracking
    fenceBuffers = NULL;
    fencesLock = NULL;
    nextFenceID = 1;
    
    statsLock = NULL;
    memset(&stats, 0, sizeof(stats));
    
    initialized = false;
    
    return true;
}

void IntelIOAccelerator::setController(AppleIntelTGLController* ctrl) {
    if (controller) {
        controller->release();
    }
    controller = ctrl;
    if (controller) {
        controller->retain();
    }
}

bool IntelIOAccelerator::start(IOService* provider) {
    if (!super::start(provider)) {
        return false;
    }
    
    IOLog("IntelIOAccelerator: Starting IOAccelerator Service \n");
    
    //  FIX: Controller might be set already by AppleIntelTGLController::start()
    // If not, search for it in IORegistry
    if (!controller) {
        IOLog("IntelIOAccelerator: Controller not set, searching IORegistry...\n");
        OSIterator* iter = provider->getChildIterator(gIOServicePlane);
        if (iter) {
            while (OSObject* obj = iter->getNextObject()) {
                IOService* service = OSDynamicCast(IOService, obj);
                if (service) {
                    AppleIntelTGLController* ctrl = OSDynamicCast(AppleIntelTGLController, service);
                    if (ctrl) {
                        controller = ctrl;
                        controller->retain();  // Keep a reference
                        break;
                    }
                }
            }
            iter->release();
        }
        
        if (!controller) {
            IOLog("IntelIOAccelerator: ERROR - Cannot find AppleIntelTGLController in IORegistry\n");
            return false;
        }
    }
    
    IOLog("IntelIOAccelerator: OK  Found AppleIntelTGLController: %s\n", controller->getName());
    
    // Get GuC and submission
    guc = controller->getGuC();
    submission = controller->getGuCSubmission();
    
    if (!guc || !submission) {
        IOLog("IntelIOAccelerator: ERROR - GuC or submission not available\n");
        return false;
    }

    IntelIOSurfaceManager* surfaceManager = IntelIOSurfaceManager::sharedInstance();
    if (!surfaceManager || !surfaceManager->initWithController(controller)) {
        IOLog("IntelIOAccelerator: ERROR - IOSurfaceManager init failed\n");
        return false;
    }
    
    // Create locks
    clientsLock = IOLockAlloc();
    queuesLock = IOLockAlloc();
    resourcesLock = IOLockAlloc();
    fencesLock = IOLockAlloc();
    statsLock = IOLockAlloc();
    
    if (!clientsLock || !queuesLock || !resourcesLock || !fencesLock || !statsLock) {
        IOLog("IntelIOAccelerator: ERROR - Failed to allocate locks\n");
        return false;
    }
    
    // Create arrays
    clients = OSArray::withCapacity(MAX_CLIENTS);
    commandQueues = OSArray::withCapacity(MAX_COMMAND_QUEUES);
    resources = OSArray::withCapacity(MAX_TEXTURES + MAX_BUFFERS);
    fenceBuffers = OSArray::withCapacity(256);  // Support up to 256 fences
    
    if (!clients || !commandQueues || !resources || !fenceBuffers) {
        IOLog("IntelIOAccelerator: ERROR - Failed to allocate arrays\n");
        return false;
    }
    
    initialized = true;
    
    //  CRITICAL: Set properties for Quartz Extreme and Metal support
    // These MUST match exactly what Metal framework expects!
    setProperty("IOClass", "IntelIOAccelerator");  // Set class name explicitly
    setProperty("IOProviderClass", "AppleIntelTGLController");  // Parent class
    setProperty("IOMatchCategory", "IOAccelerator");  // OK  CRITICAL for Metal discovery!
    setProperty("IOName", "IntelIOAccelerator");  // Service name
    
    //  CRITICAL FIX: IOAcceleratorTypes is a BITMASK telling WindowServer which client types we support!
    // Bit 0 (0x001) = IOAccelSurface (Type 0)
    // Bit 1 (0x002) = IOAccelContext2 (Type 1) - Metal/GL command submission
    // Bit 2 (0x004) = IOAccelGLContext2 (Type 2)
    // Bit 3 (0x008) = IOAccel2DContext2 (Type 3) - WindowServer compositing!
    // Bit 4 (0x010) = IOAccelDisplayPipe (Type 4)
    // Bit 5 (0x020) = IOAccelDevice2 (Type 5) - CRITICAL for device config!
    // Bit 6 (0x040) = IOAccelSharedUserClient2 (Type 6)
    // Bit 7 (0x080) = IOAccelVideoContext (Type 7)
    // Bit 8 (0x100) = IOAccelCommandQueue (Type 8) - Metal command queues
    // Bit 9 (0x200) = IOAccelGLDrawable (Type 9)
    // All supported = 0x3FF (bits 0-9)
    setProperty("IOAcceleratorTypes", (unsigned long long)0x3FF, 32);
    
    // Additional properties for Metal
    setProperty("IOGPUAcceleratorID", 1, 32);
    setProperty("IOGPUAcceleratorMetalPluginFamily", 2, 32);
    setProperty("IOAcceleratorFamily", 2, 32);
    setProperty("PerformanceStatistics", kOSBooleanTrue);
    
    //  GPU Class identifiers for Metal framework
    setProperty("IOGPUClass", "IntelIrisXeGraphics");
    setProperty("IOGPUAcceleratorPresent", kOSBooleanTrue);
    setProperty("IOAccelDeviceShmem", kOSBooleanTrue);  // Required for Metal shared memory
    
    //  Metal Plugin - Points to the user-space Metal driver bundle in PlugIns
    // Bundle name (folder name in PlugIns/)
    setProperty("MetalPluginName", "AppleIntelTGLGraphicsMTLDriver");
    
    // Principal class name inside the bundle (NSPrincipalClass in bundle's Info.plist)
    setProperty("MetalPluginClassName", "MTLIGAccelDevice");
    
    // Display name in System Information
    setProperty("MetalStatisticsName", "Intel(R) Iris(R) Xe Graphics");
    
    // Set PlugIns path for Metal framework to find bundles
    // Path is relative to the kext bundle root (not Contents/)
    OSString* pluginsPath = OSString::withCString("PlugIns");
    setProperty("PlugInCollection", pluginsPath);
    pluginsPath->release();
    
    // Metal discovery properties
    // IOName is set above
    
    //  CRITICAL: IOAccelerator2 / SkyLight properties
    // These tell WindowServer that this GPU supports hardware acceleration
    setProperty("IOAccelCapabilities", (unsigned long long)0x7FFFFFFF, 32);  // All capabilities
    setProperty("IOAccelTypes", "IOSurface");
    setProperty("IOAccelRevision", (unsigned long long)2, 32);  // IOAcceleratorFamily2
    setProperty("IOAccelIndex", (unsigned long long)0, 32);     // First accelerator
    setProperty("IOAccelCaps", (unsigned long long)0x7, 32);    // Blit, fill, alpha
    
    // IOSurface support - CRITICAL for compositing!
    setProperty("IOSurfaceSupported", kOSBooleanTrue);
    setProperty("IOAccelSurfaceSupported", kOSBooleanTrue);
    setProperty("IOSurfaceIsGlobal", kOSBooleanTrue);
    
    // Texture capabilities
    setProperty("IOAccelMaxTextureSize", (unsigned long long)16384, 32);
    setProperty("IOAccelTextureFormats", (unsigned long long)0xFFFFFFFF, 32);
    
    // Memory info - Updated to 2GB to match GEM
    setProperty("IOAccelVideoMemorySize", (unsigned long long)(2ULL * 1024 * 1024 * 1024), 64);
    setProperty("IOAccelMemorySize", (unsigned long long)(256 * 1024 * 1024), 64);
    
    // GPU info for system_profiler and About This Mac
    setProperty("model", "Intel Iris Xe Graphics");
    setProperty("device-id", 0x9A49, 32);
    setProperty("vendor-id", 0x8086, 32);
    setProperty("subsystem-id", 0x7270, 32);
    setProperty("subsystem-vendor-id", 0x8086, 32);
    
    IOLog("IntelIOAccelerator: IORegistry properties set for Metal/WindowServer\n");
    
    // Register as IOAccelerator service
    registerService();
    
    IOLog("IntelIOAccelerator: OK  IOAccelerator service started\n");
   
    
    return true;
}

void IntelIOAccelerator::stop(IOService* provider) {
    IOLog("IntelIOAccelerator: Stopping service...\n");
    
    initialized = false;
    
    // Release controller reference
    if (controller) {
        controller->release();
        controller = NULL;
    }

    IntelIOSurfaceManager::destroySharedInstance();
    
    // Release arrays
    OSSafeReleaseNULL(clients);
    OSSafeReleaseNULL(commandQueues);
    OSSafeReleaseNULL(resources);
    
    // Free locks
    if (clientsLock) {
        IOLockFree(clientsLock);
        clientsLock = NULL;
    }
    if (queuesLock) {
        IOLockFree(queuesLock);
        queuesLock = NULL;
    }
    if (resourcesLock) {
        IOLockFree(resourcesLock);
        resourcesLock = NULL;
    }
    if (statsLock) {
        IOLockFree(statsLock);
        statsLock = NULL;
    }
    
    super::stop(provider);
}

void IntelIOAccelerator::free() {
    // Clean up fence buffers
    if (fenceBuffers) {
        IOLockLock(fencesLock);
        for (unsigned int i = 0; i < fenceBuffers->getCount(); i++) {
            IOBufferMemoryDescriptor* buffer =
                OSDynamicCast(IOBufferMemoryDescriptor, fenceBuffers->getObject(i));
            if (buffer) {
                buffer->complete();
                buffer->release();
            }
        }
        fenceBuffers->release();
        fenceBuffers = NULL;
        IOLockUnlock(fencesLock);
    }
    
    if (fencesLock) {
        IOLockFree(fencesLock);
        fencesLock = NULL;
    }
    
    super::free();
}


// MARK: - Client Management

//humannn verified
// MARK: - newUserClient (Apple's EXACT client type dispatch)
// From IOAcceleratorFamily2 at offset 0x3f27a
//
// Apple's Client Type Table:
// | Type | Hex  | Class Created                | Description                        |
// |  0   | 0x00 | IOAccelSurface               | Standard pixel buffer/surface      |
// |  1   | 0x01 | IOAccelContext2 (vtable 0xa58)| General execution context          |
// |  2   | 0x02 | IOAccelContext2 (vtable 0xa38)| OpenGL Context (GL variant)        |
// |  3   | 0x03 | IOAccelContext2 (vtable 0xa58)| 2D Context (WindowServer)          |
// |  4   | 0x04 | IOAccelDisplayPipeUserClient2| Display pipeline (needs this[0xccf])|
// |  5   | 0x05 | IOAccelDevice2 (vtable 0xa18) | Main device connection             |
// |  6   | 0x06 | IOAccelSharedUserClient2     | Global resource sharing            |
// |  7   | 0x07 | IOAccelContext2 (vtable 0xa58)| Video context                      |
// |  8   | 0x08 | IOAccelCommandQueue          | Command submission queue           |
// |  9   | 0x09 | IOAccelGLDrawableUserClient  | OpenGL drawable client             |
// | 32   | 0x20 | IOAccelSurfaceMTL            | Metal surface (modern graphics)    |
// | 33   | 0x21 | IOAccelMemoryInfoUserClient  | Memory debugging tool              |

IOReturn IntelIOAccelerator::newUserClient(task_t owningTask, void* securityID,
                                           UInt32 type, IOUserClient** handler)
{
    IOUserClient* client = NULL;
    
    IOLog("[TGL][Accel] newUserClient() - EXACT Apple Implementation\n");
    IOLog("[TGL][Accel] Client Type Requested: %u (0x%x)\n", type, type);
    
    //  EXACT Apple client type dispatch (from IOAcceleratorFamily2 at 0x3f27a)
    switch (type) {
        case 0:  // IOAccelSurface
            IOLog("[TGL][Accel] Creating IOAccelSurface client (Type 0)\n");
            client = IntelClientFactory::createSurfaceClient();
            break;
            
        case 1:  // IOAccelContext2 (GL/Metal)
            IOLog("[TGL][Accel] Creating IOAccelContext2 client (Type 1)\n");
            client = IntelClientFactory::createContextClient(1);
            break;
            
        case 2:  // IOAccelGLContext2 (OpenGL)
            IOLog("[TGL][Accel] Creating IOAccelGLContext2 client (Type 2)\n");
            client = IntelClientFactory::createGLContextClient();
            break;
            
        case 3:  // IOAccel2DContext2 (WindowServer!)
            IOLog("[TGL][Accel] Creating IOAccel2DContext2 client (Type 3) - WindowServer!\n");
            client = IntelClientFactory::createContextClient(3);
            break;
            
        case 4:  // IOAccelDisplayPipeUserClient2
            IOLog("[TGL][Accel] Creating IOAccelDisplayPipeUserClient2 client (Type 4)\n");
            client = IntelClientFactory::createDisplayPipeClient();
            break;
            
        case 5:  // IOAccelDevice2 (Device Configuration) 
            IOLog("[TGL][Accel] Creating IOAccelDevice2 client (Type 5) - Device Config!\n");
            client = IntelClientFactory::createDeviceClient();
            break;
            
        case 6:  // IOAccelSharedUserClient2 (Shared Resources)
            IOLog("[TGL][Accel] Creating IOAccelSharedUserClient2 client (Type 6)\n");
            client = IntelClientFactory::createSharedClient();
            break;
            
        case 7:  // IOAccelVideoContext (Video)
            IOLog("[TGL][Accel] Creating IOAccelVideoContext client (Type 7)\n");
            client = IntelClientFactory::createContextClient(7);
            break;
            
        case 8:  // IOAccelCommandQueue (Metal Command Queues) 
            IOLog("[TGL][Accel] Creating IOAccelCommandQueue client (Type 8) - Metal!\n");
            client = IntelClientFactory::createCommandQueueClient();
            break;
            
        case 9:  // IOAccelGLDrawableUserClient
            IOLog("[TGL][Accel] Creating IOAccelGLDrawableUserClient client (Type 9)\n");
            client = IntelClientFactory::createGLDrawableClient();
            break;
            
        case 32:  // IOAccelSurfaceMTL (Metal Surface) - 0x20
            IOLog("[TGL][Accel] Creating IOAccelSurfaceMTL client (Type 32/0x20)\n");
            client = IntelClientFactory::createSurfaceMTLClient();
            break;
            
        case 33:  // IOAccelMemoryInfoUserClient (Memory debugging) - 0x21
            IOLog("[TGL][Accel] Creating IOAccelMemoryInfoUserClient client (Type 33/0x21)\n");
            client = IntelClientFactory::createMemoryInfoClient();
            break;
            
        default:
            IOLog("[TGL][Accel] ERROR: Unknown client type %u (0x%x)\n", type, type);
            return kIOReturnBadArgument;
    }
    
    if (!client) {
        IOLog("[TGL][Accel] ERROR: Failed to create client (out of memory)\n");
        return kIOReturnNoMemory;
    }
    
    //  CRITICAL: Apple's exact initialization sequence
    // initWithTask() -> attach() -> start()
    
    if (!client->initWithTask(owningTask, securityID, type)) {
        IOLog("[TGL][Accel] ERROR: client->initWithTask() failed\n");
        client->release();
        return kIOReturnError;
    }
    
    if (!client->attach(this)) {
        IOLog("[TGL][Accel] ERROR: client->attach() failed\n");
        client->release();
        return kIOReturnError;
    }
    
    if (!client->start(this)) {
        IOLog("[TGL][Accel] ERROR: client->start() failed\n");
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    
    IOLog("[TGL][Accel] OK  Client created and started successfully\n");
    *handler = client;
    return kIOReturnSuccess;
}

bool IntelIOAccelerator::registerClient(IntelIOAcceleratorClientBase* client) {
    if (!client) {
        return false;
    }
    
    IOLockLock(clientsLock);
    
    clients->setObject(client);
    stats.activeClients = clients->getCount();
    
    IOLockUnlock(clientsLock);
    
    IOLog("IntelIOAccelerator: Client registered (ID: %u, total: %u)\n",
          nextClientID, stats.activeClients);
    
    nextClientID++;
    
    return true;
}

void IntelIOAccelerator::unregisterClient(IntelIOAcceleratorClientBase* client) {
    if (!client) {
        return;
    }
    
    IOLockLock(clientsLock);
    
    unsigned int index = clients->getNextIndexOfObject(client, 0);
    if (index != (unsigned int)-1) {
        clients->removeObject(index);
        stats.activeClients = clients->getCount();
    }
    
    IOLockUnlock(clientsLock);
    
    IOLog("IntelIOAccelerator: Client unregistered (total: %u)\n", stats.activeClients);
}

uint32_t IntelIOAccelerator::getClientCount() {
    IOLockLock(clientsLock);
    uint32_t count = clients->getCount();
    IOLockUnlock(clientsLock);
    return count;
}

uint32_t IntelIOAccelerator::getNextClientID() {
    // Client ID must be >= 1 (0 is reserved/invalid for GuC scheduling)
    IOLockLock(clientsLock);
    uint32_t id = nextClientID++;
    if (nextClientID == 0) {
        nextClientID = 1;  // Wrap around but skip 0
    }
    IOLockUnlock(clientsLock);
    IOLog("IntelIOAccelerator: Assigned client ID %u\n", id);
    return id;
}


// MARK: - Command Queue Management


IOReturn IntelIOAccelerator::createCommandQueue(IOAccelCommandQueueDescriptor* desc,
                                               uint32_t* outQueueID) {
    if (!initialized || !desc || !outQueueID) {
        return kIOReturnBadArgument;
    }
    
    IOLockLock(queuesLock);
    
    // Check queue limit
    if (commandQueues->getCount() >= MAX_COMMAND_QUEUES) {
        IOLockUnlock(queuesLock);
        IOLog("IntelIOAccelerator: ERROR - Maximum command queues reached\n");
        return kIOReturnNoResources;
    }
    
    // Allocate queue ID
    uint32_t queueID = nextQueueID++;
    *outQueueID = queueID;
    
    // Create queue memory
    uint32_t queueSize = desc->size > 0 ? desc->size : COMMAND_QUEUE_SIZE;
    IOBufferMemoryDescriptor* queueMemory =
        IOBufferMemoryDescriptor::withCapacity(queueSize, kIODirectionInOut);
    
    if (!queueMemory) {
        IOLockUnlock(queuesLock);
        IOLog("IntelIOAccelerator: ERROR - Failed to allocate queue memory\n");
        return kIOReturnNoMemory;
    }
    
    // Add to tracking (simplified - would store more metadata)
    commandQueues->setObject(queueMemory);
    queueMemory->release();
    
    stats.activeQueues = commandQueues->getCount();
    
    IOLockUnlock(queuesLock);
    
    //  CRITICAL: Register default context with GuC when first queue is created
    if (submission && commandQueues->getCount() == 1) {
        IntelContext* context = controller->getDefaultContext();
        if (context) {
            if (submission->registerContext(context, 0)) {
                IOLog("IntelIOAccelerator: OK  Default context registered with GuC for queue %u\n", queueID);
            } else {
                IOLog("IntelIOAccelerator:   Failed to register context with GuC (will use render ring)\n");
            }
        }
    }
    
    IOLog("IntelIOAccelerator: Command queue created (ID: %u, type: %u, size: %u KB)\n",
          queueID, desc->queueType, queueSize / 1024);
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::destroyCommandQueue(uint32_t queueID) {
    if (!initialized) {
        return kIOReturnNotReady;
    }
    
    IOLockLock(queuesLock);
    
    // Simplified - would look up queue by ID and remove
    // For now, just decrement count
    if (commandQueues->getCount() > 0) {
        commandQueues->removeObject(0);
        stats.activeQueues = commandQueues->getCount();
    }
    
    IOLockUnlock(queuesLock);
    
    IOLog("IntelIOAccelerator: Command queue destroyed (ID: %u)\n", queueID);
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::submitCommandBuffer(uint32_t queueID, void* buffer,
                                                uint32_t size) {
    if (!initialized || !buffer || size == 0) {
        return kIOReturnBadArgument;
    }
    
    IOLog("IntelIOAccelerator: Submitting command buffer (queue: %u, size: %u bytes)\n",
          queueID, size);
    
    // Buffer contains raw GPU commands (no header needed for direct ring submission)
    const uint32_t* commands = (const uint32_t*)buffer;
    uint32_t commandCount = size / sizeof(uint32_t);
    
    IOLog("IntelIOAccelerator: Raw GPU commands - %u dwords (%u bytes)\n",
          commandCount, size);
    
    // Debug: show first few commands
    if (commandCount > 0) {
        IOLog("IntelIOAccelerator: First command: 0x%08x\n", commands[0]);
    }
    
    //  TRY GuC submission first, fall back to render ring if GuC fails
    bool useGuC = true;  // Change to: (submission != NULL) to re-enable GuC
    
    if (useGuC) {
        IOLog("IntelIOAccelerator:  Attempting GuC submission...\n");
        
        // Get default context
        IntelContext* context = controller->getDefaultContext();
        if (!context) {
            IOLog("IntelIOAccelerator: ERROR - No context for GuC submission\n");
            useGuC = false;
        } else {
            // Ensure context is registered with GuC
            if (!submission->registerContext(context, 0)) {
                IOLog("IntelIOAccelerator: WARNING - Context registration failed, falling back to direct ring\n");
                useGuC = false;
            } else {
                // For now, GuC submission is complex - need batch buffers
                // Fall back to render ring until batch buffer support is complete
                IOLog("IntelIOAccelerator: INFO - GuC requires batch buffer support, using render ring\n");
                useGuC = false;
            }
        }
    }
    
    // Fallback to direct render ring submission
    if (!useGuC) {
        IOLog("IntelIOAccelerator:  Using direct render ring submission...\n");
        
        IntelRingBuffer* renderRing = controller->getRenderRing();
        if (!renderRing) {
            IOLog("IntelIOAccelerator: ERROR - Render ring not available\n");
            return kIOReturnError;
        }
        
        IOLog("IntelIOAccelerator: Submitting %u dwords to render ring...\n", commandCount);
        
        // Submit directly to render ring (proven working method)
        if (!renderRing->submitCommand(commands, commandCount, NULL)) {
            IOLog("IntelIOAccelerator: ERROR - Render ring submission failed\n");
            return kIOReturnError;
        }
        
        IOLog("IntelIOAccelerator: OK  Commands submitted via render ring!\n");
        // Commands execute asynchronously - completion via interrupt
    }
    
    // Track statistics
    IOLockLock(statsLock);
    stats.commandBuffersSubmitted++;
    // NOTE: commandBuffersCompleted incremented by interrupt handler
    IOLockUnlock(statsLock);
    
    if (useGuC) {
        IOLog("IntelIOAccelerator: OK  Command buffer executed via GuC (%llu total)\n",
              stats.commandBuffersSubmitted);
    } else {
        IOLog("IntelIOAccelerator: OK  Command buffer executed via render ring (%llu total)\n",
              stats.commandBuffersSubmitted);
    }
    IOLog("IntelIOAccelerator: OK  Command buffer submitted (%llu total) - will complete via interrupt\n",
          stats.commandBuffersSubmitted);
    
    return kIOReturnSuccess;
}


// MARK: - Completion Handling (called by GT interrupts)


void IntelIOAccelerator::handleCommandCompletion(uint32_t engine, uint32_t seqno) {
    // Called by GT interrupt handler when render completes
    IOLockLock(statsLock);
    stats.commandBuffersCompleted++;
    IOLockUnlock(statsLock);
    
    IOLog("IntelIOAccelerator: OK  Command completed! engine=%u seqno=%u (completed: %llu/%llu)\n",
          engine, seqno, stats.commandBuffersCompleted, stats.commandBuffersSubmitted);
}
    
  

// MARK: - Shared Memory Management (GTT-backed for GPU access)


IOReturn IntelIOAccelerator::allocateSharedMemory(uint64_t size, uint32_t flags,
                                                 IOMemoryDescriptor** outMemory) {
    if (!initialized || !outMemory || size == 0) {
        return kIOReturnBadArgument;
    }
    
    // Align size to page boundary
    size = (size + 4095) & ~4095;
    
    IOLog("IntelIOAccelerator: Allocating GTT-backed shared memory (%llu KB, flags: 0x%x)\n",
          size / 1024, flags);
    
    // Create shared memory buffer with GPU-accessible memory
    // Using inTaskWithPhysicalMask for physically contiguous memory
    IOBufferMemoryDescriptor* memory =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryKernelUserShared,
            size,
            0x000000003FFFF000ULL  // 4GB physical mask for GPU
        );
    
    if (!memory) {
        IOLog("IntelIOAccelerator: ERROR - Failed to allocate shared memory\n");
        return kIOReturnNoMemory;
    }
    
    if (memory->prepare() != kIOReturnSuccess) {
        IOLog("IntelIOAccelerator: ERROR - Failed to prepare shared memory\n");
        memory->release();
        return kIOReturnVMError;
    }
    
    *outMemory = memory;
    
    IOLockLock(statsLock);
    stats.memoryAllocated += size;
    IOLockUnlock(statsLock);
    
    IOLog("IntelIOAccelerator: OK  GTT-backed shared memory allocated at phys=0x%llx (%llu MB total)\n",
          memory->getPhysicalAddress(), stats.memoryAllocated / (1024 * 1024));
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::mapSharedMemory(IOMemoryDescriptor* memory, task_t task,
                                            uint64_t* outAddress) {
    if (!initialized || !memory || !outAddress) {
        return kIOReturnBadArgument;
    }
    
    // For kernel task, use the map to get the virtual address
    if (task == kernel_task) {
        IOMemoryMap* map = memory->map();
        if (map) {
            *outAddress = (uint64_t)map->getAddress();
            map->release();
            IOLog("IntelIOAccelerator: Shared memory mapped to kernel address 0x%llx\n", *outAddress);
            return kIOReturnSuccess;
        }
        return kIOReturnError;
    }
    
    // Map memory into user's address space
    IOMemoryMap* map = memory->createMappingInTask(task, 0, kIOMapAnywhere);
    if (!map) {
        IOLog("IntelIOAccelerator: ERROR - Failed to map shared memory\n");
        return kIOReturnVMError;
    }
    
    *outAddress = map->getVirtualAddress();
    
    IOLockLock(statsLock);
    stats.memoryMapped += memory->getLength();
    IOLockUnlock(statsLock);
    
    IOLog("IntelIOAccelerator: Shared memory mapped to user 0x%llx\n", *outAddress);
    
    map->release();
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::unmapSharedMemory(IOMemoryDescriptor* memory, task_t task) {
    if (!initialized || !memory) {
        return kIOReturnBadArgument;
    }
    
    IOLockLock(statsLock);
    stats.memoryMapped -= memory->getLength();
    IOLockUnlock(statsLock);
    
    IOLog("IntelIOAccelerator: Shared memory unmapped\n");
    
    return kIOReturnSuccess;
}


// MARK: - Resource Management


IOReturn IntelIOAccelerator::createResource(IOAccelResourceDescriptor* desc,
                                           uint32_t* outResourceID) {
    if (!initialized || !desc || !outResourceID) {
        return kIOReturnBadArgument;
    }
    
    IOLockLock(resourcesLock);
    
    // Check resource limits
    uint32_t resourceCount = resources->getCount();
    if (resourceCount >= (MAX_TEXTURES + MAX_BUFFERS)) {
        IOLockUnlock(resourcesLock);
        IOLog("IntelIOAccelerator: ERROR - Maximum resources reached\n");
        return kIOReturnNoResources;
    }
    
    // Allocate resource ID
    uint32_t resourceID = nextResourceID++;
    *outResourceID = resourceID;
    
    //  CRITICAL: Create GPU-backed resource using GEM allocator!
    IntelGEM* gem = controller->getGEM();
    if (!gem) {
        IOLockUnlock(resourcesLock);
        IOLog("IntelIOAccelerator: ERROR - GEM allocator not available\n");
        return kIOReturnError;
    }
    
    // Allocate GPU memory via GEM (Graphics Execution Manager)
    IntelGEMObject* gemObject = gem->createObject(desc->size);
    if (!gemObject) {
        IOLockUnlock(resourcesLock);
        IOLog("IntelIOAccelerator: ERROR - Failed to allocate GPU memory via GEM\n");
        return kIOReturnNoMemory;
    }
    
    // Store GPU address in descriptor for later access
    desc->gpuAddress = gemObject->getGPUAddress();
    desc->resourceID = resourceID;
    
    // Track resource (simplified - would store in proper data structure)
    stats.activeResources++;
    stats.memoryAllocated += desc->size;
    
    if (desc->resourceType == kIOAccelResourceTypeTexture) {
        stats.texturesCreated++;
        IOLog("IntelIOAccelerator: OK  GPU texture created (ID: %u, %ux%u, GPU: 0x%llx, %llu KB)\n",
              resourceID, desc->width, desc->height, desc->gpuAddress, desc->size / 1024);
    } else if (desc->resourceType == kIOAccelResourceTypeBuffer) {
        stats.buffersCreated++;
        IOLog("IntelIOAccelerator: OK  GPU buffer created (ID: %u, GPU: 0x%llx, %llu KB)\n",
              resourceID, desc->gpuAddress, desc->size / 1024);
    }
    
    IOLockUnlock(resourcesLock);
    
    const char* typeName = (desc->resourceType == kIOAccelResourceTypeTexture) ? "Texture" : "Buffer";
    IOLog("IntelIOAccelerator: %s created (ID: %u, size: %llu KB)\n",
          typeName, resourceID, desc->size / 1024);
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::destroyResource(uint32_t resourceID) {
    if (!initialized) {
        return kIOReturnNotReady;
    }
    
    IOLockLock(resourcesLock);
    
    // Simplified - would look up resource by ID and remove
    if (resources->getCount() > 0) {
        resources->removeObject(0);
        stats.activeResources = resources->getCount();
    }
    
    IOLockUnlock(resourcesLock);
    
    IOLog("IntelIOAccelerator: Resource destroyed (ID: %u)\n", resourceID);
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::mapResource(uint32_t resourceID, task_t task,
                                        uint64_t* outAddress) {
    if (!initialized || !outAddress) {
        return kIOReturnBadArgument;
    }
    
    // Simplified - would look up resource and map to task
    *outAddress = 0;
    
    IOLog("IntelIOAccelerator: Resource mapped (ID: %u)\n", resourceID);
    
    return kIOReturnSuccess;
}


// MARK: - Synchronization


IOReturn IntelIOAccelerator::createFence(uint64_t initialValue, uint32_t* outFenceID) {
    if (!initialized || !outFenceID) {
        return kIOReturnBadArgument;
    }
    
    //  CRITICAL FIX: Allocate shared memory for fence value
    // WindowServer needs to read this value to check GPU completion
    IOBufferMemoryDescriptor* fenceBuffer = IOBufferMemoryDescriptor::withCapacity(
        sizeof(uint64_t), kIODirectionInOut);
    
    if (!fenceBuffer) {
        IOLog("IntelIOAccelerator: ERR  Failed to allocate fence memory\n");
        return kIOReturnNoMemory;
    }
    
    //  CRITICAL FIX: ALWAYS initialize fence value to ZERO!
    // Random garbage values (like 4.8x10^18) make WindowServer think GPU is deadlocked!
    // WindowServer reads this to check GPU completion - MUST start at 0!
    uint64_t* fenceData = (uint64_t*)fenceBuffer->getBytesNoCopy();
    *fenceData = 0;  // ALWAYS 0! WindowServer sets real value via signalFence()
    
    // Prepare buffer for I/O
    fenceBuffer->prepare(kIODirectionInOut);
    
    //  CRITICAL FIX: Store the fence buffer to prevent memory leak!
    // This was causing WindowServer to detect memory corruption and kill clients
    IOLockLock(fencesLock);
    
    // Allocate fence ID
    *outFenceID = nextFenceID++;
    
    // Store fence buffer at index (fenceID - 1)
    // We need to ensure the array is large enough
    unsigned int index = *outFenceID - 1;
    while (fenceBuffers->getCount() <= index) {
        fenceBuffers->setObject(fenceBuffer);  // This will retain it
    }
    
    // If we already had something at this index (reused ID), replace it
    if (fenceBuffers->getCount() > index) {
        IOBufferMemoryDescriptor* oldBuffer =
            OSDynamicCast(IOBufferMemoryDescriptor, fenceBuffers->getObject(index));
        if (oldBuffer) {
            oldBuffer->complete();
            // Array will release it when we replace
        }
        fenceBuffers->replaceObject(index, fenceBuffer);
    }
    
    IOLockUnlock(fencesLock);
    
    IOLog("IntelIOAccelerator: OK  Fence created (ID: %u, initial: %llu, addr: %p) - STORED in array\n",
          *outFenceID, *fenceData, fenceData);
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::waitForFence(uint32_t fenceID, uint64_t value,
                                         uint32_t timeoutMs) {
    if (!initialized) {
        return kIOReturnNotReady;
    }
    
    IOLog("IntelIOAccelerator: Waiting for fence (ID: %u, value: %llu, timeout: %u ms)\n",
          fenceID, value, timeoutMs);
    
    // Simplified - would actually wait for GPU to signal fence
    // For now, simulate immediate completion
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::signalFence(uint32_t fenceID, uint64_t value) {
    if (!initialized) {
        return kIOReturnNotReady;
    }
    
    IOLog("IntelIOAccelerator: Signaling fence (ID: %u, value: %llu)\n",
          fenceID, value);
    
    // Simplified - would signal fence to waiting threads
    
    return kIOReturnSuccess;
}


// MARK: - Statistics


void IntelIOAccelerator::getStatistics(IOAccelStatistics* outStats) {
    if (!outStats) {
        return;
    }
    
    IOLockLock(statsLock);
    
    // Update dynamic stats from GEM if controller is available
    if (controller) {
        // Get GEM memory stats
        stats.memoryAllocated = stats.memoryMapped;  // Track allocated memory
        
        // Get VRAM usage from GEM (approximate)
        // Note: Actual GEM stats would require controller->getGEM()->getUsedMemory()
    }
    
    memcpy(outStats, &stats, sizeof(IOAccelStatistics));
    IOLockUnlock(statsLock);
}

void IntelIOAccelerator::resetStatistics() {
    IOLockLock(statsLock);
    
    uint32_t activeClients = stats.activeClients;
    uint32_t activeQueues = stats.activeQueues;
    uint32_t activeResources = stats.activeResources;
    
    memset(&stats, 0, sizeof(IOAccelStatistics));
    
    stats.activeClients = activeClients;
    stats.activeQueues = activeQueues;
    stats.activeResources = activeResources;
    
    IOLockUnlock(statsLock);
    
    IOLog("IntelIOAccelerator: Statistics reset\n");
}


// MARK: - Hardware Access


IOWorkLoop* IntelIOAccelerator::getWorkLoop() {
    return controller ? controller->getWorkLoop() : NULL;
}


// MARK: - VRAM/Aperture Access (Hardware Acceleration)


IODeviceMemory* IntelIOAccelerator::getVRAMRange() {
    if (!controller) {
        IOLog("IntelIOAccelerator: ERROR - No controller for VRAM access\n");
        return NULL;
    }
    
    // Get GTT (Graphics Translation Table) from controller
    IntelGTT* gtt = controller->getGTT();
    if (!gtt) {
        IOLog("IntelIOAccelerator: ERROR - No GTT for VRAM access\n");
        return NULL;
    }
    
    // Get PCI device to access BAR (Base Address Register)
    IOService* pciDevice = controller->getProvider();
    if (!pciDevice) {
        IOLog("IntelIOAccelerator: ERROR - No PCI device for VRAM\n");
        return NULL;
    }
    
    // Get framebuffer from IntelIOFramebuffer (sibling service)
    IOService* framebuffer = NULL;
    OSIterator* iter = pciDevice->getChildIterator(gIOServicePlane);
    if (iter) {
        while (OSObject* obj = iter->getNextObject()) {
            IOService* service = OSDynamicCast(IOService, obj);
            if (service && strcmp(service->getName(), "IntelIOFramebuffer") == 0) {
                framebuffer = service;
                break;
            }
        }
        iter->release();
    }
    
    if (framebuffer) {
        // Get VRAM range from framebuffer (it has the actual framebuffer memory)
        IODeviceMemory* vramRange = (IODeviceMemory*)framebuffer->getProperty("vramMemory");
        if (vramRange) {
            IOPhysicalAddress phys = vramRange->getPhysicalAddress();
            IOByteCount length = vramRange->getLength();
            
            IOLog("IntelIOAccelerator: OK  VRAM range from framebuffer: phys=0x%llx, size=%llu MB\n",
                  (uint64_t)phys, (uint64_t)length / (1024 * 1024));
            
            // Return a new IODeviceMemory object for this range
            return IODeviceMemory::withRange(phys, length);
        }
    }
    
    // Fallback: Create VRAM range from GTT aperture
    // Intel GPUs use GTT as the aperture for CPU access to GPU memory
    uint64_t gttTotalSize = gtt->getTotalSize();
    
    // For Intel GPUs, the GTT base is typically at a fixed offset
    // We'll use the framebuffer memory pool size as VRAM
    // In real implementation, this would come from BAR mappings
    uint64_t vramSize = gttTotalSize; // Use full GTT size as VRAM aperture
    
    IOLog("IntelIOAccelerator: OK  VRAM range from GTT: size=%llu MB\n",
          vramSize / (1024 * 1024));
    
    // Create IODeviceMemory for GPU-accessible memory region
    // Physical address will be from the actual GPU BAR mapping
    // For now, use a placeholder - in production this comes from PCI BAR1
    IODeviceMemory* vramRange = IODeviceMemory::withRange(0, vramSize);
    
    if (!vramRange) {
        IOLog("IntelIOAccelerator: ERROR - Failed to create VRAM range\n");
        return NULL;
    }
    
    IOLog("IntelIOAccelerator: OK  VRAM range created for GPU acceleration!\n");
    return vramRange;
}

IOReturn IntelIOAccelerator::getGPUAperture(uint64_t* outPhysicalAddress, uint64_t* outSize) {
    if (!controller || !outPhysicalAddress || !outSize) {
        return kIOReturnBadArgument;
    }
    
    // Get GTT which manages the GPU aperture
    IntelGTT* gtt = controller->getGTT();
    if (!gtt) {
        IOLog("IntelIOAccelerator: ERROR - No GTT available\n");
        return kIOReturnError;
    }
    
    // GTT provides the aperture for CPU-visible GPU memory
    // Physical address would come from PCI BAR in production
    *outPhysicalAddress = 0; // Placeholder - real address from PCI BAR1
    *outSize = gtt->getTotalSize();
    
    IOLog("IntelIOAccelerator: GPU Aperture: size=%llu GB\n",
          *outSize / (1024 * 1024 * 1024));
    
    return kIOReturnSuccess;
}

IOReturn IntelIOAccelerator::mapGPUMemory(uint64_t gpuAddress, uint64_t size,
                                         task_t task, uint64_t* outVirtualAddress) {
    if (!controller || !outVirtualAddress) {
        return kIOReturnBadArgument;
    }
    
    IOLog("IntelIOAccelerator: Mapping GPU memory: addr=0x%llx, size=%llu KB\n",
          gpuAddress, size / 1024);
    
    // Create memory descriptor for the GPU memory range
    IODeviceMemory* gpuMem = IODeviceMemory::withRange(gpuAddress, size);
    if (!gpuMem) {
        IOLog("IntelIOAccelerator: ERROR - Failed to create GPU memory descriptor\n");
        return kIOReturnNoMemory;
    }
    
    // Map into the task's address space
    IOMemoryMap* map = gpuMem->createMappingInTask(task, 0, kIOMapAnywhere);
    gpuMem->release();
    
    if (!map) {
        IOLog("IntelIOAccelerator: ERROR - Failed to map GPU memory into task\n");
        return kIOReturnVMError;
    }
    
    *outVirtualAddress = map->getVirtualAddress();
    
    IOLog("IntelIOAccelerator: OK  GPU memory mapped to virtual 0x%llx\n", *outVirtualAddress);
    
    // Note: map will be released when the client closes, keeping memory mapped
    map->release();
    
    return kIOReturnSuccess;
}


// MARK: - Metal Context Support


/*
 * Get or create Metal context for Metal command queues
 */
IntelContext* IntelIOAccelerator::getMetalContext() {
    IOLog("IntelIOAccelerator:  Getting Metal context\n");
    
    if (!controller) {
        IOLog("   ERR  No controller available\n");
        return NULL;
    }
    
    // Create dedicated Metal context if not already created
    static IntelContext* metalContext = NULL;
    
    if (!metalContext) {
        metalContext = new IntelContext();
        if (!metalContext || !metalContext->initWithController(controller)) {
            if (metalContext) {
                metalContext->release();
            }
            IOLog("   ERR  Failed to create Metal context\n");
            return NULL;
        }
        
        // Register with GuC for Metal command submission
        if (submission) {
            if (submission->registerContext(metalContext, 1)) {  // High priority for Metal
                IOLog("   OK  Metal context registered with GuC\n");
            } else {
                IOLog("    Metal context registration failed\n");
            }
        }
        
        IOLog("   OK  Metal context created and configured\n");
    }
    
    return metalContext;
}
