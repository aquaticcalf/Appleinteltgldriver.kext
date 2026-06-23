/*
 * IntelGTInterrupts.cpp
 * Intel GT Interrupt Handling Implementation
 */

#include "IntelGTInterrupts.h"
#include "AppleIntelTGLController.h"
#include "IntelRingBuffer.h"
#include "AppleIntelTGLIOAccelerator.h"
#include <IOKit/IOLib.h>

#define super OSObject
OSDefineMetaClassAndStructors(IntelGTInterrupts, OSObject)

// Engine names for logging
static const char* engineNames[] = {
    "RCS", "BCS", "VCS0", "VCS1", "VECS"
};

/* Initialization */
bool IntelGTInterrupts::init() {
    if (!super::init()) {
        return false;
    }
    
    controller = nullptr;
    workLoop = nullptr;
    interruptSource = nullptr;
    watchdogTimer = nullptr;
    
    // Initialize handler lists
    for (int i = 0; i < GT_ENGINE_COUNT; i++) {
        renderCompleteHandlers[i] = nullptr;
        userInterruptHandlers[i] = nullptr;
        gpuHangHandlers[i] = nullptr;
        
        currentSeqno[i] = 0;
        lastSeqno[i] = 0;
        lastSeqnoTime[i] = 0;
        seqnoPending[i] = false;
        lastActivityTime[i] = 0;
        
        memset(&hangState[i], 0, sizeof(GPUHangState));
    }
    
    pageFaultHandlers = nullptr;
    
    enabledInterrupts = 0;
    engineEnabled = 0;
    
    // Clear statistics
    memset(&stats, 0, sizeof(stats));
    handlerStartTime = 0;
    
    watchdogInterval = WATCHDOG_CHECK_INTERVAL_MS;
    watchdogRunning = false;
    
    // Create locks
    interruptLock = IOLockAlloc();
    renderLock = IOLockAlloc();
    userLock = IOLockAlloc();
    hangLock = IOLockAlloc();
    faultLock = IOLockAlloc();
    
    if (!interruptLock || !renderLock || !userLock || !hangLock || !faultLock) {
        return false;
    }
    
    isStarted = false;
    interruptsRegistered = false;
    
    return true;
}

void IntelGTInterrupts::free() {
    if (isStarted) {
        stop();
    }
    
    // Free locks
    if (interruptLock) IOLockFree(interruptLock);
    if (renderLock) IOLockFree(renderLock);
    if (userLock) IOLockFree(userLock);
    if (hangLock) IOLockFree(hangLock);
    if (faultLock) IOLockFree(faultLock);
    
    super::free();
}

bool IntelGTInterrupts::initWithController(AppleIntelTGLController* ctrl) {
    if (!ctrl) return false;
    
    controller = ctrl;
    workLoop = controller->getWorkLoop();
    
    if (!workLoop) return false;
    
    return true;
}

bool IntelGTInterrupts::start() {
    if (isStarted) return true;
    
    IOLog("IntelGTInterrupts::start()\n");
    
    //  Don't register a separate interrupt handler!
    // GT interrupts are handled by the display interrupt handler
    // which calls our handleInterrupt() method on shared interrupt line
    IOLog("OK  GT interrupts will be handled via shared display interrupt (index 0)\n");
    
    // Clear pending interrupts
    uint32_t status = readGTInterruptStatus();
    clearGTInterruptStatus(status);
    
    // Disable all interrupts initially
    writeGTInterruptMask(0xFFFFFFFF);
    writeGTInterruptEnable(0);
    
    isStarted = true;
    interruptsRegistered = true;  // Mark as registered (via display handler)
    IOLog("OK  GT interrupts module started - using shared interrupt dispatch\n");
    
    return true;
}

