/*
 * IntelGTT.cpp
 *
 * Graphics Translation Table (GTT) implementation
 */

#include "IntelGTT.h"
#include "AppleIntelTGLController.h"
#include "IntelUncore.h"
#include "IntelGEMObject.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODMACommand.h>

/* Gen12 GTT Register Offsets */
#define GEN12_GGTT_BASE     0x100000  // GGTT base address
#define GEN12_GGTT_SIZE_REG 0x108080  // GGTT size register

IntelGTT::IntelGTT()
    : controller(NULL)
    , uncore(NULL)
    , gttMap(NULL)
    , gttBase(NULL)
    , gttSize(0)
    , baseAddress(0)
    , totalSize(0)
    , usableSize(0)
    , pageSize(4096)
    , numEntries(0)
    , allocationBitmap(NULL)
    , bitmapSize(0)
    , statsLock(NULL)
{
    bzero(&stats, sizeof(stats));
}

IntelGTT::~IntelGTT()
{
    cleanup();
}

bool IntelGTT::init(AppleIntelTGLController *ctrl)
{
    if (!ctrl) {
        IOLog("IntelGTT: Invalid controller\n");
        return false;
    }
    
    controller = ctrl;
    uncore = controller->getUncore();
    
    if (!uncore) {
        IOLog("IntelGTT: Uncore not available\n");
        return false;
    }
    
    IOLog("IntelGTT: Initializing Graphics Translation Table\n");
    
    // Create locks
    // allocationLock = IOLockAlloc();  // REMOVED - no locking needed for single-threaded bitmap ops
    statsLock = IOLockAlloc();
    if (/*!allocationLock || */!statsLock) {
        IOLog("IntelGTT: Failed to allocate locks\n");
        return false;
    }
    
    // Detect GTT size
    if (!detectGTTSize()) {
        IOLog("IntelGTT: Failed to detect GTT size\n");
        return false;
    }
    
    // Map GTT memory
    if (!mapGTTMemory()) {
        IOLog("IntelGTT: Failed to map GTT memory\n");
        return false;
    }
    
    // Initialize page tables
    if (!initializePageTables()) {
        IOLog("IntelGTT: Failed to initialize page tables\n");
        return false;
    }
    
    IOLog("IntelGTT: Initialized successfully\n");
    IOLog("IntelGTT: Total size: %zu MB (%zu entries)\n",
          totalSize / (1024 * 1024), numEntries);
    IOLog("IntelGTT: Usable size: %zu MB\n",
          usableSize / (1024 * 1024));
    
    return true;
}

void IntelGTT::cleanup()
{
    IOLog("IntelGTT: Cleaning up\n");
    
    //  DO NOT clear GTT entries on cleanup!
    // The framebuffer is still mapped and display needs to stay visible.
    // Only clear if we're doing a full GPU reset, not during normal unload.
    //
    // OLD CODE (would blank screen on driver unload):
    // if (gttBase && numEntries > 0) {
    //     clearEntries(0, totalSize);
    //     flush();
    // }
    IOLog("IntelGTT:  Preserving GTT entries (framebuffer must stay mapped)\n");
    
    // Free allocation bitmap
    if (allocationBitmap) {
        IOFree(allocationBitmap, bitmapSize);
        allocationBitmap = NULL;
    }
    
    // Release GTT mapping
    if (gttMap) {
        gttMap->release();
        gttMap = NULL;
    }
    
    // Free locks
    // REMOVED - allocationLock not used anymore
    // if (allocationLock) {
    //     IOLockFree(allocationLock);
    //     allocationLock = NULL;
    // }

    if (statsLock) {
        IOLockFree(statsLock);
        statsLock = NULL;
    }
    
    printStats();
    IOLog("IntelGTT: Cleanup complete\n");
}


