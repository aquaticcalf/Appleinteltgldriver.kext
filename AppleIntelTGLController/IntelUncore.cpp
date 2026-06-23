/*
 * IntelUncore.cpp
 * 
 * Register access layer with forcewake support
 * Ported from Linux intel_uncore.c
 */

#include "IntelUncore.h"
#include "AppleIntelTGLController.h"
#include <IOKit/IOLib.h>


// Initialization

bool IntelUncore::init(AppleIntelTGLController *ctrl, void *base, size_t size)
{
    IOLog("IntelUncore: init() called\n");
    
    if (!ctrl || !base || size == 0) {
        IOLog("IntelUncore: Invalid parameters\n");
        return false;
    }
    
    controller = ctrl;
    mmioBase = base;
    mmioSize = size;
    
    // Initialize locks
    mmioLock = IOLockAlloc();
    if (!mmioLock) {
        IOLog("IntelUncore: Failed to allocate MMIO lock\n");
        return false;
    }
    
    forcewakeLock = IOLockAlloc();
    if (!forcewakeLock) {
        IOLog("IntelUncore: Failed to allocate forcewake lock\n");
        IOLockFree(mmioLock);
        mmioLock = NULL;
        return false;
    }
    
    // Initialize state
    fw_domains_active = 0;
    unclaimed_mmio_check = true;
    read_count = 0;
    write_count = 0;
    forcewake_count = 0;
    unclaimed_mmio_count = 0;
    
    // Clear forcewake domain structures
    for (int i = 0; i < FW_DOMAIN_ID_COUNT; i++) {
        fw_domains[i].mask = 0;
        fw_domains[i].reg_set = 0;
        fw_domains[i].reg_ack = 0;
        fw_domains[i].val_set = 0;
        fw_domains[i].val_clear = 0;
        fw_domains[i].val_reset = 0;
        fw_domains[i].active = false;
    }
    
    // Detect and setup forcewake domains
    detectForcewakeConfig();
    if (!setupForcewakeDomains()) {
        IOLog("IntelUncore: Failed to setup forcewake domains\n");
        cleanup();
        return false;
    }
    
    initialized = true;
    
    IOLog("IntelUncore: Initialized successfully\n");
    IOLog("IntelUncore: MMIO Base: %p, Size: 0x%lx\n", mmioBase, mmioSize);
    
    return true;
}

void IntelUncore::cleanup()
{
    IOLog("IntelUncore: cleanup() called\n");
    
    initialized = false;
    
    // Release all forcewake domains
    if (fw_domains_active != 0) {
        IOLog("IntelUncore: Releasing active forcewake domains: 0x%x\n", 
              fw_domains_active);
        forcewakePut((enum forcewake_domains)fw_domains_active);
    }
    
    // Free locks
    if (forcewakeLock) {
        IOLockFree(forcewakeLock);
        forcewakeLock = NULL;
    }
    
    if (mmioLock) {
        IOLockFree(mmioLock);
        mmioLock = NULL;
    }
    
    // Print statistics
    IOLog("IntelUncore: Statistics:\n");
    IOLog("  Reads: %llu\n", read_count);
    IOLog("  Writes: %llu\n", write_count);
    IOLog("  Forcewake ops: %llu\n", forcewake_count);
    IOLog("  Unclaimed MMIO: %llu\n", unclaimed_mmio_count);
    
    mmioBase = NULL;
    mmioSize = 0;
    controller = NULL;
}


// Low-level MMIO Access (inline for performance)

inline u8 IntelUncore::rawRead8(u32 offset)
{
    volatile u8 *ptr = (volatile u8 *)((uintptr_t)mmioBase + offset);
    return *ptr;
}

inline u16 IntelUncore::rawRead16(u32 offset)
{
    volatile u16 *ptr = (volatile u16 *)((uintptr_t)mmioBase + offset);
    return *ptr;
}

inline u32 IntelUncore::rawRead32(u32 offset)
{
    volatile u32 *ptr = (volatile u32 *)((uintptr_t)mmioBase + offset);
    return *ptr;
}

inline u64 IntelUncore::rawRead64(u32 offset)
{
    volatile u64 *ptr = (volatile u64 *)((uintptr_t)mmioBase + offset);
    return *ptr;
}