void IntelGTInterrupts::stop() {
    if (!isStarted) return;
    
    IOLog("IntelGTInterrupts::stop()\n");
    
    // Stop watchdog
    if (watchdogRunning) {
        stopWatchdog();
    }
    
    // Disable all interrupts
    disableInterrupts(GT_INT_ALL);
    
    // Unregister interrupt handler
    unregisterInterruptHandler();
    
    // Free all handler lists
    IOLockLock(renderLock);
    for (int i = 0; i < GT_ENGINE_COUNT; i++) {
        RenderCompleteHandler* handler = renderCompleteHandlers[i];
        while (handler) {
            RenderCompleteHandler* next = handler->next;
            IOFree(handler, sizeof(RenderCompleteHandler));
            handler = next;
        }
        renderCompleteHandlers[i] = nullptr;
    }
    IOLockUnlock(renderLock);
    
    IOLockLock(userLock);
    for (int i = 0; i < GT_ENGINE_COUNT; i++) {
        UserInterruptHandler* handler = userInterruptHandlers[i];
        while (handler) {
            UserInterruptHandler* next = handler->next;
            IOFree(handler, sizeof(UserInterruptHandler));
            handler = next;
        }
        userInterruptHandlers[i] = nullptr;
    }
    IOLockUnlock(userLock);
    
    IOLockLock(hangLock);
    for (int i = 0; i < GT_ENGINE_COUNT; i++) {
        GPUHangHandler* handler = gpuHangHandlers[i];
        while (handler) {
            GPUHangHandler* next = handler->next;
            IOFree(handler, sizeof(GPUHangHandler));
            handler = next;
        }
        gpuHangHandlers[i] = nullptr;
    }
    IOLockUnlock(hangLock);
    
    IOLockLock(faultLock);
    PageFaultHandler* faultHandler = pageFaultHandlers;
    while (faultHandler) {
        PageFaultHandler* next = faultHandler->next;
        IOFree(faultHandler, sizeof(PageFaultHandler));
        faultHandler = next;
    }
    pageFaultHandlers = nullptr;
    IOLockUnlock(faultLock);
    
    isStarted = false;
    IOLog("GT interrupts stopped\n");
}

/* Interrupt registration */
bool IntelGTInterrupts::registerInterruptHandler() {
    if (interruptsRegistered) return true;
    
    //  CRITICAL: This function should NOT be called!
    // GT interrupts are dispatched by the display interrupt handler
    // If you see this message, the GT start() function is calling registerInterruptHandler() incorrectly
    IOLog(" WARNING: GT registerInterruptHandler() called - should use display dispatch instead!\n");
    
    interruptSource = IOInterruptEventSource::interruptEventSource(
        this,
        (IOInterruptEventAction)&IntelGTInterrupts::interruptOccurred,
        controller->getProvider(),
        0  // OK  Use index 0 (shared with display)
    );
    
    if (!interruptSource) {
        IOLog("ERR  Failed to create GT interrupt event source\n");
        return false;
    }
    
    if (workLoop->addEventSource(interruptSource) != kIOReturnSuccess) {
        IOLog("ERR  Failed to add GT interrupt source to work loop\n");
        interruptSource->release();
        interruptSource = nullptr;
        return false;
    }
    
    interruptSource->enable();
    interruptsRegistered = true;
    IOLog("OK  GT interrupt handler registered (shared interrupt 0)\n");
    
    return true;
}

void IntelGTInterrupts::unregisterInterruptHandler() {
    if (!interruptsRegistered || !interruptSource) return;
    
    interruptSource->disable();
    workLoop->removeEventSource(interruptSource);
    interruptSource->release();
    interruptSource = nullptr;
    
    interruptsRegistered = false;
    IOLog("GT interrupt handler unregistered\n");
}

/* Enable/disable interrupts */
bool IntelGTInterrupts::enableInterrupts(uint32_t types) {
    IOLockLock(interruptLock);
    
    enabledInterrupts |= types;
    
    // Enable master GT interrupt
    uint32_t masterCtl = controller->readRegister32(GEN11_GT_INT_CTL);
    masterCtl |= (1 << 31);
    controller->writeRegister32(GEN11_GT_INT_CTL, masterCtl);
    
    // Enable per-engine interrupts
    for (int engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        if (engineEnabled & (1 << engine)) {
            uint32_t engineIER = 0;
            
            if (types & GT_INT_RENDER_COMPLETE) {
                engineIER |= GT_RENDER_COMPLETE_INT;
            }
            if (types & GT_INT_USER) {
                engineIER |= GT_USER_INTERRUPT;
            }
            if (types & GT_INT_CONTEXT_SWITCH) {
                engineIER |= GT_CONTEXT_SWITCH_INT;
            }
            if (types & GT_INT_ERROR) {
                engineIER |= GT_ERROR_INT;
            }
            
            controller->writeRegister32(GEN11_GT_ENGINE_IER(engine), engineIER);
        }
    }
    
    IOLockUnlock(interruptLock);
    
    IOLog("Enabled GT interrupts: 0x%x\n", types);
    return true;
}

