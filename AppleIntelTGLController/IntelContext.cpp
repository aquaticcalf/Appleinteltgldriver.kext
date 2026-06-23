/*
 * IntelContext.cpp
 *
 * GPU context management implementation
 */

#include "IntelContext.h"
#include "AppleIntelTGLController.h"
#include "IntelGEM.h"
#include "IntelGEMObject.h"
#include "IntelGTT.h"
#include "IntelRingBuffer.h"
#include "IntelPPGTT.h"
#include <IOKit/IOLib.h>

static u32 g_nextContextId = 1;

IntelContext::IntelContext()
    : controller(NULL)
    , contextId(0)
    , flags(0)
    , active(false)
    , contextObj(NULL)
    , contextVirtual(NULL)
    , contextGpuAddress(0)
    , ppgtt(NULL)
    , boundRing(NULL)
    , statsLock(NULL)
{
    bzero(&stats, sizeof(stats));
}

IntelContext::~IntelContext()
{
    cleanup();
}

bool IntelContext::init(AppleIntelTGLController *ctrl, u32 ctxId)
{
    if (!ctrl) {
        IOLog("IntelContext: Invalid controller\n");
        return false;
    }
    
    controller = ctrl;
    contextId = (ctxId != 0) ? ctxId : generateContextId();
    
    IOLog("IntelContext: Initializing context ID %u\n", contextId);
    
    // Create statistics lock
    statsLock = IOLockAlloc();
    if (!statsLock) {
        IOLog("IntelContext: Failed to allocate stats lock\n");
        return false;
    }
    
    // Allocate context state object
    if (!allocateContextState()) {
        IOLog("IntelContext: Failed to allocate context state\n");
        return false;
    }
    
    // Create PPGTT (Per-Process GTT)
    if (!createPPGTT()) {
        IOLog("IntelContext: Failed to create PPGTT\n");
        return false;
    }
    
    IOLog("IntelContext: Context %u initialized successfully\n", contextId);
    return true;
}

void IntelContext::cleanup()
{
    IOLog("IntelContext: Cleaning up context %u\n", contextId);
    
    // Deactivate if active
    if (active) {
        deactivate();
    }
    
    // Unbind ring
    if (boundRing) {
        unbindRing();
    }
    
    // Cleanup PPGTT
    if (ppgtt) {
        ppgtt->cleanup();
        delete ppgtt;
        ppgtt = NULL;
    }
    
    // Release context state
    if (contextObj) {
        if (contextVirtual) {
            contextObj->unmapCPU();
            contextVirtual = NULL;
        }
        
        IntelGTT *gtt = controller->getGTT();
        if (gtt && contextGpuAddress) {
            gtt->unbindObject(contextObj);
            gtt->freeSpace(contextGpuAddress, contextObj->getSize());
        }
        
        IntelGEM *gem = controller->getGEM();
        if (gem) {
            gem->destroyObject(contextObj);
        }
        contextObj = NULL;
    }
    
    // Free locks
    if (statsLock) {
        IOLockFree(statsLock);
        statsLock = NULL;
    }
    
    printStats();
    IOLog("IntelContext: Cleanup complete\n");
}

u32 IntelContext::generateContextId()
{
    return OSIncrementAtomic((SInt32 *)&g_nextContextId);
}


// Context State Management

bool IntelContext::allocateContextState()
{
    IOLog("IntelContext: Allocating context state (%u KB)\n",
          GEN12_CONTEXT_SIZE / 1024);
    
    IntelGEM *gem = controller->getGEM();
    if (!gem) {
        IOLog("IntelContext: No GEM manager\n");
        return false;
    }
    
    // Create context state object
    contextObj = gem->createObject(GEN12_CONTEXT_SIZE, 0);
    if (!contextObj) {
        IOLog("IntelContext: Failed to create context object\n");
        return false;
    }
    
    // Allocate GTT space
    IntelGTT *gtt = controller->getGTT();
    if (!gtt) {
        IOLog("IntelContext: No GTT manager\n");
        return false;
    }
    
    contextGpuAddress = gtt->allocateSpace(GEN12_CONTEXT_SIZE, 4096);
    if (contextGpuAddress == 0) {
        IOLog("IntelContext: Failed to allocate GTT space\n");
        return false;
    }
    
    contextObj->setGTTAddress(contextGpuAddress);
    
    // Bind to GTT
    if (!gtt->bindObject(contextObj, 0)) {
        IOLog("IntelContext: Failed to bind context object\n");
        return false;
    }
    
    // Map for CPU access
    if (!contextObj->mapCPU(&contextVirtual)) {
        IOLog("IntelContext: Failed to map context state\n");
        return false;
    }
    
    // Initialize context state
    bzero(contextVirtual, GEN12_CONTEXT_SIZE);
    
    IOLog("IntelContext: Context state at GPU 0x%llx, CPU %p\n",
          contextGpuAddress, contextVirtual);
    
    return true;
}