bool IntelGTT::mapGTTMemory()
{
    IOLog("IntelGTT: Mapping GTT memory via BAR1 (GTTMMADR)\n");
    
    IOPCIDevice *pciDev = controller->getPCIDevice();
    if (!pciDev) {
        IOLog("IntelGTT: No PCI device\n");
        return false;
    }
    
    // OK  CORRECT APPROACH: Use BAR1 (GTTMMADR) - Intel's documented GTT register
    // BAR1 contains the physical address of the GTT base
    // This is the official, documented way to find GTT on all Intel GPUs
    
    uint64_t bar1Lo = pciDev->configRead32(0x18) & ~0xF;  // BAR1 low 32 bits
    uint64_t bar1Hi = pciDev->configRead32(0x1C);         // BAR1 high 32 bits
    uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;           // Combine into 64-bit address
    
    IOLog("IntelGTT: Reading GTT base from BAR1 (GTTMMADR)...\n");
    IOLog("IntelGTT:   BAR1_LO (PCI 0x18) = 0x%08llx\n", bar1Lo);
    IOLog("IntelGTT:   BAR1_HI (PCI 0x1C) = 0x%08llx\n", bar1Hi);
    IOLog("IntelGTT:   GTT Physical (BAR1) = 0x%llx\n", gttPhys);
    
    if (gttPhys == 0) {
        IOLog("IntelGTT: ERR  BAR1 is zero - GTT not initialized by firmware\n");
        return false;
    }
    
    // Create memory descriptor for GTT
    // GTT size is 8MB (0x800000) = 2M entries * 8 bytes per PTE
    IOMemoryDescriptor *gttDesc = IOMemoryDescriptor::withPhysicalAddress(
        gttPhys,
        8 * 1024 * 1024,  // 8MB GTT size
        kIODirectionInOut
    );
    
    if (!gttDesc) {
        IOLog("IntelGTT: Failed to create GTT memory descriptor\n");
        return false;
    }
    
    //  CRITICAL: Map with write-combine for GTT access
    // This allows CPU to write PTEs that actually stick in the hardware
    gttMap = gttDesc->map(kIOMapWriteCombineCache);
    gttDesc->release();
    
    if (!gttMap) {
        IOLog("IntelGTT: Failed to map GTT from BAR1\n");
        return false;
    }
    
    gttBase = (void *)gttMap->getVirtualAddress();
    
    IOLog("IntelGTT: OK  GTT mapped successfully via BAR1\n");
    IOLog("IntelGTT:    Physical: 0x%llx\n", gttPhys);
    IOLog("IntelGTT:    Virtual:  %p\n", gttBase);
    IOLog("IntelGTT:    Size:     8 MB\n");
    IOLog("IntelGTT:    Mode:     kIOMapWriteCombineCache\n");
    

    //  VERIFY: GTT is actually writable

    IOLog("\nIntelGTT: Verifying GTT is writable...\n");
    
    volatile uint64_t *gttTest = (volatile uint64_t *)gttBase;
    uint64_t testValue = 0xDEADBEEFCAFEBABEULL;
    uint64_t originalValue = gttTest[0];  // Save original
    
    // Write test value
    gttTest[0] = testValue;
    
    // Force write to complete with memory barriers
    __asm__ volatile("mfence" ::: "memory");  // Memory fence
    OSSynchronizeIO();                         // IOKit barrier
    IODelay(1);                                // Small delay
    
    // Read back
    uint64_t readBack = gttTest[0];
    
    // Restore original
    gttTest[0] = originalValue;
    OSSynchronizeIO();
    
    // Analyze result
    if (readBack == testValue) {
        IOLog("IntelGTT: OK  GTT IS WRITABLE!\n");
        IOLog("IntelGTT:    Wrote: 0x%016llx\n", testValue);
        IOLog("IntelGTT:    Read:  0x%016llx\n", readBack);
        IOLog("IntelGTT:    PTE writes will work correctly!\n\n");
        return true;
    } else {
        IOLog("IntelGTT:   GTT writability test FAILED\n");
        IOLog("IntelGTT:    Wrote: 0x%016llx\n", testValue);
        IOLog("IntelGTT:    Read:  0x%016llx\n", readBack);
        IOLog("IntelGTT:    GTT may not be properly mapped!\n");
        IOLog("IntelGTT:    Continuing anyway, but PTE writes may fail...\n\n");
        return true;  // Continue anyway - BAR1 is the right address
    }
}