void IntelGTInterrupts::disableInterrupts(uint32_t types) {
    IOLockLock(interruptLock);
    
    enabledInterrupts &= ~types;
    
    for (int engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        uint32_t engineIER = controller->readRegister32(GEN11_GT_ENGINE_IER(engine));
        
        if (types & GT_INT_RENDER_COMPLETE) engineIER &= ~GT_RENDER_COMPLETE_INT;
        if (types & GT_INT_USER) engineIER &= ~GT_USER_INTERRUPT;
        if (types & GT_INT_CONTEXT_SWITCH) engineIER &= ~GT_CONTEXT_SWITCH_INT;
        if (types & GT_INT_ERROR) engineIER &= ~GT_ERROR_INT;
        
        controller->writeRegister32(GEN11_GT_ENGINE_IER(engine), engineIER);
    }
    
    if (enabledInterrupts == 0) {
        uint32_t masterCtl = controller->readRegister32(GEN11_GT_INT_CTL);
        masterCtl &= ~(1 << 31);
        controller->writeRegister32(GEN11_GT_INT_CTL, masterCtl);
    }
    
    IOLockUnlock(interruptLock);
    
    IOLog("Disabled GT interrupts: 0x%x\n", types);
}

bool IntelGTInterrupts::isInterruptEnabled(uint32_t type) {
    IOLockLock(interruptLock);
    bool enabled = (enabledInterrupts & type) != 0;
    IOLockUnlock(interruptLock);
    return enabled;
}

bool IntelGTInterrupts::enableEngineInterrupts(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return false;
    
    IOLockLock(interruptLock);
    engineEnabled |= (1 << engine);
    IOLockUnlock(interruptLock);
    
    IOLog("GT engine %s interrupts enabled\n", getEngineName(engine));
    return true;
}

void IntelGTInterrupts::disableEngineInterrupts(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return;
    
    IOLockLock(interruptLock);
    engineEnabled &= ~(1 << engine);
    
    uint32_t engineIER = controller->readRegister32(GEN11_GT_ENGINE_IER(engine));
    engineIER = 0;
    controller->writeRegister32(GEN11_GT_ENGINE_IER(engine), engineIER);
    
    IOLockUnlock(interruptLock);
    
    IOLog("GT engine %s interrupts disabled\n", getEngineName(engine));
}

/* Render complete */
bool IntelGTInterrupts::registerRenderCompleteHandler(uint32_t engine, uint32_t seqno,
                                                     RenderCompleteCallback callback, void* context) {
    if (engine >= GT_ENGINE_COUNT || !callback) return false;
    
    RenderCompleteHandler* handler = (RenderCompleteHandler*)IOMalloc(sizeof(RenderCompleteHandler));
    if (!handler) return false;
    
    handler->callback = callback;
    handler->context = context;
    handler->engine = engine;
    handler->waitSeqno = seqno;
    handler->enabled = true;
    
    IOLockLock(renderLock);
    handler->next = renderCompleteHandlers[engine];
    renderCompleteHandlers[engine] = handler;
    IOLockUnlock(renderLock);
    
    return true;
}

void IntelGTInterrupts::unregisterRenderCompleteHandler(uint32_t engine, RenderCompleteCallback callback) {
    if (engine >= GT_ENGINE_COUNT || !callback) return;
    
    IOLockLock(renderLock);
    
    RenderCompleteHandler** prev = &renderCompleteHandlers[engine];
    RenderCompleteHandler* handler = renderCompleteHandlers[engine];
    
    while (handler) {
        if (handler->callback == callback) {
            *prev = handler->next;
            IOFree(handler, sizeof(RenderCompleteHandler));
            break;
        }
        prev = &handler->next;
        handler = handler->next;
    }
    
    IOLockUnlock(renderLock);
}

bool IntelGTInterrupts::waitForRenderComplete(uint32_t engine, uint32_t seqno, uint32_t timeout_ms) {
    if (engine >= GT_ENGINE_COUNT) return false;
    
    // Check if already complete
    if (seqnoComplete(engine, seqno)) {
        return true;
    }
    
    uint64_t startTime = mach_absolute_time();
    {
        // Use nanoseconds directly
        // tbase assumed 1:1
        uint64_t timeoutNs = timeout_ms * 1000000ULL;
        uint64_t timeoutAbs = (timeoutNs * 1) / 1;
    
        while (!seqnoComplete(engine, seqno)) {
            uint64_t now = mach_absolute_time();
            if ((now - startTime) >= timeoutAbs) {
                IOLog("GT: Render wait timeout on %s (seqno %u)\n", getEngineName(engine), seqno);
                return false;
            }
            IOSleep(1);
        }
    }
    
    return true;
}

