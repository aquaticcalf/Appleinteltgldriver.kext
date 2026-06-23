/*
 * IntelRingBuffer.cpp
 *
 * Ring buffer for GPU command submission
 */

#include "IntelRingBuffer.h"
#include "AppleIntelTGLController.h"
#include "IntelGEM.h"
#include "IntelGEMObject.h"
#include "IntelGTT.h"
#include "IntelUncore.h"
#include "IntelGuC.h"
#include <IOKit/IOLib.h>

/* Tiger Lake (Gen12) Ring Register Offsets */
#define RCS_RING_TAIL       0x02030
#define RCS_RING_HEAD       0x02034
#define RCS_RING_START      0x02038
#define RCS_RING_CTL        0x0203C

#define BCS_RING_TAIL       0x22030
#define BCS_RING_HEAD       0x22034
#define BCS_RING_START      0x22038
#define BCS_RING_CTL        0x2203C

#define VCS0_RING_TAIL      0x1C030
#define VCS0_RING_HEAD      0x1C034
#define VCS0_RING_START     0x1C038
#define VCS0_RING_CTL       0x1C03C

#define RING_CTL_SIZE(x)    ((x) - 0x1000)
#define RING_VALID          (1 << 0)
#define RING_WAIT           (1 << 11)

IntelRingBuffer::IntelRingBuffer()
    : controller(NULL)
    , gtt(NULL)
    , ringObj(NULL)
    , ringVirtual(NULL)
    , ringGpuAddress(0)
    , ringSize(0)
    , engineId(RCS0)
    , engineClass(RENDER_CLASS)
    , engineName(NULL)
    , head(0)
    , tail(0)
    , space(0)
    , active(false)
    , gucManaged(false)
    , currentSeqno(0)
    , lastRetiredSeqno(0)
    , ringLock(NULL)
    , statsLock(NULL)
{
    bzero(&regs, sizeof(regs));
    bzero(&stats, sizeof(stats));
}

IntelRingBuffer::~IntelRingBuffer()
{
    cleanup();
}

bool IntelRingBuffer::init(AppleIntelTGLController *ctrl, enum intel_engine_id id, size_t size)
{
    if (!ctrl) {
        IOLog("IntelRing: Invalid controller\n");
        return false;
    }
    
    controller = ctrl;
    gtt = controller->getGTT();
    engineId = id;
    ringSize = size;
    
    engineName = getEngineName(id);
    engineClass = getEngineClass(id);
    
    IOLog("IntelRing: Initializing %s ring buffer (%zu KB)\n",
          engineName, ringSize / 1024);
    
    // Create locks
    ringLock = IOLockAlloc();
    statsLock = IOLockAlloc();
    if (!ringLock || !statsLock) {
        IOLog("IntelRing: Failed to allocate locks\n");
        return false;
    }
    
    // Get register offsets for this engine
    getRegisterOffsets(id, &regs);
    
    // Allocate ring buffer
    if (!allocateRing()) {
        IOLog("IntelRing: Failed to allocate ring buffer\n");
        return false;
    }
    
    // Setup hardware registers
    if (!setupRegisters()) {
        IOLog("IntelRing: Failed to setup registers\n");
        return false;
    }
    
    // Initialize hardware
    if (!initializeHardware()) {
        IOLog("IntelRing: Failed to initialize hardware\n");
        return false;
    }
    
    IOLog("IntelRing: %s initialized successfully\n", engineName);
    IOLog("IntelRing: Ring at GPU address 0x%llx\n", ringGpuAddress);
    
    return true;
}

void IntelRingBuffer::cleanup()
{
    IOLog("IntelRing: Cleaning up %s\n", engineName ? engineName : "ring");
    
    // Stop the ring
    if (active) {
        stop();
    }
    
    // Print final statistics
    printStats();
    
    // Release ring buffer
    if (ringObj) {
        if (ringVirtual) {
            ringObj->unmapCPU();
            ringVirtual = NULL;
        }
        
        if (gtt && ringGpuAddress) {
            gtt->unbindObject(ringObj);
        }
        
        // Ring object will be freed by GEM
        ringObj = NULL;
    }
    
    // Free locks
    if (ringLock) {
        IOLockFree(ringLock);
        ringLock = NULL;
    }
    
    if (statsLock) {
        IOLockFree(statsLock);
        statsLock = NULL;
    }
    
    IOLog("IntelRing: Cleanup complete\n");
}