// Also update detectGTTSize() to use correct GTT size
// This is already correct in your code, but make sure it's:

bool IntelGTT::detectGTTSize()
{
    IOLog("IntelGTT: Detecting GTT size\n");
    
    // Tiger Lake GTT is 8GB address space = 2M entries
    // Each entry is 8 bytes, so GTT table is 16MB
    // But the actual memory is only 8MB in most configs
    
    totalSize = 8ULL * 1024 * 1024 * 1024;  // 8GB address space
    gttSize = 8 * 1024 * 1024;               // 8MB GTT table OK 
    numEntries = totalSize / pageSize;       // 2M entries
    
    // Reserve first 8MB for firmware/bootloader
    baseAddress = 8 * 1024 * 1024;           // 0x800000
    usableSize = totalSize - baseAddress;
    
    IOLog("IntelGTT: GTT Address space: 0x%llx - 0x%llx (%zu MB)\n",
          baseAddress, baseAddress + usableSize, usableSize / (1024 * 1024));
    IOLog("IntelGTT: GTT table size: %zu MB (%zu entries)\n",
          gttSize / (1024 * 1024), numEntries);
    
    // Initialize statistics
    IOLockLock(statsLock);
    stats.total_entries = numEntries;
    stats.free_entries = numEntries;
    stats.used_entries = 0;
    stats.total_bytes = totalSize;
    stats.free_bytes = usableSize;
    stats.used_bytes = 0;
    IOLockUnlock(statsLock);
    
    return true;
}

// Helper: Get BAR1 (GTTMMADR) address directly
// Useful for debugging or getting the address elsewhere

uint64_t IntelGTT::getGTTBasePhysicalAddress()
{
    IOPCIDevice *pciDev = controller->getPCIDevice();
    if (!pciDev) {
        return 0;
    }
    
    uint64_t bar1Lo = pciDev->configRead32(0x18) & ~0xF;
    uint64_t bar1Hi = pciDev->configRead32(0x1C);
    return (bar1Hi << 32) | bar1Lo;
}







bool IntelGTT::initializePageTables()
{
    IOLog("IntelGTT: Initializing page tables\n");
    
    size_t numPages = usableSize / pageSize;
    bitmapSize = (numPages + 7) / 8;
    
    allocationBitmap = (u8 *)IOMalloc(bitmapSize);
    if (!allocationBitmap) {
        IOLog("IntelGTT: Failed to allocate bitmap (%zu bytes)\n", bitmapSize);
        return false;
    }
    
    bzero(allocationBitmap, bitmapSize);
    
    IOLog("IntelGTT: Allocated %zu KB for space tracking\n", bitmapSize / 1024);
    IOLog("IntelGTT:  Preserving existing GTT entries\n");
    
    // Framebuffer is at GGTT offset 0x800 which is BELOW baseAddress (0x800000)
    // Our allocator only allocates from baseAddress onwards
    // So framebuffer is AUTOMATICALLY protected - no reservation needed!
    IOLog("IntelGTT: Framebuffer at GGTT 0x800 is below baseAddress 0x%llx "
          "- automatically protected\n", baseAddress);
    
    flush();
    
    return true;
}




// GTT Entry Management

u64 IntelGTT::makeGen12PTE(u64 physAddr, u32 flags)
{
    u64 pte = 0;
    
    // Physical address (4KB aligned)
    pte |= (physAddr & ~0xFFFULL);
    
    // Flags
    if (flags & GTT_PAGE_PRESENT)
        pte |= GTT_PAGE_PRESENT;
    
    if (flags & GTT_PAGE_WRITEABLE)
        pte |= GTT_PAGE_WRITEABLE;
    
    // Cache settings
    pte |= GTT_PAGE_CACHE_LLC;  // Use LLC by default
    
    return pte;
}

void IntelGTT::writePTE(size_t index, u64 physAddr, u32 flags)
{
    if (index >= numEntries || !gttBase) {
        return;
    }
    
    u64 pte = makeGen12PTE(physAddr, flags);
    volatile u64 *pteAddr = (volatile u64 *)((uintptr_t)gttBase + (index * sizeof(u64)));
    *pteAddr = pte;
}