uint32_t IntelGTInterrupts::getCurrentSeqno(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return 0;
    
    IOLockLock(interruptLock);
    uint32_t seqno = currentSeqno[engine];
    IOLockUnlock(interruptLock);
    
    return seqno;
}

/* User interrupts */
bool IntelGTInterrupts::registerUserInterruptHandler(uint32_t engine, UserInterruptCallback callback, void* context) {
    if (engine >= GT_ENGINE_COUNT || !callback) return false;
    
    UserInterruptHandler* handler = (UserInterruptHandler*)IOMalloc(sizeof(UserInterruptHandler));
    if (!handler) return false;
    
    handler->callback = callback;
    handler->context = context;
    handler->engine = engine;
    handler->enabled = true;
    
    IOLockLock(userLock);
    handler->next = userInterruptHandlers[engine];
    userInterruptHandlers[engine] = handler;
    IOLockUnlock(userLock);
    
    return true;
}

void IntelGTInterrupts::unregisterUserInterruptHandler(uint32_t engine, UserInterruptCallback callback) {
    if (engine >= GT_ENGINE_COUNT || !callback) return;
    
    IOLockLock(userLock);
    
    UserInterruptHandler** prev = &userInterruptHandlers[engine];
    UserInterruptHandler* handler = userInterruptHandlers[engine];
    
    while (handler) {
        if (handler->callback == callback) {
            *prev = handler->next;
            IOFree(handler, sizeof(UserInterruptHandler));
            break;
        }
        prev = &handler->next;
        handler = handler->next;
    }
    
    IOLockUnlock(userLock);
}

void IntelGTInterrupts::triggerUserInterrupt(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return;
    
    // Write to engine's user interrupt register
    uint32_t ringCtl = controller->readRegister32(GEN11_GT_ENGINE_RING_CTL(engine));
    ringCtl |= (1 << 0);  // User interrupt bit
    controller->writeRegister32(GEN11_GT_ENGINE_RING_CTL(engine), ringCtl);
}

/* GPU hang detection */
bool IntelGTInterrupts::registerGPUHangHandler(uint32_t engine, GPUHangCallback callback, void* context) {
    if (engine >= GT_ENGINE_COUNT || !callback) return false;
    
    GPUHangHandler* handler = (GPUHangHandler*)IOMalloc(sizeof(GPUHangHandler));
    if (!handler) return false;
    
    handler->callback = callback;
    handler->context = context;
    handler->engine = engine;
    handler->enabled = true;
    
    IOLockLock(hangLock);
    handler->next = gpuHangHandlers[engine];
    gpuHangHandlers[engine] = handler;
    IOLockUnlock(hangLock);
    
    return true;
}

void IntelGTInterrupts::unregisterGPUHangHandler(uint32_t engine, GPUHangCallback callback) {
    if (engine >= GT_ENGINE_COUNT || !callback) return;
    
    IOLockLock(hangLock);
    
    GPUHangHandler** prev = &gpuHangHandlers[engine];
    GPUHangHandler* handler = gpuHangHandlers[engine];
    
    while (handler) {
        if (handler->callback == callback) {
            *prev = handler->next;
            IOFree(handler, sizeof(GPUHangHandler));
            break;
        }
        prev = &handler->next;
        handler = handler->next;
    }
    
    IOLockUnlock(hangLock);
}

bool IntelGTInterrupts::isGPUHung(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return false;
    
    IOLockLock(hangLock);
    bool hung = hangState[engine].isHung;
    IOLockUnlock(hangLock);
    
    return hung;
}

void IntelGTInterrupts::clearGPUHang(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return;
    
    IOLockLock(hangLock);
    memset(&hangState[engine], 0, sizeof(GPUHangState));
    IOLockUnlock(hangLock);
    
    IOLog("GT: Cleared hang state for %s\n", getEngineName(engine));
}

void IntelGTInterrupts::getHangState(uint32_t engine, GPUHangState* state) {
    if (engine >= GT_ENGINE_COUNT || !state) return;
    
    IOLockLock(hangLock);
    memcpy(state, &hangState[engine], sizeof(GPUHangState));
    IOLockUnlock(hangLock);
}

