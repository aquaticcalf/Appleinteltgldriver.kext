/*
 * IntelGEMObject.cpp
 * 
 * Graphics Execution Manager - Buffer Object Implementation
 * Ported from Linux gem_object.c
 */

#include "IntelGEMObject.h"
#include "IntelGEM.h"
#include <IOKit/IOLib.h>


// Creation and Destruction

IntelGEMObject::IntelGEMObject()
{
    gem = NULL;
    memory_descriptor = NULL;
    memory_map = NULL;
    cpu_address = NULL;
    gpu_address = 0;
    size = 0;
    cache_level = I915_CACHE_NONE;
    read_domains = 0;
    write_domain = 0;
    flags = 0;
    cpu_mapped = false;
    gtt_mapped = false;
    is_coherent = false;
    ref_count = 1;
    ref_lock = NULL;
    map_count = 0;
    created_time = 0;
    last_access_time = 0;
    next = NULL;
    prev = NULL;
}

IntelGEMObject::~IntelGEMObject()
{
    cleanup();
}

IntelGEMObject* IntelGEMObject::create(IntelGEM *gem_mgr, u64 obj_size, u32 obj_flags)
{
    if (!gem_mgr || obj_size == 0) {
        IOLog("IntelGEMObject: Invalid parameters\n");
        return NULL;
    }
    
    // Check size limits
    if (i915_gem_object_size_2big(obj_size)) {
        IOLog("IntelGEMObject: Size too large: %llu bytes\n", obj_size);
        return NULL;
    }
    
    // Allocate object
    IntelGEMObject *obj = new IntelGEMObject();
    if (!obj) {
        IOLog("IntelGEMObject: Failed to allocate object\n");
        return NULL;
    }
    
    // Initialize
    if (!obj->init(gem_mgr, obj_size, obj_flags)) {
        IOLog("IntelGEMObject: Initialization failed\n");
        delete obj;
        return NULL;
    }
    
    IOLog("IntelGEMObject: Created object %p, size=%llu bytes\n", obj, obj_size);
    return obj;
}

void IntelGEMObject::destroy()
{
    IOLog("IntelGEMObject: Destroying object %p (ref_count=%llu)\n", this, ref_count);
    
    // Should only destroy when ref_count reaches 0
    if (ref_count > 0) {
        IOLog("IntelGEMObject: WARNING - Destroying with ref_count=%llu\n", ref_count);
    }
    
    delete this;
}

bool IntelGEMObject::init(IntelGEM *gem_mgr, u64 obj_size, u32 obj_flags)
{
    gem = gem_mgr;
    size = obj_size;
    flags = obj_flags;
    
    // Create reference count lock
    ref_lock = IOLockAlloc();
    if (!ref_lock) {
        IOLog("IntelGEMObject: Failed to allocate ref_lock\n");
        return false;
    }
    
    // Allocate memory
    if (!allocateMemory()) {
        IOLog("IntelGEMObject: Failed to allocate memory\n");
        cleanup();
        return false;
    }
    
    // Allocate GPU address (GTT entry)
    if (!allocateGPUAddress()) {
        IOLog("IntelGEMObject: Failed to allocate GPU address\n");
        cleanup();
        return false;
    }
    
    // Record creation time
    clock_sec_t secs;
    clock_nsec_t nsecs;
    clock_get_system_nanotime(&secs, &nsecs);
    created_time = (u64)secs * 1000000000ULL + nsecs;
    last_access_time = created_time;
    
    // Set default cache level
    cache_level = I915_CACHE_LLC;  // Use CPU cache by default
    
    // Set initial domains
    read_domains = I915_GEM_DOMAIN_CPU;
    write_domain = I915_GEM_DOMAIN_CPU;
    
    IOLog("IntelGEMObject: Initialized - size=%llu, gpu_addr=0x%llx\n", 
          size, gpu_address);
    
    return true;
}

void IntelGEMObject::cleanup()
{
    // Unmap if mapped
    if (cpu_mapped) {
        unmapCPU();
    }
    
    if (gtt_mapped) {
        unmapGTT();
    }
    
    // Free GPU address
    freeGPUAddress();
    
    // Free memory
    freeMemory();
    
    // Free lock
    if (ref_lock) {
        IOLockFree(ref_lock);
        ref_lock = NULL;
    }
    
    gem = NULL;
}


// Memory Allocation

bool IntelGEMObject::allocateMemory()
{
    // Use IOBufferMemoryDescriptor for now (simple allocation)
    // TODO: Support different memory types (stolen, system, etc.)
    
    IOOptionBits options = kIOMemoryKernelUserShared;
    
    if (flags & I915_BO_ALLOC_CONTIGUOUS) {
        options |= kIOMemoryPhysicallyContiguous;
    }
    
    memory_descriptor = IOBufferMemoryDescriptor::withOptions(
        options,
        size,
        PAGE_SIZE  // Alignment
    );
    
    if (!memory_descriptor) {
        IOLog("IntelGEMObject: Failed to allocate IOBufferMemoryDescriptor\n");
        return false;
    }
    
    memory_descriptor->retain();
    
    IOLog("IntelGEMObject: Allocated memory descriptor %p\n", memory_descriptor);
    return true;
}

