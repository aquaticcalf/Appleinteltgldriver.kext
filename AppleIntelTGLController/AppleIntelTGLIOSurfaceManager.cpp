#include "AppleIntelTGLIOSurfaceManager.h"
#include "IntelGEM.h"
#include "IntelGTT.h"
#include <IOKit/IOLib.h>
#include <mach/mach_time.h>

OSDefineMetaClassAndStructors(IntelIOSurfaceManager, OSObject)

IntelIOSurfaceManager* IntelIOSurfaceManager::gSharedInstance = NULL;

IntelIOSurfaceManager* IntelIOSurfaceManager::sharedInstance()
{
    if (!gSharedInstance) {
        IntelIOSurfaceManager* manager = new IntelIOSurfaceManager;
        if (manager && manager->init()) {
            gSharedInstance = manager;
        } else if (manager) {
            manager->release();
        }
    }
    return gSharedInstance;
}

void IntelIOSurfaceManager::destroySharedInstance()
{
    if (gSharedInstance) {
        gSharedInstance->release();
        gSharedInstance = NULL;
    }
}

bool IntelIOSurfaceManager::init()
{
    if (!OSObject::init()) {
        return false;
    }

    bzero(m_hashTable, sizeof(m_hashTable));
    bzero(&m_stats, sizeof(m_stats));

    m_surfaceLock = IOLockAlloc();
    m_statsLock = IOLockAlloc();
    m_nextIOSurfaceID = 1;
    m_activeSurfaceCount = 0;
    m_totalMemoryUsage = 0;
    m_peakMemoryUsage = 0;
    m_memoryPressure = 0;
    m_maintenanceTimer = NULL;
    m_windowServerCallback = NULL;
    m_notificationPort = MACH_PORT_NULL;
    m_framebufferSurfaceID = 0;
    m_framebufferSet = false;
    m_initialized = true;
    m_compressionEnabled = false;
    m_maxSurfaceSize = IOSURFACE_MAX_SIZE;
    m_controller = NULL;
    m_gem = NULL;
    m_gtt = NULL;

    return m_surfaceLock && m_statsLock;
}

bool IntelIOSurfaceManager::initWithController(AppleIntelTGLController* controller)
{
    if (!m_initialized && !init()) {
        return false;
    }

    m_controller = controller;
    m_gem = controller ? controller->getGEM() : NULL;
    m_gtt = controller ? controller->getGTT() : NULL;
    return m_controller != NULL;
}

void IntelIOSurfaceManager::free()
{
    IOLockLock(m_surfaceLock);

    for (uint32_t i = 0; i < IOSURFACE_HASH_BUCKETS; i++) {
        IntelIOSurfaceEntry* entry = m_hashTable[i];
        while (entry) {
            IntelIOSurfaceEntry* next = entry->next;
            if (entry->gemObject && m_gem) {
                m_gem->destroyObject(entry->gemObject);
            }
            if (entry->backing) {
                entry->backing->release();
            }
            IOFree(entry, sizeof(IntelIOSurfaceEntry));
            entry = next;
        }
        m_hashTable[i] = NULL;
    }

    IOLockUnlock(m_surfaceLock);

    if (m_surfaceLock) {
        IOLockFree(m_surfaceLock);
        m_surfaceLock = NULL;
    }
    if (m_statsLock) {
        IOLockFree(m_statsLock);
        m_statsLock = NULL;
    }

    OSObject::free();
}