/* Page faults */
bool IntelGTInterrupts::registerPageFaultHandler(PageFaultCallback callback, void* context) {
    if (!callback) return false;
    
    PageFaultHandler* handler = (PageFaultHandler*)IOMalloc(sizeof(PageFaultHandler));
    if (!handler) return false;
    
    handler->callback = callback;
    handler->context = context;
    handler->enabled = true;
    
    IOLockLock(faultLock);
    handler->next = pageFaultHandlers;
    pageFaultHandlers = handler;
    IOLockUnlock(faultLock);
    
    return true;
}

void IntelGTInterrupts::unregisterPageFaultHandler(PageFaultCallback callback) {
    if (!callback) return;
    
    IOLockLock(faultLock);
    
    PageFaultHandler** prev = &pageFaultHandlers;
    PageFaultHandler* handler = pageFaultHandlers;
    
    while (handler) {
        if (handler->callback == callback) {
            *prev = handler->next;
            IOFree(handler, sizeof(PageFaultHandler));
            break;
        }
        prev = &handler->next;
        handler = handler->next;
    }
    
    IOLockUnlock(faultLock);
}

/* Watchdog */
void IntelGTInterrupts::startWatchdog(uint32_t interval_ms) {
    if (watchdogRunning) return;
    
    watchdogInterval = interval_ms;
    
    watchdogTimer = IOTimerEventSource::timerEventSource(
        this,
        (IOTimerEventSource::Action)&IntelGTInterrupts::watchdogTimerFired
    );
    
    if (!watchdogTimer) {
        IOLog("GT: Failed to create watchdog timer\n");
        return;
    }
    
    workLoop->addEventSource(watchdogTimer);
    watchdogTimer->setTimeoutMS(watchdogInterval);
    
    watchdogRunning = true;
    IOLog("GT: Watchdog started (%u ms interval)\n", interval_ms);
}

void IntelGTInterrupts::stopWatchdog() {
    if (!watchdogRunning || !watchdogTimer) return;
    
    watchdogTimer->cancelTimeout();
    workLoop->removeEventSource(watchdogTimer);
    watchdogTimer->release();
    watchdogTimer = nullptr;
    
    watchdogRunning = false;
    IOLog("GT: Watchdog stopped\n");
}

void IntelGTInterrupts::resetWatchdog() {
    if (!watchdogRunning || !watchdogTimer) return;
    
    watchdogTimer->cancelTimeout();
    watchdogTimer->setTimeoutMS(watchdogInterval);
}

/* Hardware interrupt handling */
void IntelGTInterrupts::interruptOccurred(OSObject* owner, IOInterruptEventSource* sender, int count) {
    IntelGTInterrupts* self = OSDynamicCast(IntelGTInterrupts, owner);
    if (self) {
        self->handleInterrupt();
    }
}

void IntelGTInterrupts::handleInterrupt() {
    handlerStartTime = mach_absolute_time();
    
    uint32_t masterStatus = controller->readRegister32(GEN11_GT_INT_CTL);
    if (!(masterStatus & (1 << 31))) {
        stats.spuriousInterrupts++;
        return;
    }
    
    stats.totalInterrupts++;
    
    // Handle per-engine interrupts
    for (uint32_t engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        if (!(engineEnabled & (1 << engine))) continue;
        
        uint32_t engineIIR = controller->readRegister32(GEN11_GT_ENGINE_IIR(engine));
        if (engineIIR == 0) continue;
        
        if (engineIIR & GT_RENDER_COMPLETE_INT) {
            handleRenderComplete(engine);
        }
        
        if (engineIIR & GT_USER_INTERRUPT) {
            handleUserInterrupt(engine);
        }
        
        if (engineIIR & GT_CONTEXT_SWITCH_INT) {
            handleContextSwitch(engine);
        }
        
        if (engineIIR & GT_ERROR_INT) {
            handleGPUError(engine);
        }
        
        // Clear interrupt
        controller->writeRegister32(GEN11_GT_ENGINE_IIR(engine), engineIIR);
    }
    
    // Update handler time
    {
        uint64_t handlerEnd = mach_absolute_time();
        uint64_t elapsed = handlerEnd - handlerStartTime;
        // Use nanoseconds directly
        // tbase assumed 1:1
        uint64_t elapsedUs = (elapsed * 1) / (1 * 1000);
        stats.handlerTime += elapsedUs;
    }
}