inline void IntelUncore::rawWrite8(u32 offset, u8 value)
{
    volatile u8 *ptr = (volatile u8 *)((uintptr_t)mmioBase + offset);
    *ptr = value;
}

inline void IntelUncore::rawWrite16(u32 offset, u16 value)
{
    volatile u16 *ptr = (volatile u16 *)((uintptr_t)mmioBase + offset);
    *ptr = value;
}

inline void IntelUncore::rawWrite32(u32 offset, u32 value)
{
    volatile u32 *ptr = (volatile u32 *)((uintptr_t)mmioBase + offset);
    *ptr = value;
}

inline void IntelUncore::rawWrite64(u32 offset, u64 value)
{
    volatile u64 *ptr = (volatile u64 *)((uintptr_t)mmioBase + offset);
    *ptr = value;
}


// Public Register Access

u8 IntelUncore::readRegister8(u32 offset)
{
    IOLockLock(mmioLock);
    u8 value = rawRead8(offset);
    read_count++;
    IOLockUnlock(mmioLock);
    return value;
}

u16 IntelUncore::readRegister16(u32 offset)
{
    IOLockLock(mmioLock);
    u16 value = rawRead16(offset);
    read_count++;
    IOLockUnlock(mmioLock);
    return value;
}

u32 IntelUncore::readRegister32(u32 offset)
{
    IOLockLock(mmioLock);
    u32 value = rawRead32(offset);
    read_count++;
    IOLockUnlock(mmioLock);
    return value;
}

u64 IntelUncore::readRegister64(u32 offset)
{
    IOLockLock(mmioLock);
    u64 value = rawRead64(offset);
    read_count++;
    IOLockUnlock(mmioLock);
    return value;
}

void IntelUncore::writeRegister8(u32 offset, u8 value)
{
    IOLockLock(mmioLock);
    rawWrite8(offset, value);
    write_count++;
    IOLockUnlock(mmioLock);
}

void IntelUncore::writeRegister16(u32 offset, u16 value)
{
    IOLockLock(mmioLock);
    rawWrite16(offset, value);
    write_count++;
    IOLockUnlock(mmioLock);
}

void IntelUncore::writeRegister32(u32 offset, u32 value)
{
    IOLockLock(mmioLock);
    rawWrite32(offset, value);
    write_count++;
    IOLockUnlock(mmioLock);
}

void IntelUncore::writeRegister64(u32 offset, u64 value)
{
    IOLockLock(mmioLock);
    rawWrite64(offset, value);
    write_count++;
    IOLockUnlock(mmioLock);
}

void IntelUncore::postingRead32(u32 offset)
{
    // Read to ensure previous write completes
    (void)readRegister32(offset);
}


// Register Access with Forcewake

u32 IntelUncore::readRegister32_fw(u32 offset)
{
    // Get required forcewake domains for this register
    enum forcewake_domains domains = getForcewakeDomains(offset, false);
    
    if (domains != 0) {
        forcewakeGet(domains);
    }
    
    u32 value = readRegister32(offset);
    
    if (domains != 0) {
        forcewakePut(domains);
    }
    
    return value;
}

void IntelUncore::writeRegister32_fw(u32 offset, u32 value)
{
    // Get required forcewake domains for this register
    enum forcewake_domains domains = getForcewakeDomains(offset, true);
    
    if (domains != 0) {
        forcewakeGet(domains);
    }
    
    writeRegister32(offset, value);
    
    if (domains != 0) {
        forcewakePut(domains);
    }
}


// Forcewake Operations

void IntelUncore::forcewakeGet(enum forcewake_domains domains)
{
    if (!initialized || domains == 0) {
        return;
    }
    
    IOLockLock(forcewakeLock);
    
    forcewake_count++;
    
    // Set each requested domain
    for (int i = 0; i < FW_DOMAIN_ID_COUNT; i++) {
        u32 domain_mask = (1 << i);
        if ((domains & domain_mask) && fw_domains[i].reg_set != 0) {
            forcewakeSetDomain((enum forcewake_domain_id)i);
        }
    }
    
    // Wait for acknowledgment
    for (int i = 0; i < FW_DOMAIN_ID_COUNT; i++) {
        u32 domain_mask = (1 << i);
        if ((domains & domain_mask) && fw_domains[i].reg_set != 0) {
            if (!forcewakeWaitAck((enum forcewake_domain_id)i, 
                                 TGL_REGS::FORCEWAKE_ACK_TIMEOUT_MS)) {
                IOLog("IntelUncore: Forcewake domain %d acknowledge timeout!\n", i);
            }
        }
    }
    
    fw_domains_active |= domains;
    
    IOLockUnlock(forcewakeLock);
}