void IntelGEMObject::freeMemory()
{
    if (memory_descriptor) {
        memory_descriptor->release();
        memory_descriptor = NULL;
    }
}

bool IntelGEMObject::allocateGPUAddress()
{
    // For now, use a simple scheme: GPU address = physical address
    // TODO: Implement proper GTT allocation and management
    
    if (!memory_descriptor) {
        return false;
    }
    
    IOPhysicalAddress phys_addr = memory_descriptor->getPhysicalAddress();
    if (phys_addr == 0) {
        IOLog("IntelGEMObject: Failed to get physical address\n");
        return false;
    }
    
    gpu_address = (u64)phys_addr;
    
    IOLog("IntelGEMObject: GPU address allocated: 0x%llx\n", gpu_address);
    return true;
}

void IntelGEMObject::freeGPUAddress()
{
    // TODO: Release GTT entry
    gpu_address = 0;
}


// Reference Counting

void IntelGEMObject::retain()
{
    IOLockLock(ref_lock);
    ref_count++;
    IOLockUnlock(ref_lock);
}

void IntelGEMObject::release()
{
    IOLockLock(ref_lock);
    ref_count--;
    bool should_destroy = (ref_count == 0);
    IOLockUnlock(ref_lock);
    
    if (should_destroy) {
        destroy();
    }
}


// Memory Mapping

bool IntelGEMObject::mapCPU(void **address)
{
    if (!memory_descriptor) {
        IOLog("IntelGEMObject: No memory descriptor to map\n");
        return false;
    }
    
    if (cpu_mapped) {
        // Already mapped
        *address = cpu_address;
        return true;
    }
    
    // Map memory for CPU access
    memory_map = memory_descriptor->map();
    if (!memory_map) {
        IOLog("IntelGEMObject: Failed to map memory\n");
        return false;
    }
    
    memory_map->retain();
    cpu_address = (void*)memory_map->getVirtualAddress();
    cpu_mapped = true;
    map_count++;
    
    // Update access time
    clock_sec_t secs;
    clock_nsec_t nsecs;
    clock_get_system_nanotime(&secs, &nsecs);
    last_access_time = (u64)secs * 1000000000ULL + nsecs;
    
    *address = cpu_address;
    
    IOLog("IntelGEMObject: Mapped to CPU address %p\n", cpu_address);
    return true;
}

void IntelGEMObject::unmapCPU()
{
    if (!cpu_mapped) {
        return;
    }
    
    if (memory_map) {
        memory_map->release();
        memory_map = NULL;
    }
    
    cpu_address = NULL;
    cpu_mapped = false;
    
    IOLog("IntelGEMObject: Unmapped from CPU\n");
}

bool IntelGEMObject::mapGTT(u64 *address)
{
    if (gpu_address == 0) {
        IOLog("IntelGEMObject: No GPU address allocated\n");
        return false;
    }
    
    // GPU mapping is already done via GTT allocation
    gtt_mapped = true;
    *address = gpu_address;
    
    IOLog("IntelGEMObject: GTT mapped to 0x%llx\n", gpu_address);
    return true;
}

void IntelGEMObject::unmapGTT()
{
    gtt_mapped = false;
    IOLog("IntelGEMObject: GTT unmapped\n");
}


// Domain Management

bool IntelGEMObject::setDomain(u32 new_read_domains, u32 new_write_domain)
{
    IOLog("IntelGEMObject: setDomain - read=0x%x, write=0x%x\n",
          new_read_domains, new_write_domain);
    
    // Validate domains
    if (new_write_domain != 0 && new_write_domain != I915_GEM_DOMAIN_CPU &&
        !(new_write_domain & I915_GEM_GPU_DOMAINS)) {
        IOLog("IntelGEMObject: Invalid write domain 0x%x\n", new_write_domain);
        return false;
    }
    
    // Can only have one write domain
    if (new_write_domain != 0 && (new_write_domain & (new_write_domain - 1))) {
        IOLog("IntelGEMObject: Multiple write domains not allowed\n");
        return false;
    }
    
    // TODO: Implement proper domain transitions with cache flushing
    // For now, just update the domains
    
    read_domains = new_read_domains;
    write_domain = new_write_domain;
    
    return true;
}


// Cache Control

bool IntelGEMObject::setCacheLevel(enum intel_cache_level level)
{
    IOLog("IntelGEMObject: setCacheLevel - level=%d\n", level);
    
    if (cache_level == level) {
        return true;  // Already set
    }
    
    // TODO: Implement cache level changes
    // This requires flushing caches and updating PTEs
    
    cache_level = level;
    return true;
}



bool IntelGEMObject::waitIdle(u64 timeout_ns)
{
    // TODO: Wait for GPU to finish using this object
    // For now, just return true (assume idle)
    
    IOLog("IntelGEMObject: waitIdle - timeout=%llu ns\n", timeout_ns);
    return true;
}

bool IntelGEMObject::flush()
{
    // TODO: Flush GPU caches for this object
    IOLog("IntelGEMObject: flush\n");
    return true;
}

bool IntelGEMObject::finish()
{
    // Wait for idle and flush
    return waitIdle(UINT64_MAX) && flush();
}