/* Specific handlers */
void IntelGTInterrupts::handleRenderComplete(uint32_t engine) {
    uint32_t seqno = readEngineSeqno(engine);
    
    IOLog("IntelGTInterrupts:  handleRenderComplete(engine=%u) seqno=%u\n", engine, seqno);
    
    updateSeqno(engine, seqno);
    
    uint64_t now = mach_absolute_time();
    uint64_t lastTime = lastSeqnoTime[engine];
    lastSeqnoTime[engine] = now;
    lastActivityTime[engine] = now;
    
    uint64_t latency = 0;
    if (lastTime > 0) {
        uint64_t elapsed = now - lastTime;
        // Use nanoseconds directly
        // tbase assumed 1:1
        latency = (elapsed * 1) / (1 * 1000);
    }
    
    updateRenderCompleteStats(engine, latency);
    invokeRenderCompleteHandlers(engine, seqno);
    
    //  CRITICAL: Update render ring's retired seqno
    // This allows sync() to complete immediately instead of timing out
    IntelRingBuffer* renderRing = controller->getRenderRing();
    if (renderRing && engine == 0) {  // Engine 0 = RCS (render ring)
        renderRing->retireSeqno(seqno);
        IOLog("IntelGTInterrupts:  Retired seqno %u on render ring\n", seqno);
    }
    
    //  CRITICAL: Notify IOAccelerator that command completed
    // This allows WindowServer to be woken up
    IntelIOAccelerator* accelerator = controller->getAccelerator();
    if (accelerator) {
        accelerator->handleCommandCompletion(engine, seqno);
    }
}

void IntelGTInterrupts::handleUserInterrupt(uint32_t engine) {
    stats.userInterrupt[engine]++;
    invokeUserInterruptHandlers(engine);
}

void IntelGTInterrupts::handleContextSwitch(uint32_t engine) {
    stats.contextSwitch[engine]++;
}

void IntelGTInterrupts::handlePageFault(uint64_t faultAddr, uint32_t faultType) {
    stats.pageFault++;
    invokePageFaultHandlers(faultAddr, faultType);
    IOLog("GT: Page fault at 0x%llx (type %u)\n", faultAddr, faultType);
}

void IntelGTInterrupts::handleGPUError(uint32_t engine) {
    stats.errorInterrupt[engine]++;
    detectGPUHang(engine);
    IOLog("GT: Error interrupt on %s\n", getEngineName(engine));
}

void IntelGTInterrupts::handleWatchdogTimeout(uint32_t engine) {
    stats.watchdogTimeout[engine]++;
    IOLog("GT: Watchdog timeout on %s\n", getEngineName(engine));
}

void IntelGTInterrupts::handleTLBInvalidate() {
    stats.tlbInvalidate++;
}

/* Watchdog timer */
void IntelGTInterrupts::watchdogTimerFired(OSObject* owner, IOTimerEventSource* sender) {
    IntelGTInterrupts* self = OSDynamicCast(IntelGTInterrupts, owner);
    if (self) {
        self->checkForHangs();
        if (self->watchdogRunning) {
            sender->setTimeoutMS(self->watchdogInterval);
        }
    }
}

void IntelGTInterrupts::checkForHangs() {
    uint64_t now = mach_absolute_time();
    // Use nanoseconds directly
    // tbase assumed 1:1
    
    for (uint32_t engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        if (!(engineEnabled & (1 << engine))) continue;
        
        uint64_t lastActivity = lastActivityTime[engine];
        if (lastActivity == 0) continue;
        
        uint64_t elapsed = now - lastActivity;
        uint64_t elapsedMs = (elapsed * 1) / (1 * 1000000);
        
        if (elapsedMs > GPU_HANG_THRESHOLD_MS) {
            detectGPUHang(engine);
        }
    }
}

void IntelGTInterrupts::detectGPUHang(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return;
    
    IOLockLock(hangLock);
    
    if (hangState[engine].isHung) {
        IOLockUnlock(hangLock);
        return;  // Already detected
    }
    
    uint32_t acthd = readEngineACTHD(engine);
    uint32_t seqno = currentSeqno[engine];
    
    hangState[engine].isHung = true;
    hangState[engine].engine = engine;
    hangState[engine].acthd = acthd;
    hangState[engine].lastSeqno = seqno;
    hangState[engine].hangTime = mach_absolute_time();
    hangState[engine].hangCount++;
    
    stats.gpuHang[engine]++;
    stats.hangCount++;
    
    IOLockUnlock(hangLock);
    
    updateHangStats(engine);
    invokeGPUHangHandlers(engine, acthd);
    
    IOLog("GT: GPU HANG detected on %s! ACTHD=0x%x SEQNO=%u\n",
          getEngineName(engine), acthd, seqno);
}

