/*
 * IntelMetalCommandQueue.cpp - Metal Command Queue Implementation
 * Week 42: Metal Commands - Full Implementation
 * 
 * Complete implementation of Metal command queue with lifecycle management,
 * priority scheduling, completion tracking, and performance statistics.
 */

#include "IntelMetalCommandQueue.h"
#include "IntelMetalCommandBuffer.h"
#include "AppleIntelTGLIOAccelerator.h"
#include "IntelGuCSubmission.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>

#define super OSObject
OSDefineMetaClassAndStructors(IntelMetalCommandQueue, OSObject)

// Constants
#define kDefaultMaxPendingBuffers        256
#define kDefaultCompletionCheckInterval  10  // ms
#define kMaxLabelLength                  63


// MARK: - Factory & Lifecycle


IntelMetalCommandQueue* IntelMetalCommandQueue::withAccelerator(
    IntelIOAccelerator* accel,
    MetalCommandQueueType type,
    MetalQueuePriority priority)
{
    if (!accel) {
        IOLog("IntelMetalCommandQueue: ERROR - NULL accelerator\n");
        return NULL;
    }
    
    // Create default configuration
    MetalQueueConfiguration config = {};
    config.type = type;
    config.priority = priority;
    config.flags = kMetalQueueFlagNone;
    config.maxPendingBuffers = kDefaultMaxPendingBuffers;
    config.completionCheckInterval = kDefaultCompletionCheckInterval;
    config.enableStatistics = true;
    config.enableProfiling = false;
    config.label = NULL;
    
    IntelMetalCommandQueue* queue = new IntelMetalCommandQueue;
    if (!queue) {
        return NULL;
    }
    
    if (!queue->initWithConfiguration(accel, &config)) {
        queue->release();
        return NULL;
    }
    
    return queue;
}

bool IntelMetalCommandQueue::initWithConfiguration(
    IntelIOAccelerator* accel,
    const MetalQueueConfiguration* configPtr)
{
    if (!super::init()) {
        return false;
    }
    
    if (!accel || !configPtr) {
        IOLog("IntelMetalCommandQueue: ERROR - Invalid parameters\n");
        return false;
    }
    
    // Store references
    accelerator = accel;
    accelerator->retain();
    
    submission = accel->getSubmission();
    if (!submission) {
        IOLog("IntelMetalCommandQueue: ERROR - No GuC submission\n");
        return false;
    }
    
    // Copy configuration
    memcpy(&config, configPtr, sizeof(MetalQueueConfiguration));
    queueType = config.type;
    basePriority = config.priority;
    currentPriority = basePriority;
    queueFlags = config.flags;
    
    // Set label
    if (config.label) {
        strncpy(label, config.label, kMaxLabelLength);
        label[kMaxLabelLength] = '\0';
    } else {
        const char* typeNames[] = { "Render", "Compute", "Blit" };
        snprintf(label, sizeof(label), "CommandQueue-%s-%d",
                typeNames[queueType], (int)basePriority);
    }
    
    // Create tracking array
    pendingCommandBuffers = OSArray::withCapacity(config.maxPendingBuffers);
    if (!pendingCommandBuffers) {
        IOLog("IntelMetalCommandQueue: ERROR - Failed to create tracking array\n");
        return false;
    }
    
    // Create locks
    trackingLock = IOLockAlloc();
    statsLock = IOLockAlloc();
    
    if (!trackingLock || !statsLock) {
        IOLog("IntelMetalCommandQueue: ERROR - Failed to allocate locks\n");
        return false;
    }
    
    // Get work loop from accelerator
    workLoop = accel->getWorkLoop();
    if (!workLoop) {
        IOLog("IntelMetalCommandQueue: ERROR - No work loop\n");
        return false;
    }
    workLoop->retain();
    
    // Create completion timer
    if (config.completionCheckInterval > 0) {
        completionTimer = IOTimerEventSource::timerEventSource(
            this,
            reinterpret_cast<IOTimerEventSource::Action>(
                &IntelMetalCommandQueue::completionTimerFired));
        
        if (completionTimer) {
            workLoop->addEventSource(completionTimer);
            completionTimer->setTimeoutMS(config.completionCheckInterval);
        }
    } else {
        completionTimer = NULL;
    }
    
    // Create priority boost timer
    priorityTimer = IOTimerEventSource::timerEventSource(
        this,
        reinterpret_cast<IOTimerEventSource::Action>(
            &IntelMetalCommandQueue::priorityBoostExpired));
    
    if (priorityTimer) {
        workLoop->addEventSource(priorityTimer);
    }
    
    // Initialize state
    suspended = false;
    priorityBoosted = false;
    initialized = true;
    
    // Initialize statistics
    memset(&stats, 0, sizeof(stats));
    lastSubmitTime = 0;
    lastCompleteTime = 0;
    
    IOLog("IntelMetalCommandQueue: OK  Command queue initialized\n");
    IOLog("IntelMetalCommandQueue:   Label: %s\n", label);
    IOLog("IntelMetalCommandQueue:   Type: %s\n",
          queueType == kMetalCommandQueueTypeRender ? "Render" :
          queueType == kMetalCommandQueueTypeCompute ? "Compute" : "Blit");
    IOLog("IntelMetalCommandQueue:   Priority: %s\n",
          basePriority == kMetalQueuePriorityLow ? "Low" :
          basePriority == kMetalQueuePriorityNormal ? "Normal" :
          basePriority == kMetalQueuePriorityHigh ? "High" : "Realtime");
    IOLog("IntelMetalCommandQueue:   Max pending: %u buffers\n",
          config.maxPendingBuffers);
    
    return true;
}

