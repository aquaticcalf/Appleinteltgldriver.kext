/*
 * IntelGEM.cpp
 *
 * Graphics Execution Manager - Main memory manager implementation
 * Ported from Linux i915_gem.c
 */

#include "IntelGEM.h"
#include "AppleIntelTGLController.h"
#include "IntelUncore.h"
#include <IOKit/IOLib.h>


// Initialization

bool IntelGEM::init(AppleIntelTGLController *ctrl)
{
    IOLog("IntelGEM: init() called\n");
    
    if (!ctrl) {
        IOLog("IntelGEM: Invalid controller\n");
        return false;
    }
    
    controller = ctrl;
    
    // Initialize object list
    object_list_head = NULL;
    object_list_tail = NULL;
    
    object_list_lock = IOLockAlloc();
    if (!object_list_lock) {
        IOLog("IntelGEM: Failed to allocate object_list_lock\n");
        return false;
    }
    
    // Initialize statistics
    memset(&stats, 0, sizeof(stats));
    
    stats_lock = IOLockAlloc();
    if (!stats_lock) {
        IOLog("IntelGEM: Failed to allocate stats_lock\n");
        IOLockFree(object_list_lock);
        object_list_lock = NULL;
        return false;
    }
    
    // Set memory limit - 2GB for modern workloads (Tiger Lake supports up to 2.5GB shared)
    max_memory = 2ULL * 1024 * 1024 * 1024;  // 2GB
    
    initialized = true;
    
    IOLog("IntelGEM: Initialized successfully\n");
    IOLog("IntelGEM: Max memory: %llu MB\n", max_memory / (1024 * 1024));
    
    return true;
}

void IntelGEM::cleanup()
{
    IOLog("IntelGEM: cleanup() called\n");
    
    initialized = false;
    
    // Print final statistics
    printStatistics();
    
    // Destroy all remaining objects
    IOLockLock(object_list_lock);
    
    IntelGEMObject *obj = object_list_head;
    while (obj) {
        IntelGEMObject *next = obj->next;
        IOLog("IntelGEM: WARNING - Object %p still exists at cleanup\n", obj);
        obj->destroy();
        obj = next;
    }
    
    object_list_head = NULL;
    object_list_tail = NULL;
    
    IOLockUnlock(object_list_lock);
    
    // Free locks
    if (stats_lock) {
        IOLockFree(stats_lock);
        stats_lock = NULL;
    }
    
    if (object_list_lock) {
        IOLockFree(object_list_lock);
        object_list_lock = NULL;
    }
    
    controller = NULL;
    
    IOLog("IntelGEM: Cleanup complete\n");
}


// Object Management

IntelGEMObject* IntelGEM::createObject(u64 size, u32 flags)
{
    if (!initialized) {
        IOLog("IntelGEM: Not initialized\n");
        return NULL;
    }
    
    // Check memory limit
    if (!checkMemoryLimit(size)) {
        IOLog("IntelGEM: Memory limit exceeded for allocation of %llu bytes\n", size);
        IOLockLock(stats_lock);
        stats.allocation_failures++;
        IOLockUnlock(stats_lock);
        return NULL;
    }
    
    // Create object
    IntelGEMObject *obj = IntelGEMObject::create(this, size, flags);
    if (!obj) {
        IOLog("IntelGEM: Failed to create object\n");
        IOLockLock(stats_lock);
        stats.allocation_failures++;
        IOLockUnlock(stats_lock);
        return NULL;
    }
    
    // Track object
    trackObject(obj);
    
    // Update statistics
    updateMemoryStats(size);
    
    IOLog("IntelGEM: Created object %p, size=%llu bytes (active: %llu objects, %llu MB)\n",
          obj, size, stats.active_objects, stats.active_memory / (1024 * 1024));
    
    return obj;
}

void IntelGEM::destroyObject(IntelGEMObject *obj)
{
    if (!obj) {
        return;
    }
    
    u64 size = obj->getSize();
    
    // Untrack object
    untrackObject(obj);
    
    // Update statistics
    updateMemoryStats(-(s64)size);
    
    // Destroy object
    obj->destroy();
    
    IOLog("IntelGEM: Destroyed object %p (active: %llu objects, %llu MB)\n",
          obj, stats.active_objects, stats.active_memory / (1024 * 1024));
}


// Object Tracking