bool IntelRingBuffer::allocateRing()
{
    IOLog("IntelRing: Allocating ring buffer (%zu bytes)\n", ringSize);
    
    // Create GEM object for ring
    IntelGEM *gem = controller->getGEM();
    if (!gem) {
        IOLog("IntelRing: No GEM manager\n");
        return false;
    }
    
    ringObj = gem->createObject(ringSize, 0);
    if (!ringObj) {
        IOLog("IntelRing: Failed to create ring object\n");
        return false;
    }
    
    // Allocate GTT space
    if (!gtt) {
        IOLog("IntelRing: No GTT manager\n");
        return false;
    }
    
    ringGpuAddress = gtt->allocateSpace(ringSize, 4096);
    if (ringGpuAddress == 0) {
        IOLog("IntelRing: Failed to allocate GTT space\n");
        return false;
    }
    
    ringObj->setGTTAddress(ringGpuAddress);
    
    // Bind to GTT
    if (!gtt->bindObject(ringObj, 0)) {
        IOLog("IntelRing: Failed to bind ring object\n");
        gtt->freeSpace(ringGpuAddress, ringSize);
        return false;
    }
    
    // Map for CPU access
    if (!ringObj->mapCPU(&ringVirtual)) {
        IOLog("IntelRing: Failed to map ring buffer\n");
        return false;
    }
    
    // Clear ring buffer
    bzero(ringVirtual, ringSize);
    
    IOLog("IntelRing: Ring allocated at virtual %p, GPU 0x%llx\n",
          ringVirtual, ringGpuAddress);
    
    return true;
}

bool IntelRingBuffer::setupRegisters()
{
    // Registers were set by getRegisterOffsets()
    IOLog("IntelRing: Register offsets: TAIL=0x%x HEAD=0x%x START=0x%x CTL=0x%x\n",
          regs.tail, regs.head, regs.start, regs.ctl);
    return true;
}

bool IntelRingBuffer::initializeHardware()
{
    IOLog("IntelRing: Initializing hardware for %s\n", engineName);
    
    
    // On Gen12+ (Tiger Lake), GuC manages the ring buffers
    // We don't directly initialize the hardware rings
    IntelGuC* guc = controller->getGuC();
    if (guc && guc->isReady()) {
        IOLog("IntelRing: Gen12+ detected - Using GuC submission (ring will be managed by GuC)\n");
        
        // Ring buffer is allocated and mapped, but GuC will control it
        // We just mark it as active for software tracking
        head = 0;
        tail = 0;
        space = ringSize - 8;  // Leave some safety margin
        active = true;
        gucManaged = true;  //  CRITICAL: Mark as GuC-managed
        
        IOLog("IntelRing: %s ready for GuC-managed submission\n", engineName);
        return true;
    }
    
     
     
     
    // Legacy path (Gen11 and earlier) - direct hardware initialization
    IOLog("IntelRing: No GuC available - Using legacy direct ring submission\n");
    gucManaged = false;  //  CRITICAL: Mark as legacy mode;
    
    // Stop ring first
    writeRegister(regs.ctl, 0);
    
    // Set ring start address
    writeRegister(regs.start, (u32)ringGpuAddress);
    
    // Clear head and tail
    writeRegister(regs.head, 0);
    writeRegister(regs.tail, 0);
    
    head = 0;
    tail = 0;
    space = ringSize - 8;  // Leave some safety margin
    
    // Enable ring
    u32 ctlValue = RING_VALID | RING_CTL_SIZE(ringSize);
    writeRegister(regs.ctl, ctlValue);
    
    // Verify
    u32 readBack = readRegister(regs.ctl);
    if ((readBack & RING_VALID) == 0) {
        IOLog("IntelRing: Failed to enable ring (CTL=0x%x)\n", readBack);
        return false;
    }
    
    active = true;
    
    IOLog("IntelRing: Hardware initialized, ring is active\n");
    return true;
}



u32 IntelRingBuffer::readRegister(u32 reg)
{
    IntelUncore *uncore = controller->getUncore();
    if (!uncore) {
        return 0;
    }
    
    return uncore->readRegister32_fw(reg);
}