void IntelMetalCommandQueue::free() {
    if (initialized) {
        // Wait for pending command buffers
        waitUntilIdle(5000000000ULL); // 5 second timeout
        
        // Stop timers
        if (completionTimer) {
            completionTimer->cancelTimeout();
            workLoop->removeEventSource(completionTimer);
            completionTimer->release();
            completionTimer = NULL;
        }
        
        if (priorityTimer) {
            priorityTimer->cancelTimeout();
            workLoop->removeEventSource(priorityTimer);
            priorityTimer->release();
            priorityTimer = NULL;
        }
    }
    
    OSSafeReleaseNULL(pendingCommandBuffers);
    OSSafeReleaseNULL(workLoop);
    OSSafeReleaseNULL(accelerator);
    
    if (trackingLock) {
        IOLockFree(trackingLock);
        trackingLock = NULL;
    }
    
    if (statsLock) {
        IOLockFree(statsLock);
        statsLock = NULL;
    }
    
    super::free();
}


// MARK: - Command Buffer Management


IntelMetalCommandBuffer* IntelMetalCommandQueue::commandBuffer() {
    if (!initialized || suspended) {
        return NULL;
    }
    
    // Check pending limit
    IOLockLock(trackingLock);
    uint32_t pendingCount = pendingCommandBuffers->getCount();
    IOLockUnlock(trackingLock);
    
    if (pendingCount >= config.maxPendingBuffers) {
        IOLog("IntelMetalCommandQueue: WARNING - Max pending buffers (%u) reached\n",
              config.maxPendingBuffers);
        return NULL;
    }
    
    // Create command buffer
    IntelMetalCommandBuffer* cmdBuffer =
        IntelMetalCommandBuffer::withCommandQueue(this);
    
    if (cmdBuffer) {
        // Update statistics
        IOLockLock(statsLock);
        stats.commandBuffersCreated++;
        IOLockUnlock(statsLock);
        
        IOLog("IntelMetalCommandQueue: Created command buffer #%llu\n",
              stats.commandBuffersCreated);
    }
    
    return cmdBuffer;
}