IOReturn IntelIOSurfaceManager::createSurface(const IntelIOSurfaceProperties* props, uint32_t* outIOSurfaceID)
{
    if (!props || !outIOSurfaceID) {
        return kIOReturnBadArgument;
    }

    if (!m_controller || !m_gem) {
        return kIOReturnNotReady;
    }

    IntelIOSurfaceProperties localProps = *props;
    if (!validateProperties(&localProps)) {
        return kIOReturnBadArgument;
    }

    if (localProps.size == 0) {
        uint64_t size = 0;
        uint32_t alignment = 0;
        if (calculateMemoryRequirements(&localProps, &size, &alignment) != kIOReturnSuccess) {
            return kIOReturnBadArgument;
        }
        localProps.size = size;
    }

    IntelGEMObject* gemObject = m_gem->createObject(localProps.size, I915_BO_ALLOC_USER);
    if (!gemObject) {
        return kIOReturnNoMemory;
    }

    u64 gpuAddress = 0;
    if (!gemObject->mapGTT(&gpuAddress)) {
        m_gem->destroyObject(gemObject);
        return kIOReturnError;
    }

    localProps.gpuAddress = gpuAddress;
    localProps.creationTime = mach_absolute_time();
    localProps.lastAccessTime = localProps.creationTime;

    IntelIOSurfaceEntry* entry = (IntelIOSurfaceEntry*)IOMalloc(sizeof(IntelIOSurfaceEntry));
    if (!entry) {
        m_gem->destroyObject(gemObject);
        return kIOReturnNoMemory;
    }

    bzero(entry, sizeof(IntelIOSurfaceEntry));
    entry->gemObject = gemObject;
    entry->props = localProps;
    entry->backing = NULL;
    entry->port = MACH_PORT_NULL;
    entry->refCount = 1;
    entry->inUse = false;
    entry->lastAccess = localProps.lastAccessTime;
    strlcpy(entry->owner, "unknown", sizeof(entry->owner));

    IOLockLock(m_surfaceLock);

    entry->iosurfaceID = m_nextIOSurfaceID++;
    if (m_nextIOSurfaceID == 0) {
        m_nextIOSurfaceID = 1;
    }
    entry->props.iosurfaceID = entry->iosurfaceID;

    if (createMachPort(entry->iosurfaceID, &entry->port) == kIOReturnSuccess) {
        entry->props.iosurfacePort = entry->port;
    }

    insertEntry(entry);
    m_activeSurfaceCount++;

    IOLockUnlock(m_surfaceLock);

    updateMemoryStats(localProps.size, true);
    updateFormatStats(localProps.pixelFormat);

    *outIOSurfaceID = entry->iosurfaceID;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::createSurface(uint32_t width, uint32_t height, uint32_t pixelFormat,
                                             uint32_t flags, uint32_t* outIOSurfaceID)
{
    IntelIOSurfaceProperties props;
    bzero(&props, sizeof(props));
    props.width = width;
    props.height = height;
    props.pixelFormat = pixelFormat;
    props.displayable = (flags & kIOSurfaceDisplayable) != 0;
    props.globalSurface = (flags & kIOSurfaceGlobal) != 0;
    props.purgeable = (flags & kIOSurfacePurgeable) != 0;
    props.cacheable = true;
    props.bytesPerPixel = 0;
    props.bytesPerRow = 0;
    props.planeCount = 1;

    return createSurface(&props, outIOSurfaceID);
}

IOReturn IntelIOSurfaceManager::createSurfaceFromGEMObject(IntelGEMObject* gemObject,
                                                           const IntelIOSurfaceProperties* props,
                                                           uint32_t* outIOSurfaceID)
{
    if (!gemObject || !props || !outIOSurfaceID) {
        return kIOReturnBadArgument;
    }

    IntelIOSurfaceProperties localProps = *props;
    localProps.gpuAddress = gemObject->getGPUAddress();
    localProps.creationTime = mach_absolute_time();
    localProps.lastAccessTime = localProps.creationTime;

    IntelIOSurfaceEntry* entry = (IntelIOSurfaceEntry*)IOMalloc(sizeof(IntelIOSurfaceEntry));
    if (!entry) {
        return kIOReturnNoMemory;
    }

    bzero(entry, sizeof(IntelIOSurfaceEntry));
    entry->gemObject = gemObject;
    entry->props = localProps;
    entry->backing = NULL;
    entry->port = MACH_PORT_NULL;
    entry->refCount = 1;
    entry->inUse = false;
    entry->lastAccess = localProps.lastAccessTime;

    IOLockLock(m_surfaceLock);

    entry->iosurfaceID = m_nextIOSurfaceID++;
    if (m_nextIOSurfaceID == 0) {
        m_nextIOSurfaceID = 1;
    }
    entry->props.iosurfaceID = entry->iosurfaceID;

    if (createMachPort(entry->iosurfaceID, &entry->port) == kIOReturnSuccess) {
        entry->props.iosurfacePort = entry->port;
    }

    insertEntry(entry);
    m_activeSurfaceCount++;

    IOLockUnlock(m_surfaceLock);

    updateMemoryStats(localProps.size, true);
    updateFormatStats(localProps.pixelFormat);

    *outIOSurfaceID = entry->iosurfaceID;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::createSurfaceFromDescriptor(IOMemoryDescriptor* descriptor,
                                                            const IntelIOSurfaceProperties* props,
                                                            uint32_t* outIOSurfaceID)
{
    if (!descriptor || !props || !outIOSurfaceID) {
        return kIOReturnBadArgument;
    }

    if (!m_controller || !m_gtt) {
        return kIOReturnNotReady;
    }

    IntelIOSurfaceProperties localProps = *props;
    localProps.size = descriptor->getLength();
    localProps.creationTime = mach_absolute_time();
    localProps.lastAccessTime = localProps.creationTime;

    IOReturn prepareResult = descriptor->prepare(kIODirectionOutIn);
    if (prepareResult != kIOReturnSuccess) {
        return prepareResult;
    }

    IOPhysicalLength segLen = 0;
    IOPhysicalAddress physAddr = descriptor->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    if (!physAddr) {
        descriptor->complete(kIODirectionOutIn);
        return kIOReturnNoMemory;
    }

    uint32_t gttOffset = m_gtt->bindSurfacePages(physAddr, localProps.size);
    if (gttOffset == 0) {
        descriptor->complete(kIODirectionOutIn);
        return kIOReturnNoMemory;
    }

    localProps.gpuAddress = gttOffset;
    localProps.physAddress = physAddr;

    IntelIOSurfaceEntry* entry = (IntelIOSurfaceEntry*)IOMalloc(sizeof(IntelIOSurfaceEntry));
    if (!entry) {
        m_gtt->unbindSurfacePages(gttOffset, localProps.size);
        descriptor->complete(kIODirectionOutIn);
        return kIOReturnNoMemory;
    }

    bzero(entry, sizeof(IntelIOSurfaceEntry));
    entry->gemObject = NULL;
    entry->props = localProps;
    entry->backing = descriptor;
    entry->backing->retain();
    entry->port = MACH_PORT_NULL;
    entry->refCount = 1;
    entry->inUse = false;
    entry->lastAccess = localProps.lastAccessTime;

    IOLockLock(m_surfaceLock);

    entry->iosurfaceID = m_nextIOSurfaceID++;
    if (m_nextIOSurfaceID == 0) {
        m_nextIOSurfaceID = 1;
    }
    entry->props.iosurfaceID = entry->iosurfaceID;

    if (createMachPort(entry->iosurfaceID, &entry->port) == kIOReturnSuccess) {
        entry->props.iosurfacePort = entry->port;
    }

    insertEntry(entry);
    m_activeSurfaceCount++;

    IOLockUnlock(m_surfaceLock);

    updateMemoryStats(localProps.size, true);
    updateFormatStats(localProps.pixelFormat);

    *outIOSurfaceID = entry->iosurfaceID;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::destroySurface(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);

    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }

    removeEntry(iosurfaceID);
    m_activeSurfaceCount--;

    IOLockUnlock(m_surfaceLock);

    if (entry->gemObject && m_gem) {
        m_gem->destroyObject(entry->gemObject);
    }
    if (entry->backing) {
        if (m_gtt && entry->props.gpuAddress) {
            m_gtt->unbindSurfacePages((uint32_t)entry->props.gpuAddress, entry->props.size);
        }
        entry->backing->complete(kIODirectionOutIn);
        entry->backing->release();
    }

    updateMemoryStats(entry->props.size, false);
    IOFree(entry, sizeof(IntelIOSurfaceEntry));

    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::retainSurface(uint32_t iosurfaceID)
{
    incrementRefCount(iosurfaceID);
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::releaseSurface(uint32_t iosurfaceID)
{
    decrementRefCount(iosurfaceID);
    return kIOReturnSuccess;
}

IntelIOSurfaceEntry* IntelIOSurfaceManager::findSurface(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    updateLookupStats(entry != NULL);
    IOLockUnlock(m_surfaceLock);
    return entry;
}

IOReturn IntelIOSurfaceManager::getSurfaceProperties(uint32_t iosurfaceID, IntelIOSurfaceProperties* props)
{
    if (!props) {
        return kIOReturnBadArgument;
    }

    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    *props = entry->props;
    entry->lastAccess = mach_absolute_time();
    IOLockUnlock(m_surfaceLock);

    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::setSurfaceProperties(uint32_t iosurfaceID, const IntelIOSurfaceProperties* props)
{
    if (!props) {
        return kIOReturnBadArgument;
    }

    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->props = *props;
    IOLockUnlock(m_surfaceLock);

    return kIOReturnSuccess;
}

IntelGEMObject* IntelIOSurfaceManager::getSurfaceBacking(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    IntelGEMObject* obj = entry ? entry->gemObject : NULL;
    IOLockUnlock(m_surfaceLock);
    return obj;
}

IOReturn IntelIOSurfaceManager::getSurfacePort(uint32_t iosurfaceID, mach_port_t* outPort)
{
    if (!outPort) {
        return kIOReturnBadArgument;
    }

    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    *outPort = entry->port;
    IOLockUnlock(m_surfaceLock);

    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::setSurfacePort(uint32_t iosurfaceID, mach_port_t port)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->port = port;
    IOLockUnlock(m_surfaceLock);
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::lockSurface(uint32_t iosurfaceID, uint32_t lockType, uint32_t timeoutMs)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->inUse = true;
    entry->lastAccess = mach_absolute_time();
    IOLockUnlock(m_surfaceLock);
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::unlockSurface(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->inUse = false;
    entry->lastAccess = mach_absolute_time();
    IOLockUnlock(m_surfaceLock);
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::compressSurface(uint32_t iosurfaceID, uint32_t compressionType)
{
    return kIOReturnUnsupported;
}

IOReturn IntelIOSurfaceManager::decompressSurface(uint32_t iosurfaceID)
{
    return kIOReturnUnsupported;
}

IOReturn IntelIOSurfaceManager::flushSurfaceCache(uint32_t iosurfaceID)
{
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::invalidateSurfaceCache(uint32_t iosurfaceID)
{
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::markForDisplay(uint32_t iosurfaceID, uint32_t displayID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->props.displayable = true;
    entry->props.displayID = displayID;
    IOLockUnlock(m_surfaceLock);
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::removeFromDisplay(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->props.displayable = false;
    entry->props.displayID = 0;
    IOLockUnlock(m_surfaceLock);
    return kIOReturnSuccess;
}

uint32_t IntelIOSurfaceManager::getDisplayableSurfaces(uint32_t* surfaceIDs, uint32_t maxCount)
{
    if (!surfaceIDs || maxCount == 0) {
        return 0;
    }

    uint32_t count = 0;
    IOLockLock(m_surfaceLock);
    for (uint32_t i = 0; i < IOSURFACE_HASH_BUCKETS && count < maxCount; i++) {
        IntelIOSurfaceEntry* entry = m_hashTable[i];
        while (entry && count < maxCount) {
            if (entry->props.displayable) {
                surfaceIDs[count++] = entry->iosurfaceID;
            }
            entry = entry->next;
        }
    }
    IOLockUnlock(m_surfaceLock);

    return count;
}

IOReturn IntelIOSurfaceManager::setAsFramebuffer(uint32_t iosurfaceID)
{
    m_framebufferSurfaceID = iosurfaceID;
    m_framebufferSet = true;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::getFramebuffer(uint32_t* outIOSurfaceID)
{
    if (!outIOSurfaceID) {
        return kIOReturnBadArgument;
    }
    if (!m_framebufferSet) {
        return kIOReturnNotFound;
    }
    *outIOSurfaceID = m_framebufferSurfaceID;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::registerWindowServerCallback(WindowServerCallback callback)
{
    m_windowServerCallback = callback;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::unregisterWindowServerCallback()
{
    m_windowServerCallback = NULL;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::notifySurfaceEvent(uint32_t iosurfaceID, uint32_t event)
{
    if (m_windowServerCallback) {
        m_windowServerCallback(iosurfaceID, event);
    }
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::setGlobalSurface(uint32_t iosurfaceID, bool global)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    if (!entry) {
        IOLockUnlock(m_surfaceLock);
        return kIOReturnNotFound;
    }
    entry->props.globalSurface = global;
    IOLockUnlock(m_surfaceLock);
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::getGlobalSurfaces(uint32_t* surfaceIDs, uint32_t* count)
{
    if (!surfaceIDs || !count) {
        return kIOReturnBadArgument;
    }

    uint32_t maxCount = *count;
    uint32_t found = 0;
    IOLockLock(m_surfaceLock);
    for (uint32_t i = 0; i < IOSURFACE_HASH_BUCKETS && found < maxCount; i++) {
        IntelIOSurfaceEntry* entry = m_hashTable[i];
        while (entry && found < maxCount) {
            if (entry->props.globalSurface) {
                surfaceIDs[found++] = entry->iosurfaceID;
            }
            entry = entry->next;
        }
    }
    IOLockUnlock(m_surfaceLock);

    *count = found;
    return kIOReturnSuccess;
}

void IntelIOSurfaceManager::handleMemoryPressure(uint32_t pressureLevel)
{
    m_memoryPressure = pressureLevel;
}

IOReturn IntelIOSurfaceManager::purgeSurfaces(uint32_t amountToPurge, uint64_t* actualPurged)
{
    if (actualPurged) {
        *actualPurged = 0;
    }
    return kIOReturnUnsupported;
}

uint64_t IntelIOSurfaceManager::getVRAMUsage()
{
    return m_totalMemoryUsage;
}

uint64_t IntelIOSurfaceManager::getVRAMPressure()
{
    return m_memoryPressure;
}

IOReturn IntelIOSurfaceManager::trimMemory(uint64_t targetSize)
{
    return kIOReturnUnsupported;
}

IOReturn IntelIOSurfaceManager::optimizeMemoryLayout()
{
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::compressIdleSurfaces()
{
    return kIOReturnUnsupported;
}

void IntelIOSurfaceManager::getStatistics(IOSurfaceStatistics* stats)
{
    if (!stats) {
        return;
    }

    IOLockLock(m_statsLock);
    *stats = m_stats;
    IOLockUnlock(m_statsLock);
}

void IntelIOSurfaceManager::resetStatistics()
{
    IOLockLock(m_statsLock);
    bzero(&m_stats, sizeof(m_stats));
    IOLockUnlock(m_statsLock);
}

void IntelIOSurfaceManager::printStatistics()
{
    IOSurfaceStatistics stats;
    getStatistics(&stats);
    IOLog("IntelIOSurfaceManager: active=%llu memory=%llu\n",
          stats.activeSurfaces, stats.currentMemoryUsage);
}

void IntelIOSurfaceManager::printActiveSurfaces()
{
    IOLockLock(m_surfaceLock);
    for (uint32_t i = 0; i < IOSURFACE_HASH_BUCKETS; i++) {
        IntelIOSurfaceEntry* entry = m_hashTable[i];
        while (entry) {
            IOLog("IOSurface %u: %ux%u format=0x%x gpu=0x%llx\n",
                  entry->iosurfaceID, entry->props.width, entry->props.height,
                  entry->props.pixelFormat, entry->props.gpuAddress);
            entry = entry->next;
        }
    }
    IOLockUnlock(m_surfaceLock);
}

bool IntelIOSurfaceManager::validateSurface(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    IOLockUnlock(m_surfaceLock);
    return entry != NULL;
}

IOReturn IntelIOSurfaceManager::dumpSurfaceInfo(uint32_t iosurfaceID)
{
    IntelIOSurfaceProperties props;
    IOReturn result = getSurfaceProperties(iosurfaceID, &props);
    if (result != kIOReturnSuccess) {
        return result;
    }
    IOLog("IOSurface %u: %ux%u format=0x%x size=%llu gpu=0x%llx\n",
          props.iosurfaceID, props.width, props.height, props.pixelFormat,
          props.size, props.gpuAddress);
    return kIOReturnSuccess;
}

void IntelIOSurfaceManager::dumpMemoryUsage()
{
    IOLog("IntelIOSurfaceManager: memory usage=%llu\n", m_totalMemoryUsage);
}

uint32_t IntelIOSurfaceManager::hashFunction(uint32_t iosurfaceID)
{
    return iosurfaceID % IOSURFACE_HASH_BUCKETS;
}

IntelIOSurfaceEntry* IntelIOSurfaceManager::findEntry(uint32_t iosurfaceID)
{
    uint32_t bucket = hashFunction(iosurfaceID);
    IntelIOSurfaceEntry* entry = m_hashTable[bucket];
    while (entry) {
        if (entry->iosurfaceID == iosurfaceID) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

void IntelIOSurfaceManager::insertEntry(IntelIOSurfaceEntry* entry)
{
    uint32_t bucket = hashFunction(entry->iosurfaceID);
    entry->next = m_hashTable[bucket];
    m_hashTable[bucket] = entry;
}

void IntelIOSurfaceManager::removeEntry(uint32_t iosurfaceID)
{
    uint32_t bucket = hashFunction(iosurfaceID);
    IntelIOSurfaceEntry* prev = NULL;
    IntelIOSurfaceEntry* entry = m_hashTable[bucket];
    while (entry) {
        if (entry->iosurfaceID == iosurfaceID) {
            if (prev) {
                prev->next = entry->next;
            } else {
                m_hashTable[bucket] = entry->next;
            }
            break;
        }
        prev = entry;
        entry = entry->next;
    }
}

IOReturn IntelIOSurfaceManager::allocateBackingMemory(IntelIOSurfaceProperties* props,
                                                     IntelGEMObject** outGEMObject)
{
    if (!props || !outGEMObject || !m_gem) {
        return kIOReturnBadArgument;
    }
    IntelGEMObject* obj = m_gem->createObject(props->size, I915_BO_ALLOC_USER);
    if (!obj) {
        return kIOReturnNoMemory;
    }
    *outGEMObject = obj;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::setupMultiplanarFormats(IntelIOSurfaceProperties* props)
{
    if (!props) {
        return kIOReturnBadArgument;
    }

    if (props->pixelFormat == kIOSurfacePixelFormatYUV420) {
        props->planeCount = 2;
        props->planeOffsets[0] = 0;
        props->planeOffsets[1] = props->bytesPerRow * props->height;
        props->planeWidths[0] = props->width;
        props->planeWidths[1] = props->width;
        props->planeHeights[0] = props->height;
        props->planeHeights[1] = props->height / 2;
        props->planeBytesPerRow[0] = props->bytesPerRow;
        props->planeBytesPerRow[1] = props->bytesPerRow;
    } else {
        props->planeCount = 1;
    }
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::calculateMemoryRequirements(const IntelIOSurfaceProperties* props,
                                                           uint64_t* outSize, uint32_t* outAlignment)
{
    if (!props || !outSize || !outAlignment) {
        return kIOReturnBadArgument;
    }

    uint32_t bpp = props->bytesPerPixel;
    if (bpp == 0) {
        switch (props->pixelFormat) {
            case kIOSurfacePixelFormatBGRA8:
            case kIOSurfacePixelFormatRGBA8:
                bpp = 4;
                break;
            case kIOSurfacePixelFormatRGB565:
                bpp = 2;
                break;
            case kIOSurfacePixelFormatYUV420:
                bpp = 1;
                break;
            default:
                bpp = 4;
                break;
        }
    }

    uint32_t bytesPerRow = props->bytesPerRow;
    if (bytesPerRow == 0) {
        bytesPerRow = props->width * bpp;
    }

    uint64_t size = (uint64_t)bytesPerRow * props->height;
    if (props->pixelFormat == kIOSurfacePixelFormatYUV420) {
        size = (uint64_t)bytesPerRow * props->height * 3 / 2;
    }

    *outSize = size;
    *outAlignment = 4096;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::createMachPort(uint32_t iosurfaceID, mach_port_t* outPort)
{
    if (!outPort) {
        return kIOReturnBadArgument;
    }
    *outPort = MACH_PORT_NULL;
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::destroyMachPort(mach_port_t port)
{
    return kIOReturnSuccess;
}

IOReturn IntelIOSurfaceManager::setupCompression(IntelIOSurfaceProperties* props)
{
    if (!props) {
        return kIOReturnBadArgument;
    }
    props->compressionEnabled = false;
    return kIOReturnSuccess;
}

uint64_t IntelIOSurfaceManager::calculateCompressedSize(uint32_t width, uint32_t height, uint32_t format)
{
    return (uint64_t)width * height * 4;
}

uint32_t IntelIOSurfaceManager::incrementRefCount(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    uint32_t count = 0;
    if (entry) {
        entry->refCount++;
        count = entry->refCount;
    }
    IOLockUnlock(m_surfaceLock);
    return count;
}

uint32_t IntelIOSurfaceManager::decrementRefCount(uint32_t iosurfaceID)
{
    IOLockLock(m_surfaceLock);
    IntelIOSurfaceEntry* entry = findEntry(iosurfaceID);
    uint32_t count = 0;
    if (entry && entry->refCount > 0) {
        entry->refCount--;
        count = entry->refCount;
    }
    IOLockUnlock(m_surfaceLock);
    return count;
}

void IntelIOSurfaceManager::updateCreationStats(uint64_t creationTime)
{
    IOLockLock(m_statsLock);
    m_stats.totalSurfacesCreated++;
    m_stats.activeSurfaces = m_activeSurfaceCount;
    if (m_activeSurfaceCount > m_stats.peakActiveSurfaces) {
        m_stats.peakActiveSurfaces = m_activeSurfaceCount;
    }
    IOLockUnlock(m_statsLock);
}

void IntelIOSurfaceManager::updateLookupStats(bool hit)
{
    IOLockLock(m_statsLock);
    if (hit) {
        m_stats.lookupHits++;
    } else {
        m_stats.lookupMisses++;
    }
    uint64_t total = m_stats.lookupHits + m_stats.lookupMisses;
    if (total) {
        m_stats.hitRatio = (float)m_stats.lookupHits / (float)total;
    }
    IOLockUnlock(m_statsLock);
}

void IntelIOSurfaceManager::updateMemoryStats(uint64_t delta, bool allocation)
{
    IOLockLock(m_statsLock);
    if (allocation) {
        m_stats.totalMemoryAllocated += delta;
        m_stats.currentMemoryUsage += delta;
        if (m_stats.currentMemoryUsage > m_stats.peakMemoryUsage) {
            m_stats.peakMemoryUsage = m_stats.currentMemoryUsage;
        }
    } else {
        if (m_stats.currentMemoryUsage >= delta) {
            m_stats.currentMemoryUsage -= delta;
        } else {
            m_stats.currentMemoryUsage = 0;
        }
    }
    IOLockUnlock(m_statsLock);

    if (allocation) {
        m_totalMemoryUsage += delta;
        if (m_totalMemoryUsage > m_peakMemoryUsage) {
            m_peakMemoryUsage = m_totalMemoryUsage;
        }
    } else {
        if (m_totalMemoryUsage >= delta) {
            m_totalMemoryUsage -= delta;
        } else {
            m_totalMemoryUsage = 0;
        }
    }
}

void IntelIOSurfaceManager::updateFormatStats(uint32_t pixelFormat)
{
    IOLockLock(m_statsLock);
    switch (pixelFormat) {
        case kIOSurfacePixelFormatBGRA8:
            m_stats.bgra8Surfaces++;
            break;
        case kIOSurfacePixelFormatYUV420:
            m_stats.yuv420Surfaces++;
            break;
        case kIOSurfacePixelFormatYUV422:
            m_stats.yuv422Surfaces++;
            break;
        default:
            m_stats.otherSurfaces++;
            break;
    }
    IOLockUnlock(m_statsLock);
}

void IntelIOSurfaceManager::updateUsageStats(uint32_t usage)
{
    IOLockLock(m_statsLock);
    if (usage & kIOSurfaceDisplayable) {
        m_stats.displaySurfaces++;
    }
    IOLockUnlock(m_statsLock);
}

void IntelIOSurfaceManager::cleanupStaleSurfaces()
{
}

void IntelIOSurfaceManager::performMaintenance()
{
}

bool IntelIOSurfaceManager::validateProperties(const IntelIOSurfaceProperties* props)
{
    if (!props) {
        return false;
    }
    if (!validateDimensions(props->width, props->height)) {
        return false;
    }
    if (!validatePixelFormat(props->pixelFormat)) {
        return false;
    }
    if (props->size > IOSURFACE_MAX_SIZE) {
        return false;
    }
    return true;
}

bool IntelIOSurfaceManager::validatePixelFormat(uint32_t pixelFormat)
{
    switch (pixelFormat) {
        case kIOSurfacePixelFormatBGRA8:
        case kIOSurfacePixelFormatRGBA8:
        case kIOSurfacePixelFormatYUV420:
        case kIOSurfacePixelFormatYUV422:
        case kIOSurfacePixelFormatRGB565:
        case kIOSurfacePixelFormatRGB101010:
            return true;
        default:
            return true;
    }
}

bool IntelIOSurfaceManager::validateDimensions(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) {
        return false;
    }
    if (width > 16384 || height > 16384) {
        return false;
    }
    return true;
}