/* Hardware register access */
void IntelGTInterrupts::writeGTInterruptMask(uint32_t mask) {
    for (uint32_t engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        controller->writeRegister32(GEN11_GT_ENGINE_IMR(engine), mask);
    }
}

void IntelGTInterrupts::writeGTInterruptEnable(uint32_t enable) {
    for (uint32_t engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        controller->writeRegister32(GEN11_GT_ENGINE_IER(engine), enable);
    }
}

uint32_t IntelGTInterrupts::readGTInterruptStatus() {
    uint32_t status = 0;
    for (uint32_t engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        status |= controller->readRegister32(GEN11_GT_ENGINE_ISR(engine));
    }
    return status;
}

void IntelGTInterrupts::clearGTInterruptStatus(uint32_t status) {
    for (uint32_t engine = 0; engine < GT_ENGINE_COUNT; engine++) {
        controller->writeRegister32(GEN11_GT_ENGINE_IIR(engine), 0xFFFFFFFF);
    }
}

uint32_t IntelGTInterrupts::readEngineSeqno(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return 0;
    return controller->readRegister32(GEN11_GT_ENGINE_SEQNO(engine));
}

uint32_t IntelGTInterrupts::readEngineACTHD(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return 0;
    return controller->readRegister32(GEN11_GT_ENGINE_ACTHD(engine));
}

uint32_t IntelGTInterrupts::readEngineStatus(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return 0;
    return controller->readRegister32(GEN11_GT_ENGINE_RING_CTL(engine));
}

void IntelGTInterrupts::writeEngineInterruptMask(uint32_t engine, uint32_t mask) {
    if (engine >= GT_ENGINE_COUNT) return;
    controller->writeRegister32(GEN11_GT_ENGINE_IMR(engine), mask);
}

void IntelGTInterrupts::writeEngineInterruptEnable(uint32_t engine, uint32_t enable) {
    if (engine >= GT_ENGINE_COUNT) return;
    controller->writeRegister32(GEN11_GT_ENGINE_IER(engine), enable);
}

uint64_t IntelGTInterrupts::readPageFaultAddress() {
    uint32_t data0 = controller->readRegister32(GEN11_GT_FAULT_TLB_DATA0);
    uint32_t data1 = controller->readRegister32(GEN11_GT_FAULT_TLB_DATA1);
    return ((uint64_t)data1 << 32) | data0;
}

uint32_t IntelGTInterrupts::readPageFaultType() {
    uint32_t data1 = controller->readRegister32(GEN11_GT_FAULT_TLB_DATA1);
    return (data1 >> 16) & 0xFF;
}

void IntelGTInterrupts::clearPageFault() {
    controller->writeRegister32(GEN11_GT_FAULT_TLB_DATA0, 0);
    controller->writeRegister32(GEN11_GT_FAULT_TLB_DATA1, 0);
}

/* Seqno tracking */
void IntelGTInterrupts::updateSeqno(uint32_t engine, uint32_t seqno) {
    if (engine >= GT_ENGINE_COUNT) return;
    
    IOLockLock(interruptLock);
    currentSeqno[engine] = seqno;
    stats.lastSeqno[engine] = seqno;
    IOLockUnlock(interruptLock);
}

bool IntelGTInterrupts::seqnoComplete(uint32_t engine, uint32_t seqno) {
    if (engine >= GT_ENGINE_COUNT) return false;
    
    IOLockLock(interruptLock);
    bool complete = ((int32_t)(currentSeqno[engine] - seqno)) >= 0;
    IOLockUnlock(interruptLock);
    
    return complete;
}

/* Helpers */
void IntelGTInterrupts::invokeRenderCompleteHandlers(uint32_t engine, uint32_t seqno) {
    IOLockLock(renderLock);
    
    RenderCompleteHandler** prev = &renderCompleteHandlers[engine];
    RenderCompleteHandler* handler = renderCompleteHandlers[engine];
    
    while (handler) {
        RenderCompleteHandler* next = handler->next;
        
        if (handler->enabled && ((int32_t)(seqno - handler->waitSeqno)) >= 0) {
            if (handler->callback) {
                handler->callback(handler->context, engine, seqno);
            }
            
            // Remove one-shot handler
            *prev = next;
            IOFree(handler, sizeof(RenderCompleteHandler));
        } else {
            prev = &handler->next;
        }
        
        handler = next;
    }
    
    IOLockUnlock(renderLock);
}