IntelMetalCommandBuffer* IntelMetalCommandQueue::commandBufferWithUnretainedReferences() {
    // For now, same as regular command buffer
    // In real implementation, would use different memory management
    return commandBuffer();
}

IOReturn IntelMetalCommandQueue::submitCommandBuffer(IntelMetalCommandBuffer* cmdBuffer) {
    if (!initialized || !cmdBuffer) {
        return kIOReturnBadArgument;
    }
    
    if (suspended) {
        IOLog("IntelMetalCommandQueue: ERROR - Queue suspended\n");
        return kIOReturnNotPermitted;
    }
    
    uint64_t startTime = mach_absolute_time();
    
    IOLog("IntelMetalCommandQueue: Submitting command buffer...\n");
    
    // Track command buffer
    IOReturn ret = trackCommandBuffer(cmdBuffer);
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalCommandQueue: ERROR - Failed to track buffer\n");
        return ret;
    }
    
    // Submit to GPU via GuC
    ret = cmdBuffer->submitToGPU();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalCommandQueue: ERROR - GPU submission failed: 0x%x\n", ret);
        untrackCommandBuffer(cmdBuffer);
        return ret;
    }
    
    // Update statistics
    uint64_t endTime = mach_absolute_time();
    uint64_t latency = (endTime - startTime) / 1000; // Convert to uss
    
    IOLockLock(statsLock);
    stats.commandBuffersSubmitted++;
    stats.averageSubmissionLatency =
        (stats.averageSubmissionLatency * (stats.commandBuffersSubmitted - 1) + latency) /
        stats.commandBuffersSubmitted;
    lastSubmitTime = endTime;
    IOLockUnlock(statsLock);
    
    IOLog("IntelMetalCommandQueue: OK  Command buffer submitted\n");
    IOLog("IntelMetalCommandQueue:   Submission latency: %llu uss\n", latency);
    IOLog("IntelMetalCommandQueue:   Pending buffers: %u\n",
          pendingCommandBuffers->getCount());
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalCommandQueue::waitForCommandBuffer(IntelMetalCommandBuffer* cmdBuffer,
                                                      uint64_t timeoutNs) {
    if (!cmdBuffer) {
        return kIOReturnBadArgument;
    }
    
    return cmdBuffer->waitUntilCompleted(timeoutNs);
}

void IntelMetalCommandQueue::notifyCommandBufferCompleted(
    IntelMetalCommandBuffer* cmdBuffer,
    IOReturn status)
{
    if (!cmdBuffer) {
        return;
    }
    
    IOLog("IntelMetalCommandQueue: Command buffer completed (status: 0x%x)\n", status);
    
    // Untrack buffer
    untrackCommandBuffer(cmdBuffer);
    
    // Update statistics
    uint64_t currentTime = mach_absolute_time();
    
    IOLockLock(statsLock);
    if (status == kIOReturnSuccess) {
        stats.commandBuffersCompleted++;
    } else {
        stats.commandBuffersFailed++;
    }
    
    if (lastSubmitTime > 0) {
        uint64_t completionLatency = (currentTime - lastSubmitTime) / 1000;
        stats.averageCompletionLatency =
            (stats.averageCompletionLatency * (stats.commandBuffersCompleted - 1) +
             completionLatency) / stats.commandBuffersCompleted;
    }
    
    lastCompleteTime = currentTime;
    IOLockUnlock(statsLock);
}


// MARK: - Queue Control