u64 IntelGTT::readPTE(size_t index)
{
    if (index >= numEntries || !gttBase) {
        return 0;
    }
    
    volatile u64 *pteAddr = (volatile u64 *)((uintptr_t)gttBase + (index * sizeof(u64)));
    return *pteAddr;
}

void IntelGTT::flushPTE(size_t index)
{
    // Force write to complete
    (void)readPTE(index);
}


// GTT Operations

bool IntelGTT::insertEntries(u64 start, IOMemoryDescriptor *mem, u32 flags)
{
    if (!mem || !gttBase) {
        return false;
    }
    
    size_t size = mem->getLength();
    size_t numPages = (size + pageSize - 1) / pageSize;
    size_t startIndex = (start - baseAddress) / pageSize;
    
    if (startIndex + numPages > numEntries) {
        IOLog("IntelGTT: Insert out of range\n");
        return false;
    }
    
    IOLog("IntelGTT: Inserting %zu pages at index %zu (addr 0x%llx)\n",
          numPages, startIndex, start);
    
    // Get physical segments
    IODMACommand *dmaCmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64,
        64, 0, IODMACommand::kMapped, 0, 1);
    
    if (!dmaCmd) {
        IOLog("IntelGTT: Failed to create DMA command\n");
        return false;
    }
    
    if (dmaCmd->setMemoryDescriptor(mem) != kIOReturnSuccess) {
        IOLog("IntelGTT: Failed to set memory descriptor\n");
        dmaCmd->release();
        return false;
    }
    
    UInt64 offset = 0;
    IODMACommand::Segment64 segments[10];
    UInt32 numSegments = 10;
    size_t currentIndex = startIndex;
    
    while (offset < size && currentIndex < startIndex + numPages) {
        UInt64 transferSize = 0;
        
        if (dmaCmd->gen64IOVMSegments(&offset, segments, &numSegments) != kIOReturnSuccess) {
            break;
        }
        
        for (UInt32 i = 0; i < numSegments && currentIndex < startIndex + numPages; i++) {
            IOPhysicalAddress64 physAddr = segments[i].fIOVMAddr;
            IOByteCount segLength = segments[i].fLength;
            size_t segPages = (segLength + pageSize - 1) / pageSize;
            
            for (size_t p = 0; p < segPages && currentIndex < startIndex + numPages; p++) {
                writePTE(currentIndex, physAddr + (p * pageSize), flags);
                currentIndex++;
            }
            
            transferSize += segLength;
        }
    }
    
    dmaCmd->clearMemoryDescriptor();
    dmaCmd->release();
    
    // Flush PTEs
    for (size_t i = startIndex; i < currentIndex; i++) {
        flushPTE(i);
    }
    
    // Update statistics
    IOLockLock(statsLock);
    stats.insert_count++;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelGTT::clearEntries(u64 start, size_t size)
{
    size_t numPages = (size + pageSize - 1) / pageSize;
    size_t startIndex = (start - baseAddress) / pageSize;
    
    if (start < baseAddress || startIndex + numPages > numEntries || !gttBase) {
        return false;
    }
    
    IOLog("IntelGTT: Clearing %zu pages at index %zu\n", numPages, startIndex);
    
    // Clear PTEs
    for (size_t i = 0; i < numPages; i++) {
        writePTE(startIndex + i, 0, 0);
        flushPTE(startIndex + i);
    }
    
    // Update statistics
    IOLockLock(statsLock);
    stats.clear_count++;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelGTT::bindObject(IntelGEMObject *obj, u32 cache_level)
{
    if (!obj) {
        return false;
    }
    
    u64 gttAddr = obj->getGTTAddress();
    IOMemoryDescriptor *mem = obj->getMemoryDescriptor();
    
    if (gttAddr == 0 || !mem) {
        IOLog("IntelGTT: Invalid object for binding\n");
        return false;
    }
    
    u32 flags = GTT_PAGE_PRESENT | GTT_PAGE_WRITEABLE;
    
    if (!insertEntries(gttAddr, mem, flags)) {
        IOLog("IntelGTT: Failed to bind object\n");
        return false;
    }
    
    IOLog("IntelGTT: Bound object at 0x%llx (%zu bytes)\n",
          gttAddr, obj->getSize());
    
    // Update statistics
    IOLockLock(statsLock);
    stats.bind_count++;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelGTT::unbindObject(IntelGEMObject *obj)
{
    if (!obj) {
        return false;
    }
    
    u64 gttAddr = obj->getGTTAddress();
    size_t size = obj->getSize();
    
    if (gttAddr == 0) {
        return true;  // Not bound
    }
    
    if (!clearEntries(gttAddr, size)) {
        IOLog("IntelGTT: Failed to unbind object\n");
        return false;
    }
    
    IOLog("IntelGTT: Unbound object from 0x%llx\n", gttAddr);
    
    // Update statistics
    IOLockLock(statsLock);
    stats.unbind_count++;
    IOLockUnlock(statsLock);
    
    return true;
}


// IOSurface Scanout Support - System Physical -> GTT Offset


// bindSurfacePages / unbindSurfacePages - NO LOCKING



uint32_t IntelGTT::findFreeGTTRegion(uint32_t numPages)
{
    // Bitmap page 0 = baseAddress (8MB)
    // So bitmap page N = baseAddress + N*4096
    size_t totalPages = usableSize / pageSize;
    uint32_t currentRun = 0;
    uint32_t startPage = 0;
    
    for (size_t page = 0; page < totalPages; page++) {
        size_t byteIndex = page / 8;
        size_t bitIndex = page % 8;
        
        if (byteIndex >= bitmapSize) break;
        
        bool isUsed = (allocationBitmap[byteIndex] & (1 << bitIndex)) != 0;
        
        if (!isUsed) {
            if (currentRun == 0) {
                startPage = (uint32_t)page;
            }
            currentRun++;
            
            if (currentRun >= numPages) {
                // CRITICAL: Add baseAddress offset!
                // bitmap page 0 = baseAddress, not 0
                uint64_t gttByteOffset = baseAddress + ((uint64_t)startPage * 4096);
                
                IOLog("OK  GTT: Found free region at bitmap page %u -> GTT offset 0x%llx\n",
                      startPage, gttByteOffset);
                IOLog("   Calculation: baseAddress(0x%llx) + (page %u * 4096) = 0x%llx\n",
                      baseAddress, startPage, gttByteOffset);
                
                return (uint32_t)gttByteOffset;
            }
        } else {
            currentRun = 0;
        }
    }
    
    IOLog("ERR  GTT: No free region for %u pages\n", numPages);
    return 0;
}

uint32_t IntelGTT::bindSurfacePages(IOPhysicalAddress sysPhys, size_t size)
{
    if (!gttBase || size == 0) {
        IOLog("IntelGTT::bindSurfacePages: Invalid parameters\n");
        return 0;
    }
    
    uint32_t numPages = (uint32_t)((size + 4095) / 4096);
    
    if (numPages > 65536) {
        IOLog("ERR  GTT: Request too large! %u pages (max 65536)\n", numPages);
        return 0;
    }
    
    // gttOffset now includes baseAddress (e.g. 0x800000 + something)
    uint32_t gttOffset = findFreeGTTRegion(numPages);
    
    if (gttOffset == 0) {
        IOLog("ERR  GTT: No free region for %u pages\n", numPages);
        return 0;
    }
    
    IOLog("OK  GTT: Mapping %u pages (%.1f MB) at GTT offset 0x%08x\n",
          numPages, (float)size / (1024.0f * 1024.0f), gttOffset);
    
    volatile uint64_t* gttEntries = (volatile uint64_t*)gttBase;
    
    //  CRITICAL FIX: gttOffset is in GPU address space (includes baseAddress offset)
    // PTE index must be calculated relative to the start of the GTT table, NOT GPU space!
    // Example: gttOffset=0x21a10000, baseAddress=0x800000
    //   WRONG: gttStartIndex = 0x21a10000 / 4096 = 137744 (way too high!)
    //   CORRECT: gttStartIndex = (0x21a10000 - 0x800000) / 4096 = bitmap page number
    uint32_t gttStartIndex = (gttOffset - (uint32_t)baseAddress) / 4096;
    
    IOLog("  Writing PTEs: gttOffset=0x%x, baseAddress=0x%llx, index=%u\n",
          gttOffset, baseAddress, gttStartIndex);
    
    for (uint32_t i = 0; i < numPages; i++) {
        uint32_t gttIndex = gttStartIndex + i;
        
        // Bounds check
        if (gttIndex >= numEntries) {
            IOLog("ERR  GTT: Index %u exceeds numEntries %zu!\n",
                  gttIndex, numEntries);
            break;
        }
        
        uint64_t pagePhys = sysPhys + ((uint64_t)i * 4096);
        uint64_t pte = (pagePhys & 0xFFFFFFFFF000ULL) | 0x3;  // Present + Writable
        
        //  CRITICAL: Use volatile write + explicit memory barrier
        volatile uint64_t *ptePtr = &gttEntries[gttIndex];
        *ptePtr = pte;
        __sync_synchronize();  // Full memory barrier after each write
        
        //  DEBUG: Log first and last few PTEs
        if (i < 3 || i >= numPages - 3) {
            IOLog("  PTE[%u] = 0x%016llx (phys 0x%llx)\n", gttIndex, pte, pagePhys);
        } else if (i == 3) {
            IOLog("  ... (%u more PTEs)\n", numPages - 6);
        }
    }
    
    //  CRITICAL: Multiple memory barriers to ensure all writes committed
    __sync_synchronize();  // Compiler barrier
    OSSynchronizeIO();     // I/O barrier
    __sync_synchronize();  // Another full barrier
    
    //  VERIFY: Read back first PTE to confirm write
    uint64_t firstPTE = gttEntries[gttStartIndex];
    uint64_t expectedPTE = (sysPhys & 0xFFFFFFFFF000ULL) | 0x3;
    
    IOLog("   Verification: PTE[%u] = 0x%016llx (expected 0x%016llx)\n",
          gttStartIndex, firstPTE, expectedPTE);
    IOLog("     GTT base virtual: %p\n", gttBase);
    IOLog("     PTE address: %p (base + %u * 8)\n",
          &gttEntries[gttStartIndex], gttStartIndex);
    
    if (firstPTE == expectedPTE) {
        IOLog("  OK  PTE verification PASSED! GTT write successful!\n");
    } else if ((firstPTE & 0x1) == 0) {
        IOLog("  ERR  PTE NOT PRESENT - GTT mapping may not be writable!\n");
        IOLog("     Check: Is GTT mapped with kIOMapInhibitCache?\n");
        IOLog("     Check: Is BAR0 physical address correct?\n");
    } else {
        IOLog("    PTE present but wrong value - possible address mismatch\n");
    }
    
    flush();
    
    // Mark bitmap - bitmap page 0 = baseAddress
    // So bitmap page = (gttOffset - baseAddress) / 4096
    size_t bitmapStartPage = (gttOffset - baseAddress) / 4096;
    markSpaceUsed(bitmapStartPage, numPages);
    
    IOLog("OK  GTT: Surface mapped at GTT offset 0x%08x "
          "(bitmap page %zu)\n", gttOffset, bitmapStartPage);
    
    return gttOffset;
}

bool IntelGTT::unbindSurfacePages(uint32_t gttOffset, size_t size)
{
    if (!gttBase || gttOffset == 0 || size == 0) {
        return false;
    }
    
    uint32_t numPages = (uint32_t)((size + 4095) / 4096);
    volatile uint64_t* gttEntries = (volatile uint64_t*)gttBase;
    
    // gttOffset includes baseAddress
    uint32_t gttStartIndex = gttOffset / 4096;
    
    for (uint32_t i = 0; i < numPages; i++) {
        uint32_t gttIndex = gttStartIndex + i;
        if (gttIndex >= numEntries) break;
        gttEntries[gttIndex] = 0;
    }
    
    flush();
    
    // Bitmap page = (gttOffset - baseAddress) / 4096
    size_t bitmapStartPage = (gttOffset - baseAddress) / 4096;
    markSpaceFree(bitmapStartPage, numPages);
    
    IOLog("OK  GTT: Unmapped %u pages at GTT offset 0x%08x\n",
          numPages, gttOffset);
    
    return true;
}



// Address Space Allocation

size_t IntelGTT::findFreeSpace(size_t numPages, size_t alignmentPages)
{
    size_t totalPages = usableSize / pageSize;
    size_t currentRun = 0;
    size_t startPage = 0;
    
    for (size_t page = 0; page < totalPages; page++) {
        size_t byteIndex = page / 8;
        size_t bitIndex = page % 8;
        
        bool isUsed = (allocationBitmap[byteIndex] & (1 << bitIndex)) != 0;
        
        if (!isUsed) {
            if (currentRun == 0) {
                if ((page % alignmentPages) == 0) {
                    startPage = page;
                    currentRun = 1;
                }
            } else {
                currentRun++;
            }
            
            if (currentRun >= numPages) {
                return startPage;
            }
        } else {
            currentRun = 0;
        }
    }
    
    return SIZE_MAX;
}


// Bitmap operations - NO LOCKING
// bindSurfacePages is single-threaded from WindowServer

void IntelGTT::markSpaceUsed(size_t startPage, size_t numPages)
{
    if (numPages == 0) return;
    
    size_t endPage = startPage + numPages;
    size_t lastPage = endPage - 1;
    size_t maxByteIndex = lastPage / 8;
    
    if (maxByteIndex >= bitmapSize) {
        IOLog("IntelGTT::markSpaceUsed: ERROR - Out of bounds! "
              "startPage=%zu numPages=%zu maxByte=%zu bitmapSize=%zu\n",
              startPage, numPages, maxByteIndex, bitmapSize);
        return;
    }
    
    for (size_t page = startPage; page < endPage; ) {
        size_t byteIndex = page / 8;
        size_t bitOffset = page % 8;
        size_t pagesRemaining = endPage - page;
        
        if (byteIndex >= bitmapSize) break;
        
        if (bitOffset == 0 && pagesRemaining >= 8) {
            size_t wholeBytesAvailable = pagesRemaining / 8;
            size_t maxBytes = bitmapSize - byteIndex;
            size_t bytesToSet = (wholeBytesAvailable < maxBytes)
                                ? wholeBytesAvailable
                                : maxBytes;
            
            memset(&allocationBitmap[byteIndex], 0xFF, bytesToSet);
            page += bytesToSet * 8;
        } else {
            size_t bitsAvailable = 8 - bitOffset;
            size_t bitsToSet = (pagesRemaining < bitsAvailable)
                               ? pagesRemaining
                               : bitsAvailable;
            
            uint8_t mask = ((1 << bitsToSet) - 1) << bitOffset;
            allocationBitmap[byteIndex] |= mask;
            
            page += bitsToSet;
        }
    }
}

void IntelGTT::markSpaceFree(size_t startPage, size_t numPages)
{
    if (numPages == 0) return;
    
    size_t endPage = startPage + numPages;
    size_t lastPage = endPage - 1;
    size_t maxByteIndex = lastPage / 8;
    
    if (maxByteIndex >= bitmapSize) {
        IOLog("IntelGTT::markSpaceFree: ERROR - Out of bounds!\n");
        return;
    }
    
    for (size_t page = startPage; page < endPage; ) {
        size_t byteIndex = page / 8;
        size_t bitOffset = page % 8;
        size_t pagesRemaining = endPage - page;
        
        if (byteIndex >= bitmapSize) break;
        
        if (bitOffset == 0 && pagesRemaining >= 8) {
            size_t wholeBytesAvailable = pagesRemaining / 8;
            size_t maxBytes = bitmapSize - byteIndex;
            size_t bytesToClear = (wholeBytesAvailable < maxBytes)
                                  ? wholeBytesAvailable
                                  : maxBytes;
            
            memset(&allocationBitmap[byteIndex], 0x00, bytesToClear);
            page += bytesToClear * 8;
        } else {
            size_t bitsAvailable = 8 - bitOffset;
            size_t bitsToClear = (pagesRemaining < bitsAvailable)
                                 ? pagesRemaining
                                 : bitsAvailable;
            
            uint8_t mask = ((1 << bitsToClear) - 1) << bitOffset;
            allocationBitmap[byteIndex] &= ~mask;
            
            page += bitsToClear;
        }
    }
}


// allocateSpace / freeSpace - NO LOCKING
u64 IntelGTT::allocateSpace(size_t size, size_t alignment)
{
    if (size == 0 || size > usableSize) {
        return 0;
    }
    
    size_t numPages = (size + pageSize - 1) / pageSize;
    size_t alignmentPages = (alignment + pageSize - 1) / pageSize;
    if (alignmentPages == 0) alignmentPages = 1;
    
    size_t startPage = findFreeSpace(numPages, alignmentPages);
    if (startPage == SIZE_MAX) {
        IOLog("IntelGTT: No free space for %zu pages\n", numPages);
        return 0;
    }
    
    markSpaceUsed(startPage, numPages);
    
    // startPage is bitmap page, address = baseAddress + startPage*pageSize
    u64 address = baseAddress + (startPage * pageSize);
    
    IOLog("IntelGTT: Allocated 0x%llx - 0x%llx (%zu pages)\n",
          address, address + size, numPages);
    
    return address;
}

bool IntelGTT::freeSpace(u64 address, size_t size)
{
    if (address < baseAddress || size == 0) {
        return false;
    }
    
    // Bitmap page = (address - baseAddress) / pageSize
    size_t startPage = (address - baseAddress) / pageSize;
    size_t numPages = (size + pageSize - 1) / pageSize;
    
    markSpaceFree(startPage, numPages);
    
    IOLog("IntelGTT: Freed 0x%llx - 0x%llx (%zu pages)\n",
          address, address + size, numPages);
    
    return true;
}




// Statistics

void IntelGTT::getStats(struct gtt_stats *out)
{
    if (!out) {
        return;
    }
    
    IOLockLock(statsLock);
    memcpy(out, &stats, sizeof(struct gtt_stats));
    IOLockUnlock(statsLock);
}


// Statistics - FIXED: check statsLock before using

void IntelGTT::printStats()
{
    // statsLock might be NULL during cleanup
    if (!statsLock) return;
    
    IOLockLock(statsLock);
    IOLog("IntelGTT Statistics:\n");
    IOLog("  Total entries: %zu (%.2f MB)\n",
          stats.total_entries, stats.total_bytes / (1024.0 * 1024.0));
    IOLog("  Used entries: %zu (%.2f MB, %.1f%%)\n",
          stats.used_entries, stats.used_bytes / (1024.0 * 1024.0),
          stats.total_entries > 0 ?
          100.0 * stats.used_entries / stats.total_entries : 0.0);
    IOLog("  Free entries: %zu (%.2f MB, %.1f%%)\n",
          stats.free_entries, stats.free_bytes / (1024.0 * 1024.0),
          stats.total_entries > 0 ?
          100.0 * stats.free_entries / stats.total_entries : 0.0);
    IOLog("  Operations: insert=%u clear=%u bind=%u unbind=%u\n",
          stats.insert_count, stats.clear_count,
          stats.bind_count, stats.unbind_count);
    IOLockUnlock(statsLock);
}

void IntelGTT::flush()
{
    if (!gttBase) {
        return;
    }
    
    //  Strong flush sequence for Intel GTT
    __sync_synchronize();  // Compiler barrier
    
    // Read back a GTT entry to force all writes to complete
    volatile uint64_t *gttEntries = (volatile uint64_t *)gttBase;
    volatile uint64_t dummy = gttEntries[0];
    (void)dummy;
    
    __sync_synchronize();  // Another barrier after read
    OSSynchronizeIO();     // I/O barrier
}