bool IntelContext::writeContextState()
{
    if (!contextVirtual || !boundRing) {
        return false;
    }
    
    // Write basic context state
    // In a full implementation, this would write ring state,
    // page table pointers, and GPU registers
    
    volatile u32 *state = (volatile u32 *)contextVirtual;
    
    // Context control
    state[0] = 0x00000001;  // Valid context
    
    // Ring buffer state (if bound)
    if (boundRing) {
        state[1] = boundRing->getHead();
        state[2] = boundRing->getTail();
        state[3] = (u32)boundRing->getRingAddress();
        state[4] = (u32)(boundRing->getRingAddress() >> 32);
    }
    
    // PPGTT pointer
    if (ppgtt) {
        state[5] = (u32)ppgtt->getRootAddress();
        state[6] = (u32)(ppgtt->getRootAddress() >> 32);
    }
    
    return true;
}

bool IntelContext::readContextState()
{
    if (!contextVirtual) {
        return false;
    }
    
    volatile u32 *state = (volatile u32 *)contextVirtual;
    
    // Read context control
    u32 control = state[0];
    
    if ((control & 0x1) == 0) {
        IOLog("IntelContext: Context %u is not valid\n", contextId);
        return false;
    }
    
    return true;
}

bool IntelContext::saveState()
{
    if (!active) {
        return true;
    }
    
    IOLog("IntelContext: Saving context %u state\n", contextId);
    
    // Read current ring state
    if (boundRing) {
        // Ring will update its own state
    }
    
    // Write to context object
    if (!writeContextState()) {
        IOLog("IntelContext: Failed to write context state\n");
        return false;
    }
    
    return true;
}

bool IntelContext::restoreState()
{
    IOLog("IntelContext: Restoring context %u state\n", contextId);
    
    // Read from context object
    if (!readContextState()) {
        IOLog("IntelContext: Failed to read context state\n");
        return false;
    }
    
    // Restore ring state
    if (boundRing) {
        // Ring will restore its own state
    }
    
    return true;
}


// PPGTT Management

bool IntelContext::createPPGTT()
{
    IOLog("IntelContext: Creating PPGTT for context %u\n", contextId);
    
    ppgtt = new IntelPPGTT();
    if (!ppgtt) {
        IOLog("IntelContext: Failed to allocate PPGTT\n");
        return false;
    }
    
    if (!ppgtt->init(controller)) {
        IOLog("IntelContext: Failed to initialize PPGTT\n");
        delete ppgtt;
        ppgtt = NULL;
        return false;
    }
    
    IOLog("IntelContext: PPGTT created successfully\n");
    return true;
}


// Ring Binding

bool IntelContext::bindRing(IntelRingBuffer *ring)
{
    if (!ring) {
        return false;
    }
    
    if (boundRing) {
        IOLog("IntelContext: Ring already bound, unbinding first\n");
        unbindRing();
    }
    
    boundRing = ring;
    
    IOLog("IntelContext: Bound ring to context %u\n", contextId);
    return true;
}

bool IntelContext::unbindRing()
{
    if (!boundRing) {
        return true;
    }
    
    IOLog("IntelContext: Unbinding ring from context %u\n", contextId);
    boundRing = NULL;
    
    return true;
}


// Context Control