void IntelUncore::forcewakePut(enum forcewake_domains domains)
{
    if (!initialized || domains == 0) {
        return;
    }
    
    IOLockLock(forcewakeLock);
    
    // Clear each requested domain
    for (int i = 0; i < FW_DOMAIN_ID_COUNT; i++) {
        u32 domain_mask = (1 << i);
        if ((domains & domain_mask) && fw_domains[i].reg_set != 0) {
            forcewakeClearDomain((enum forcewake_domain_id)i);
        }
    }
    
    fw_domains_active &= ~domains;
    
    IOLockUnlock(forcewakeLock);
}

bool IntelUncore::forcewakeWaitAck(enum forcewake_domain_id domain_id, u32 timeout_ms)
{
    if (domain_id >= FW_DOMAIN_ID_COUNT || fw_domains[domain_id].reg_ack == 0) {
        return false;
    }
    
    u32 reg_ack = fw_domains[domain_id].reg_ack;
    u32 ack_value = TGL_REGS::FORCEWAKE_MT_ACK;
    
    // Wait for acknowledge bit
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeout_ms, kMillisecondScale, &deadline);
    
    while (true) {
        u32 value = rawRead32(reg_ack);
        if (value & ack_value) {
            return true;  // Acknowledged
        }
        
        AbsoluteTime now;
        clock_get_uptime(&now);
        if (CMP_ABSOLUTETIME(&now, &deadline) > 0) {
            return false;  // Timeout
        }
        
        // Small delay before retry
        IODelay(1);  // 1 microsecond
    }
}


// Forcewake Helpers

void IntelUncore::forcewakeSetDomain(enum forcewake_domain_id domain_id)
{
    if (domain_id >= FW_DOMAIN_ID_COUNT) {
        return;
    }
    
    struct forcewake_domain *d = &fw_domains[domain_id];
    if (d->reg_set == 0) {
        return;
    }
    
    // Write set value to forcewake register
    rawWrite32(d->reg_set, d->val_set);
    d->active = true;
}

void IntelUncore::forcewakeClearDomain(enum forcewake_domain_id domain_id)
{
    if (domain_id >= FW_DOMAIN_ID_COUNT) {
        return;
    }
    
    struct forcewake_domain *d = &fw_domains[domain_id];
    if (d->reg_set == 0) {
        return;
    }
    
    // Write clear value to forcewake register
    rawWrite32(d->reg_set, d->val_clear);
    d->active = false;
}

bool IntelUncore::checkForcewakeAck(enum forcewake_domain_id domain_id)
{
    if (domain_id >= FW_DOMAIN_ID_COUNT) {
        return false;
    }
    
    struct forcewake_domain *d = &fw_domains[domain_id];
    if (d->reg_ack == 0) {
        return true;  // No ack register, assume OK
    }
    
    u32 value = rawRead32(d->reg_ack);
    return (value & TGL_REGS::FORCEWAKE_MT_ACK) != 0;
}


// Forcewake Configuration

void IntelUncore::detectForcewakeConfig()
{
    IOLog("IntelUncore: Detecting forcewake configuration\n");
    
    // For now, assume Tiger Lake (Gen12)
    // TODO: Detect actual platform from device info
    
    if (isTigerLake() || isGen12()) {
        IOLog("IntelUncore: Detected Gen12/Tiger Lake\n");
    } else {
        IOLog("IntelUncore: Warning: Unknown platform, assuming Gen12\n");
    }
}