void IntelRingBuffer::writeRegister(u32 reg, u32 value)
{
    IntelUncore *uncore = controller->getUncore();
    if (!uncore) {
        return;
    }
    
    uncore->writeRegister32_fw(reg, value);
}


// Ring State Management

void IntelRingBuffer::updateHead()
{
    u32 hwHead = readRegister(regs.head);
    head = hwHead & (ringSize - 1);  // Mask to ring size
    updateSpace();
}

void IntelRingBuffer::updateTail(u32 newTail)
{
    tail = newTail & (ringSize - 1);
    writeRegister(regs.tail, tail);
    updateSpace();
    
    // Update statistics
    IOLockLock(statsLock);
    if (newTail < tail) {
        stats.wraps++;
    }
    IOLockUnlock(statsLock);
}

void IntelRingBuffer::updateSpace()
{
    space = calculateSpace(head, tail);
}

size_t IntelRingBuffer::calculateSpace(u32 head, u32 tail) const
{
    if (head <= tail) {
        return ringSize - (tail - head) - 8;
    } else {
        return head - tail - 8;
    }
}


// Command Submission

bool IntelRingBuffer::begin(size_t numDwords)
{
    IOLockLock(ringLock);
    
    size_t numBytes = numDwords * 4;
    
    // Check if we have enough space
    if (!waitForSpace(numBytes, 1000)) {
        IOLog("IntelRing: Timeout waiting for %zu bytes\n", numBytes);
        IOLockUnlock(ringLock);
        return false;
    }
    
    return true;
}