bool IntelContext::activate()
{
    if (active) {
        return true;
    }
    
    IOLog("IntelContext: Activating context %u\n", contextId);
    
    // For first activation or default context, initialize instead of restore
    bool isFirstActivation = (stats.switches == 0);
    bool isDefaultContext = (flags & CONTEXT_DEFAULT);
    
    if (isFirstActivation || isDefaultContext) {
        IOLog("IntelContext: First activation - initializing context state\n");
        // Initialize context state to valid defaults
        if (contextVirtual) {
            volatile u32 *state = (volatile u32 *)contextVirtual;
            // Set context as valid
            state[0] = 0x1;  // Valid bit
            IOLog("IntelContext: Context %u initialized and marked valid\n", contextId);
        }
    } else {
        // Restore state for subsequent activations
        if (!restoreState()) {
            IOLog("IntelContext: Failed to restore state\n");
            return false;
        }
    }
    
    active = true;
    
    // Update statistics
    IOLockLock(statsLock);
    stats.switches++;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelContext::deactivate()
{
    if (!active) {
        return true;
    }
    
    IOLog("IntelContext: Deactivating context %u\n", contextId);
    
    // Save state
    if (!saveState()) {
        IOLog("IntelContext: Failed to save state\n");
        return false;
    }
    
    active = false;
    
    return true;
}

bool IntelContext::reset()
{
    IOLog("IntelContext: Resetting context %u\n", contextId);
    
    // Clear context state
    if (contextVirtual) {
        bzero(contextVirtual, GEN12_CONTEXT_SIZE);
    }
    
    // Reset statistics
    IOLockLock(statsLock);
    stats.last_seqno = 0;
    stats.hangs++;
    IOLockUnlock(statsLock);
    
    return true;
}


// Query

u64 IntelContext::getContextAddress() const
{
    return contextGpuAddress;
}


// Statistics

void IntelContext::getStats(struct context_stats *out)
{
    if (!out) {
        return;
    }
    
    IOLockLock(statsLock);
    memcpy(out, &stats, sizeof(struct context_stats));
    IOLockUnlock(statsLock);
}

void IntelContext::printStats()
{
    IOLog("IntelContext %u Statistics:\n", contextId);
    IOLog("  Switches: %llu\n", stats.switches);
    IOLog("  Submissions: %llu\n", stats.submissions);
    IOLog("  Active time: %llu ns\n", stats.active_time_ns);
    IOLog("  Last seqno: %u\n", stats.last_seqno);
    IOLog("  Hangs: %u\n", stats.hangs);
    IOLog("  Flags: 0x%x (banned=%d, closed=%d, default=%d)\n",
          flags, isBanned(), hasFlag(CONTEXT_CLOSED), isDefault());
}


// MARK: - GPU Hang Recovery


bool IntelContext::isValid() const {
    // Check if context is in a valid state for execution
    // Invalid conditions:
    // 1. Context is banned (caused GPU hang)
    // 2. Context is closed
    // 3. Context object is missing
    // 4. Controller is missing
    
    if (!controller) {
        IOLog("[IntelContext] Context %u invalid: no controller\n", contextId);
        return false;
    }
    
    if (hasFlag(CONTEXT_BANNED)) {
        IOLog("[IntelContext] Context %u invalid: banned (caused GPU hang)\n", contextId);
        return false;
    }
    
    if (hasFlag(CONTEXT_CLOSED)) {
        IOLog("[IntelContext] Context %u invalid: closed\n", contextId);
        return false;
    }
    
    if (!contextObj) {
        IOLog("[IntelContext] Context %u invalid: no context object\n", contextId);
        return false;
    }
    
    return true;
}

IOReturn IntelContext::restoreAfterReset() {
    IOLog("[IntelContext] Restoring context %u after GPU reset\n", contextId);
    
    if (!controller) {
        IOLog("[IntelContext] ERROR: No controller for context restore\n");
        return kIOReturnError;
    }
    
    // Context restoration sequence:
    // 1. Verify context object is still valid
    // 2. Restore context state from saved copy
    // 3. Rebind ring buffer if needed
    // 4. Restore PPGTT mappings
    // 5. Clear banned flag if this wasn't the hanging context
    
    // Verify context object
    if (!contextObj) {
        IOLog("[IntelContext] ERROR: Context object missing during restore\n");
        return kIOReturnError;
    }
    
    // Restore hardware state
    if (!restoreState()) {
        IOLog("[IntelContext] ERROR: Failed to restore context state\n");
        return kIOReturnError;
    }
    
    // Rebind ring if it was bound
    if (boundRing) {
        if (!bindRing(boundRing)) {
            IOLog("[IntelContext] WARNING: Failed to rebind ring after reset\n");
            // Don't fail completely - ring might be reset separately
        }
    }
    
    // Context is now ready for use again
    IOLog("[IntelContext] OK  Context %u restored successfully\n", contextId);
    
    return kIOReturnSuccess;
}