bool IntelUncore::setupForcewakeDomains()
{
    IOLog("IntelUncore: Setting up forcewake domains for Gen12\n");
    
    // Tiger Lake (Gen12) forcewake configuration
    
    // Render domain
    fw_domains[FW_DOMAIN_ID_RENDER].mask = FORCEWAKE_RENDER;
    fw_domains[FW_DOMAIN_ID_RENDER].reg_set = TGL_REGS::FORCEWAKE_RENDER_GEN9;
    fw_domains[FW_DOMAIN_ID_RENDER].reg_ack = TGL_REGS::FORCEWAKE_ACK_RENDER_GEN9;
    fw_domains[FW_DOMAIN_ID_RENDER].val_set = TGL_REGS::FORCEWAKE_KERNEL;
    fw_domains[FW_DOMAIN_ID_RENDER].val_clear = 0;
    fw_domains[FW_DOMAIN_ID_RENDER].val_reset = 0;
    fw_domains[FW_DOMAIN_ID_RENDER].active = false;
    
    // GT domain (includes blitter)
    fw_domains[FW_DOMAIN_ID_GT].mask = FORCEWAKE_GT;
    fw_domains[FW_DOMAIN_ID_GT].reg_set = TGL_REGS::FORCEWAKE_GT_GEN9;
    fw_domains[FW_DOMAIN_ID_GT].reg_ack = TGL_REGS::FORCEWAKE_ACK_GT_GEN9;
    fw_domains[FW_DOMAIN_ID_GT].val_set = TGL_REGS::FORCEWAKE_KERNEL;
    fw_domains[FW_DOMAIN_ID_GT].val_clear = 0;
    fw_domains[FW_DOMAIN_ID_GT].val_reset = 0;
    fw_domains[FW_DOMAIN_ID_GT].active = false;
    
    // Media domain
    fw_domains[FW_DOMAIN_ID_MEDIA].mask = FORCEWAKE_MEDIA;
    fw_domains[FW_DOMAIN_ID_MEDIA].reg_set = TGL_REGS::FORCEWAKE_MEDIA_GEN9;
    fw_domains[FW_DOMAIN_ID_MEDIA].reg_ack = TGL_REGS::FORCEWAKE_ACK_MEDIA_GEN9;
    fw_domains[FW_DOMAIN_ID_MEDIA].val_set = TGL_REGS::FORCEWAKE_KERNEL;
    fw_domains[FW_DOMAIN_ID_MEDIA].val_clear = 0;
    fw_domains[FW_DOMAIN_ID_MEDIA].val_reset = 0;
    fw_domains[FW_DOMAIN_ID_MEDIA].active = false;
    
    IOLog("IntelUncore: Forcewake domains configured\n");
    IOLog("  Render: reg=0x%x, ack=0x%x\n", 
          fw_domains[FW_DOMAIN_ID_RENDER].reg_set,
          fw_domains[FW_DOMAIN_ID_RENDER].reg_ack);
    IOLog("  GT: reg=0x%x, ack=0x%x\n",
          fw_domains[FW_DOMAIN_ID_GT].reg_set,
          fw_domains[FW_DOMAIN_ID_GT].reg_ack);
    IOLog("  Media: reg=0x%x, ack=0x%x\n",
          fw_domains[FW_DOMAIN_ID_MEDIA].reg_set,
          fw_domains[FW_DOMAIN_ID_MEDIA].reg_ack);
    
    return true;
}

enum forcewake_domains IntelUncore::getForcewakeDomains(u32 offset, bool is_write)
{
    // For now, return FORCEWAKE_ALL for safety
    // TODO: Implement proper register range detection
    // based on i915_reg.h register definitions
    
    // Ranges that don't need forcewake (always accessible)
    if (offset < 0x4000) {
        return (enum forcewake_domains)0;  // Legacy VGA range
    }
    
    // Most GT registers need GT or RENDER forcewake
    // For now, be conservative and use both
    return (enum forcewake_domains)(FORCEWAKE_GT | FORCEWAKE_RENDER);
}



bool IntelUncore::isTigerLake() const
{
    // TODO: Check device ID from controller
    return true;  // Assume Tiger Lake for now
}

bool IntelUncore::isGen12() const
{
    return isTigerLake();  // Tiger Lake is Gen12
}


// Debug / Error Checking

bool IntelUncore::checkForUnclaimedMMIO(u32 offset)
{
    if (!unclaimed_mmio_check) {
        return false;
    }
    
    // TODO: Check for unclaimed MMIO errors
    // On real hardware, this reads a status register
    return false;
}

void IntelUncore::reportUnclaimedMMIO(u32 offset, bool is_write)
{
    unclaimed_mmio_count++;
    IOLog("IntelUncore: WARNING: Unclaimed MMIO %s at offset 0x%x (count: %llu)\n",
          is_write ? "write" : "read", offset, unclaimed_mmio_count);
}