IOReturn IntelMetalCommandQueue::suspend() {
    if (suspended) {
        return kIOReturnSuccess;
    }
    
    IOLog("IntelMetalCommandQueue: Suspending queue '%s'\n", label);
    
    suspended = true;
    
    // Stop completion timer
    if (completionTimer) {
        completionTimer->cancelTimeout();
    }
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalCommandQueue::resume() {
    if (!suspended) {
        return kIOReturnSuccess;
    }
    
    IOLog("IntelMetalCommandQueue: Resuming queue '%s'\n", label);
    
    suspended = false;
    
    // Restart completion timer
    if (completionTimer) {
        completionTimer->setTimeoutMS(config.completionCheckInterval);
    }
    
    return kIOReturnSuccess;
}

bool IntelMetalCommandQueue::isSuspended() const {
    return suspended;
}

IOReturn IntelMetalCommandQueue::waitUntilIdle(uint64_t timeoutNs) {
    IOLog("IntelMetalCommandQueue: Waiting for %u pending buffers...\n",
          pendingCommandBuffers->getCount());
    
    uint64_t startTime = mach_absolute_time();
    uint64_t timeoutAbs = startTime + (timeoutNs / 1000); // Convert to absolute time
    
    while (true) {
        IOLockLock(trackingLock);
        uint32_t pendingCount = pendingCommandBuffers->getCount();
        IOLockUnlock(trackingLock);
        
        if (pendingCount == 0) {
            IOLog("IntelMetalCommandQueue: OK  Queue idle\n");
            return kIOReturnSuccess;
        }
        
        uint64_t currentTime = mach_absolute_time();
        if (currentTime >= timeoutAbs) {
            IOLog("IntelMetalCommandQueue: ERROR - Timeout waiting for idle (%u still pending)\n",
                  pendingCount);
            return kIOReturnTimeout;
        }
        
        // Check completions
        checkCompletedCommandBuffers();
        
        // Brief sleep
        IOSleep(10);
    }
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalCommandQueue::flush() {
    // Force check for completed command buffers
    checkCompletedCommandBuffers();
    return kIOReturnSuccess;
}


// MARK: - Priority Management


MetalQueuePriority IntelMetalCommandQueue::getPriority() const {
    return currentPriority;
}

IOReturn IntelMetalCommandQueue::setPriority(MetalQueuePriority priority) {
    if (priority > kMetalQueuePriorityRealtime) {
        return kIOReturnBadArgument;
    }
    
    IOLog("IntelMetalCommandQueue: Changing priority: %d -> %d\n",
          currentPriority, priority);
    
    basePriority = priority;
    if (!priorityBoosted) {
        currentPriority = priority;
    }
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalCommandQueue::boostPriority(uint64_t durationNs) {
    if (currentPriority >= kMetalQueuePriorityRealtime) {
        return kIOReturnSuccess; // Already at max
    }
    
    IOLog("IntelMetalCommandQueue: Boosting priority for %llu ns\n", durationNs);
    
    currentPriority = kMetalQueuePriorityRealtime;
    priorityBoosted = true;
    
    // Set timer to restore priority
    if (priorityTimer) {
        priorityTimer->setTimeoutUS(durationNs / 1000);
    }
    
    return kIOReturnSuccess;
}


// MARK: - Queue Properties


void IntelMetalCommandQueue::setLabel(const char* newLabel) {
    if (newLabel) {
        strncpy(label, newLabel, kMaxLabelLength);
        label[kMaxLabelLength] = '\0';
        IOLog("IntelMetalCommandQueue: Label updated: %s\n", label);
    }
}


// MARK: - Statistics & Profiling


void IntelMetalCommandQueue::getStatistics(MetalQueueStatistics* outStats) {
    if (!outStats) {
        return;
    }
    
    IOLockLock(statsLock);
    memcpy(outStats, &stats, sizeof(MetalQueueStatistics));
    
    // Update current pending count
    IOLockLock(trackingLock);
    outStats->currentPendingBuffers = pendingCommandBuffers->getCount();
    IOLockUnlock(trackingLock);
    
    IOLockUnlock(statsLock);
}

void IntelMetalCommandQueue::resetStatistics() {
    IOLockLock(statsLock);
    memset(&stats, 0, sizeof(stats));
    lastSubmitTime = 0;
    lastCompleteTime = 0;
    IOLockUnlock(statsLock);
    
    IOLog("IntelMetalCommandQueue: Statistics reset\n");
}

uint32_t IntelMetalCommandQueue::getPendingCommandBufferCount() const {
    IOLockLock(trackingLock);
    uint32_t count = pendingCommandBuffers->getCount();
    IOLockUnlock(trackingLock);
    return count;
}

IOReturn IntelMetalCommandQueue::insertDebugCaptureBoundary() {
    return kIOReturnSuccess;
}


// MARK: - Internal Methods


IOReturn IntelMetalCommandQueue::trackCommandBuffer(IntelMetalCommandBuffer* cmdBuffer) {
    IOLockLock(trackingLock);
    
    // Check if already tracked
    unsigned int index = pendingCommandBuffers->getNextIndexOfObject(cmdBuffer, 0);
    if (index != (unsigned int)-1) {
        IOLockUnlock(trackingLock);
        return kIOReturnSuccess;
    }
    
    // Add to tracking array
    bool added = pendingCommandBuffers->setObject(cmdBuffer);
    
    if (added) {
        cmdBuffer->retain();
        
        // Update peak
        uint32_t count = pendingCommandBuffers->getCount();
        IOLockLock(statsLock);
        if (count > stats.peakPendingBuffers) {
            stats.peakPendingBuffers = count;
        }
        IOLockUnlock(statsLock);
    }
    
    IOLockUnlock(trackingLock);
    
    return added ? kIOReturnSuccess : kIOReturnNoMemory;
}

IOReturn IntelMetalCommandQueue::untrackCommandBuffer(IntelMetalCommandBuffer* cmdBuffer) {
    IOLockLock(trackingLock);
    
    unsigned int index = pendingCommandBuffers->getNextIndexOfObject(cmdBuffer, 0);
    if (index != (unsigned int)-1) {
        pendingCommandBuffers->removeObject(index);
        cmdBuffer->release();
    }
    
    IOLockUnlock(trackingLock);
    
    return kIOReturnSuccess;
}

void IntelMetalCommandQueue::completionTimerFired(OSObject* owner,
                                                  IOTimerEventSource* timer) {
    IntelMetalCommandQueue* queue = OSDynamicCast(IntelMetalCommandQueue, owner);
    if (queue) {
        queue->checkCompletedCommandBuffers();
        
        // Reschedule timer
        if (!queue->suspended) {
            timer->setTimeoutMS(queue->config.completionCheckInterval);
        }
    }
}

void IntelMetalCommandQueue::checkCompletedCommandBuffers() {
    IOLockLock(trackingLock);
    
    // Iterate through pending buffers
    for (unsigned int i = 0; i < pendingCommandBuffers->getCount(); ) {
        IntelMetalCommandBuffer* cmdBuffer =
            OSDynamicCast(IntelMetalCommandBuffer, pendingCommandBuffers->getObject(i));
        
        if (cmdBuffer && cmdBuffer->isCompleted()) {
            IOLog("IntelMetalCommandQueue: Detected completed buffer at index %u\n", i);
            pendingCommandBuffers->removeObject(i);
            cmdBuffer->release();
            // Don't increment i, since we removed an element
        } else {
            i++;
        }
    }
    
    IOLockUnlock(trackingLock);
}

void IntelMetalCommandQueue::priorityBoostExpired(OSObject* owner,
                                                  IOTimerEventSource* timer) {
    IntelMetalCommandQueue* queue = OSDynamicCast(IntelMetalCommandQueue, owner);
    if (queue) {
        queue->restoreBasePriority();
    }
}

void IntelMetalCommandQueue::restoreBasePriority() {
    if (priorityBoosted) {
        IOLog("IntelMetalCommandQueue: Restoring base priority: %d\n", basePriority);
        currentPriority = basePriority;
        priorityBoosted = false;
    }
}