void IntelGEM::trackObject(IntelGEMObject *obj)
{
    if (!obj) {
        return;
    }
    
    IOLockLock(object_list_lock);
    addToList(obj);
    IOLockUnlock(object_list_lock);
}

void IntelGEM::untrackObject(IntelGEMObject *obj)
{
    if (!obj) {
        return;
    }
    
    IOLockLock(object_list_lock);
    removeFromList(obj);
    IOLockUnlock(object_list_lock);
}

void IntelGEM::addToList(IntelGEMObject *obj)
{
    obj->next = NULL;
    obj->prev = object_list_tail;
    
    if (object_list_tail) {
        object_list_tail->next = obj;
    } else {
        object_list_head = obj;
    }
    
    object_list_tail = obj;
}

void IntelGEM::removeFromList(IntelGEMObject *obj)
{
    if (obj->prev) {
        obj->prev->next = obj->next;
    } else {
        object_list_head = obj->next;
    }
    
    if (obj->next) {
        obj->next->prev = obj->prev;
    } else {
        object_list_tail = obj->prev;
    }
    
    obj->next = NULL;
    obj->prev = NULL;
}


// Memory Management

bool IntelGEM::allocateMemory(IntelGEMObject *obj)
{
    // Object allocates its own memory
    // This is just for accounting
    return true;
}

void IntelGEM::freeMemory(IntelGEMObject *obj)
{
    // Object frees its own memory
    // This is just for accounting
}

bool IntelGEM::checkMemoryLimit(u64 size)
{
    IOLockLock(stats_lock);
    bool ok = (stats.active_memory + size <= max_memory);
    IOLockUnlock(stats_lock);
    return ok;
}

void IntelGEM::updateMemoryStats(s64 delta)
{
    IOLockLock(stats_lock);
    
    if (delta > 0) {
        stats.total_objects++;
        stats.active_objects++;
        stats.total_memory += delta;
        stats.active_memory += delta;
        
        if (stats.active_memory > stats.peak_memory) {
            stats.peak_memory = stats.active_memory;
        }
    } else {
        stats.active_objects--;
        stats.active_memory -= (-delta);
    }
    
    IOLockUnlock(stats_lock);
}



bool IntelGEM::waitForIdle(u64 timeout_ms)
{
    IOLog("IntelGEM: waitForIdle - timeout=%llu ms\n", timeout_ms);
    
    // TODO: Wait for all GPU operations to complete
    // For now, just iterate through objects
    
    IOLockLock(object_list_lock);
    
    IntelGEMObject *obj = object_list_head;
    while (obj) {
        if (!obj->waitIdle(timeout_ms * 1000000ULL)) {
            IOLog("IntelGEM: Object %p failed to become idle\n", obj);
            IOLockUnlock(object_list_lock);
            return false;
        }
        obj = obj->next;
    }
    
    IOLockUnlock(object_list_lock);
    
    IOLog("IntelGEM: All objects idle\n");
    return true;
}

bool IntelGEM::flushAll()
{
    IOLog("IntelGEM: flushAll\n");
    
    IOLockLock(object_list_lock);
    
    IntelGEMObject *obj = object_list_head;
    while (obj) {
        obj->flush();
        obj = obj->next;
    }
    
    IOLockUnlock(object_list_lock);
    
    return true;
}


// Statistics

void IntelGEM::getStatistics(struct gem_stats *out_stats)
{
    if (!out_stats) {
        return;
    }
    
    IOLockLock(stats_lock);
    memcpy(out_stats, &stats, sizeof(struct gem_stats));
    IOLockUnlock(stats_lock);
}

void IntelGEM::printStatistics()
{
    IOLockLock(stats_lock);
    
    IOLog("IntelGEM: Statistics:\n");
    IOLog("  Total objects created: %llu\n", stats.total_objects);
    IOLog("  Active objects: %llu\n", stats.active_objects);
    IOLog("  Total memory allocated: %llu MB\n", stats.total_memory / (1024 * 1024));
    IOLog("  Active memory: %llu MB\n", stats.active_memory / (1024 * 1024));
    IOLog("  Peak memory: %llu MB\n", stats.peak_memory / (1024 * 1024));
    IOLog("  Allocation failures: %llu\n", stats.allocation_failures);
    
    IOLockUnlock(stats_lock);
}


// Accessors

IntelUncore* IntelGEM::getUncore() const
{
    return controller ? controller->getUncore() : NULL;
}