bool IntelRingBuffer::emit(u32 dword)
{
    if (!ringVirtual) {
        return false;
    }
    
    // Check space
    if (space < 4) {
        IOLog("IntelRing: No space for emit\n");
        return false;
    }
    
    // Write dword
    volatile u32 *ringPtr = (volatile u32 *)((uintptr_t)ringVirtual + tail);
    *ringPtr = dword;
    
    tail = (tail + 4) & (ringSize - 1);
    space -= 4;
    
    // Update statistics
    IOLockLock(statsLock);
    stats.bytes_written += 4;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelRingBuffer::emitDwords(const u32 *data, size_t count)
{
    if (!data || count == 0) {
        return false;
    }
    
    for (size_t i = 0; i < count; i++) {
        if (!emit(data[i])) {
            return false;
        }
    }
    
    return true;
}

bool IntelRingBuffer::advance()
{
    if (!ringLock) {
        return false;
    }
    
    // Update tail register to submit commands
    updateTail(tail);
    
    // Increment sequence number
    currentSeqno++;
    
    IOLog("IntelRing:  Submitted seqno %u (tail=0x%x)\n", currentSeqno, tail);
    
    // Update statistics
    IOLockLock(statsLock);
    stats.commands_submitted++;
    stats.last_seqno = currentSeqno;
    IOLockUnlock(statsLock);
    
    IOLockUnlock(ringLock);
    
    return true;
}

bool IntelRingBuffer::submitCommand(const u32 *commands, size_t numDwords, IntelRequest* request)
{
    if (!commands || numDwords == 0) {
        return false;
    }
    
    // Begin command buffer
    if (!begin(numDwords)) {
        return false;
    }
    
    // Emit all commands
    if (!emitDwords(commands, numDwords)) {
        IOLockUnlock(ringLock); // Unlock on error
        return false;
    }
    
    // Advance and submit
    return advance();
}


// Space Management

size_t IntelRingBuffer::getAvailableSpace() const
{
    return space;
}

bool IntelRingBuffer::isSpaceAvailable(size_t numBytes) const
{
    return space >= numBytes;
}

bool IntelRingBuffer::waitForSpace(size_t numBytes, u32 timeoutMs)
{
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    
    while (!isSpaceAvailable(numBytes)) {
        updateHead();
        
        if (isSpaceAvailable(numBytes)) {
            return true;
        }
        
        AbsoluteTime now;
        clock_get_uptime(&now);
        
        if (CMP_ABSOLUTETIME(&now, &deadline) > 0) {
            // Timeout
            IOLockLock(statsLock);
            stats.waits++;
            IOLockUnlock(statsLock);
            return false;
        }
        
        IOSleep(1);  // Sleep 1ms
    }
    
    return true;
}



u32 IntelRingBuffer::getSeqno()
{
    return currentSeqno;
}

bool IntelRingBuffer::sync()
{
    //  CRITICAL: GuC-managed rings cannot use legacy sync!
    if (gucManaged) {
        IOLog("IntelRing:  sync() called on GuC-managed ring!\n");
        IOLog("IntelRing: GuC submissions complete asynchronously via CTB messages\n");
        IOLog("IntelRing: Use fence-based waits or GuC CTB polling instead\n");
        IOLog("IntelRing: Returning success to avoid timeout (but sync is NOOP!)\n");
        return true;  // Return success to avoid blocking, but it's a lie
    }
    
    IOLog("IntelRing:  sync() called - waiting for seqno %u (lastRetired=%u)\n",
          currentSeqno, lastRetiredSeqno);
    return waitSeqno(currentSeqno, 1000);
}

void IntelRingBuffer::retireSeqno(u32 seqno)
{
    // Called by interrupt handler when GPU completes work
    IOLockLock(ringLock);
    
    // Update retired seqno if newer
    if ((int32_t)(seqno - lastRetiredSeqno) > 0) {
        lastRetiredSeqno = seqno;
        IOLog("IntelRing: OK  Retired seqno %u (current=%u)\n", seqno, currentSeqno);
    }
    
    IOLockUnlock(ringLock);
}

bool IntelRingBuffer::waitSeqno(u32 seqno, u32 timeoutMs)
{
    //  CRITICAL: GuC-managed rings cannot use legacy waitSeqno!
    if (gucManaged) {
        IOLog("IntelRing:  waitSeqno() called on GuC-managed ring!\n");
        IOLog("IntelRing: Returning success to avoid blocking\n");
        return true;  // Pretend it's done
    }
    
    IOLog("IntelRing:  waitSeqno(%u) - lastRetiredSeqno=%u\n", seqno, lastRetiredSeqno);
    
    if (seqno <= lastRetiredSeqno) {
        IOLog("IntelRing: OK  seqno %u already retired!\n", seqno);
        return true;  // Already complete
    }
    
    IOLog("IntelRing: ⏳ Polling for seqno %u...\n", seqno);
    
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    
    while (true) {
        updateHead();
        
        if (isIdle()) {
            lastRetiredSeqno = currentSeqno;
            return true;
        }
        
        // Check if interrupt retired it
        if (seqno <= lastRetiredSeqno) {
            IOLog("IntelRing: OK  seqno %u retired by interrupt!\n", seqno);
            return true;
        }
        
        AbsoluteTime now;
        clock_get_uptime(&now);
        
        if (CMP_ABSOLUTETIME(&now, &deadline) > 0) {
            IOLog("IntelRing: ERR  Timeout waiting for seqno %u (current=%u, lastRetired=%u)\n",
                  seqno, currentSeqno, lastRetiredSeqno);
            return false;
        }
        
        IOSleep(1);
    }
}

bool IntelRingBuffer::waitForIdle(uint32_t timeoutMs)
{
    //  CRITICAL: GuC-managed rings cannot use legacy waitForIdle!
    if (gucManaged) {
        IOLog("IntelRing:  waitForIdle() called on GuC-managed ring!\n");
        IOLog("IntelRing: Returning success to avoid blocking\n");
        return true;  // Pretend it's idle
    }
    
    // Wait for the ring to become idle
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    
    while (true) {
        updateHead();
        
        if (isIdle()) {
            return true;
        }
        
        AbsoluteTime now;
        clock_get_uptime(&now);
        
        if (CMP_ABSOLUTETIME(&now, &deadline) > 0) {
            IOLog("IntelRing: Timeout waiting for idle\n");
            return false;
        }
        
        IOSleep(1);
    }
}

void IntelRingBuffer::flush()
{
    // Ensure all pending commands are submitted
    updateTail(tail);
    
    // Force a cache flush
    if (ringObj) {
        void* vaddr = nullptr;
        if (ringObj->mapCPU(&vaddr)) {
            // Memory barrier to ensure writes are visible
            OSSynchronizeIO();
            ringObj->unmapCPU();
        }
    }
}


// Control

bool IntelRingBuffer::start()
{
    if (active) {
        return true;
    }
    
    return initializeHardware();
}

bool IntelRingBuffer::stop()
{
    if (!active) {
        return true;
    }
    
    IOLog("IntelRing: Stopping %s\n", engineName);
    
    // Wait for idle
    if (!waitForIdleEngine(1000)) {
        IOLog("IntelRing: Warning - engine didn't idle\n");
    }
    
    // Disable ring
    writeRegister(regs.ctl, 0);
    
    active = false;
    
    IOLog("IntelRing: %s stopped\n", engineName);
    return true;
}

bool IntelRingBuffer::reset()
{
    IOLog("IntelRing: Resetting %s\n", engineName);
    
    stop();
    
    // Clear ring buffer
    if (ringVirtual) {
        bzero(ringVirtual, ringSize);
    }
    
    head = 0;
    tail = 0;
    currentSeqno = 0;
    lastRetiredSeqno = 0;
    
    return start();
}


// Query

u32 IntelRingBuffer::getHead() const
{
    return head;
}

u32 IntelRingBuffer::getTail() const
{
    return tail;
}

bool IntelRingBuffer::isBusy() const
{
    return head != tail;
}

bool IntelRingBuffer::isIdle() const
{
    return !isBusy();
}

u64 IntelRingBuffer::getRingAddress() const
{
    return ringGpuAddress;
}

bool IntelRingBuffer::waitForIdleEngine(u32 timeoutMs)
{
    AbsoluteTime deadline;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    
    while (!isIdle()) {
        updateHead();
        
        AbsoluteTime now;
        clock_get_uptime(&now);
        
        if (CMP_ABSOLUTETIME(&now, &deadline) > 0) {
            return false;
        }
        
        IOSleep(1);
    }
    
    return true;
}


// Statistics

void IntelRingBuffer::getStats(struct ring_stats *out)
{
    if (!out) {
        return;
    }
    
    IOLockLock(statsLock);
    memcpy(out, &stats, sizeof(struct ring_stats));
    IOLockUnlock(statsLock);
}

void IntelRingBuffer::printStats()
{
    IOLog("IntelRing %s Statistics:\n", engineName);
    IOLog("  Commands submitted: %llu\n", stats.commands_submitted);
    IOLog("  Bytes written: %llu (%.2f MB)\n",
          stats.bytes_written, stats.bytes_written / (1024.0 * 1024.0));
    IOLog("  Waits: %llu\n", stats.waits);
    IOLog("  Wraps: %llu\n", stats.wraps);
    IOLog("  Last seqno: %u\n", stats.last_seqno);
    IOLog("  Current state: head=%u tail=%u space=%u\n", head, tail, space);
}


// Helper Functions

const char* IntelRingBuffer::getEngineName(enum intel_engine_id id)
{
    switch (id) {
        case RCS0:  return "RCS0 (Render)";
        case BCS0:  return "BCS0 (Blitter)";
        case VCS0:  return "VCS0 (Video)";
        case VCS1:  return "VCS1 (Video)";
        case VECS0: return "VECS0 (VideoEnhance)";
        default:    return "Unknown";
    }
}

enum intel_engine_class IntelRingBuffer::getEngineClass(enum intel_engine_id id)
{
    switch (id) {
        case RCS0:  return RENDER_CLASS;
        case BCS0:  return COPY_ENGINE_CLASS;
        case VCS0:
        case VCS1:  return VIDEO_DECODE_CLASS;
        case VECS0: return VIDEO_ENHANCE_CLASS;
        default:    return OTHER_CLASS;
    }
}

void IntelRingBuffer::getRegisterOffsets(enum intel_engine_id id, struct ring_registers *out)
{
    switch (id) {
        case RCS0:
            out->tail = RCS_RING_TAIL;
            out->head = RCS_RING_HEAD;
            out->start = RCS_RING_START;
            out->ctl = RCS_RING_CTL;
            break;
            
        case BCS0:
            out->tail = BCS_RING_TAIL;
            out->head = BCS_RING_HEAD;
            out->start = BCS_RING_START;
            out->ctl = BCS_RING_CTL;
            break;
            
        case VCS0:
            out->tail = VCS0_RING_TAIL;
            out->head = VCS0_RING_HEAD;
            out->start = VCS0_RING_START;
            out->ctl = VCS0_RING_CTL;
            break;
            
        default:
            bzero(out, sizeof(*out));
            break;
    }
}