void IntelGTInterrupts::invokeUserInterruptHandlers(uint32_t engine) {
    IOLockLock(userLock);
    
    UserInterruptHandler* handler = userInterruptHandlers[engine];
    while (handler) {
        if (handler->enabled && handler->callback) {
            handler->callback(handler->context, engine);
        }
        handler = handler->next;
    }
    
    IOLockUnlock(userLock);
}

void IntelGTInterrupts::invokeGPUHangHandlers(uint32_t engine, uint32_t acthd) {
    IOLockLock(hangLock);
    
    GPUHangHandler* handler = gpuHangHandlers[engine];
    while (handler) {
        if (handler->enabled && handler->callback) {
            handler->callback(handler->context, engine, acthd);
        }
        handler = handler->next;
    }
    
    IOLockUnlock(hangLock);
}

void IntelGTInterrupts::invokePageFaultHandlers(uint64_t faultAddr, uint32_t faultType) {
    IOLockLock(faultLock);
    
    PageFaultHandler* handler = pageFaultHandlers;
    while (handler) {
        if (handler->enabled && handler->callback) {
            handler->callback(handler->context, faultAddr, faultType);
        }
        handler = handler->next;
    }
    
    IOLockUnlock(faultLock);
}

void IntelGTInterrupts::updateRenderCompleteStats(uint32_t engine, uint64_t latency) {
    stats.renderComplete[engine]++;
    
    if (stats.renderComplete[engine] > 1) {
        stats.renderLatency[engine] = (stats.renderLatency[engine] * 9 + latency) / 10;
    } else {
        stats.renderLatency[engine] = latency;
    }
}

void IntelGTInterrupts::updateContextSwitchStats(uint32_t engine, uint64_t latency) {
    if (stats.contextSwitch[engine] > 1) {
        stats.contextSwitchLatency[engine] = (stats.contextSwitchLatency[engine] * 9 + latency) / 10;
    } else {
        stats.contextSwitchLatency[engine] = latency;
    }
}

void IntelGTInterrupts::updateHangStats(uint32_t engine) {
    // Stats already updated in detectGPUHang
}

const char* IntelGTInterrupts::getEngineName(uint32_t engine) {
    if (engine >= GT_ENGINE_COUNT) return "UNKNOWN";
    return engineNames[engine];
}

/* Statistics */
void IntelGTInterrupts::getStats(GTInterruptStats* outStats) {
    if (!outStats) return;
    
    IOLockLock(interruptLock);
    memcpy(outStats, &stats, sizeof(GTInterruptStats));
    IOLockUnlock(interruptLock);
}

void IntelGTInterrupts::resetStats() {
    IOLockLock(interruptLock);
    memset(&stats, 0, sizeof(GTInterruptStats));
    IOLockUnlock(interruptLock);
    
    IOLog("GT interrupt statistics reset\n");
}

void IntelGTInterrupts::printStats() {
    IOLog("Total interrupts: %llu\n", stats.totalInterrupts);
    IOLog("Spurious: %llu\n", stats.spuriousInterrupts);
    IOLog("Handler time: %llu us\n", stats.handlerTime);
    
    IOLog("\nRender Complete:\n");
    for (uint32_t i = 0; i < GT_ENGINE_COUNT; i++) {
        if (stats.renderComplete[i] > 0) {
            IOLog("  %s: %llu (%llu us avg latency)\n",
                  getEngineName(i), stats.renderComplete[i], stats.renderLatency[i]);
        }
    }
    
    IOLog("\nUser Interrupts:\n");
    for (uint32_t i = 0; i < GT_ENGINE_COUNT; i++) {
        if (stats.userInterrupt[i] > 0) {
            IOLog("  %s: %llu\n", getEngineName(i), stats.userInterrupt[i]);
        }
    }
    
    IOLog("\nContext Switches:\n");
    for (uint32_t i = 0; i < GT_ENGINE_COUNT; i++) {
        if (stats.contextSwitch[i] > 0) {
            IOLog("  %s: %llu (%llu us avg)\n",
                  getEngineName(i), stats.contextSwitch[i], stats.contextSwitchLatency[i]);
        }
    }
    
    IOLog("\nErrors:\n");
    IOLog("  Page faults: %llu\n", stats.pageFault);
    IOLog("  Total GPU hangs: %u\n", stats.hangCount);
    for (uint32_t i = 0; i < GT_ENGINE_COUNT; i++) {
        if (stats.gpuHang[i] > 0) {
            IOLog("  %s hangs: %llu\n", getEngineName(i), stats.gpuHang[i]);
        }
    }
    
}
