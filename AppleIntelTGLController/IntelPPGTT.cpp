/*
 * IntelPPGTT.cpp
 * 
 * Per-Process Graphics Translation Table implementation
 */

#include "IntelPPGTT.h"
#include "AppleIntelTGLController.h"
#include "IntelGEM.h"
#include "IntelGEMObject.h"
#include "IntelGTT.h"
#include <IOKit/IOLib.h>

IntelPPGTT::IntelPPGTT()
    : controller(NULL)
    , globalGtt(NULL)
    , pml4Obj(NULL)
    , pml4Virtual(NULL)
    , pml4Address(0)
    , baseAddress(0)
    , totalSize(PPGTT_ADDRESS_SPACE_SIZE)
    , pageSize(PPGTT_PAGE_SIZE)
    , allocationBitmap(NULL)
    , bitmapSize(0)
    , allocationLock(NULL)
    , statsLock(NULL)
{
    bzero(&stats, sizeof(stats));
}

IntelPPGTT::~IntelPPGTT()
{
    cleanup();
}

bool IntelPPGTT::init(AppleIntelTGLController *ctrl)
{
    if (!ctrl) {
        IOLog("IntelPPGTT: Invalid controller\n");
        return false;
    }
    
    controller = ctrl;
    globalGtt = controller->getGTT();
    
    if (!globalGtt) {
        IOLog("IntelPPGTT: No global GTT\n");
        return false;
    }
    
    IOLog("IntelPPGTT: Initializing Per-Process GTT\n");
    IOLog("IntelPPGTT: Address space: %zu GB\n", totalSize / (1024ULL * 1024 * 1024));
    
    // Create locks
    allocationLock = IOLockAlloc();
    statsLock = IOLockAlloc();
    if (!allocationLock || !statsLock) {
        IOLog("IntelPPGTT: Failed to allocate locks\n");
        return false;
    }
    
    // Allocate page tables
    if (!allocatePageTables()) {
        IOLog("IntelPPGTT: Failed to allocate page tables\n");
        return false;
    }
    
    // Initialize root page table
    if (!initializeRoot()) {
        IOLog("IntelPPGTT: Failed to initialize root\n");
        return false;
    }
    
    // Setup allocation bitmap
    size_t numPages = totalSize / pageSize;
    bitmapSize = (numPages + 7) / 8;
    
    allocationBitmap = (u8 *)IOMalloc(bitmapSize);
    if (!allocationBitmap) {
        IOLog("IntelPPGTT: Failed to allocate bitmap\n");
        return false;
    }
    
    bzero(allocationBitmap, bitmapSize);
    
    // Initialize statistics
    IOLockLock(statsLock);
    stats.total_entries = numPages;
    stats.used_entries = 0;
    stats.total_bytes = totalSize;
    stats.used_bytes = 0;
    IOLockUnlock(statsLock);
    
    IOLog("IntelPPGTT: Initialized successfully\n");
    IOLog("IntelPPGTT: Root page table at GPU 0x%llx\n", pml4Address);
    
    return true;
}

void IntelPPGTT::cleanup()
{
    IOLog("IntelPPGTT: Cleaning up\n");
    
    // Free allocation bitmap
    if (allocationBitmap) {
        IOFree(allocationBitmap, bitmapSize);
        allocationBitmap = NULL;
    }
    
    // Release page table root
    if (pml4Obj) {
        if (pml4Virtual) {
            pml4Obj->unmapCPU();
            pml4Virtual = NULL;
        }
        
        if (globalGtt && pml4Address) {
            globalGtt->unbindObject(pml4Obj);
            globalGtt->freeSpace(pml4Address, pml4Obj->getSize());
        }
        
        IntelGEM *gem = controller->getGEM();
        if (gem) {
            gem->destroyObject(pml4Obj);
        }
        pml4Obj = NULL;
    }
    
    // Free locks
    if (allocationLock) {
        IOLockFree(allocationLock);
        allocationLock = NULL;
    }
    
    if (statsLock) {
        IOLockFree(statsLock);
        statsLock = NULL;
    }
    
    printStats();
    IOLog("IntelPPGTT: Cleanup complete\n");
}


// Page Table Setup

bool IntelPPGTT::allocatePageTables()
{
    IOLog("IntelPPGTT: Allocating root page table (PML4)\n");
    
    IntelGEM *gem = controller->getGEM();
    if (!gem) {
        IOLog("IntelPPGTT: No GEM manager\n");
        return false;
    }
    
    // Allocate PML4 (4KB for Gen12)
    pml4Obj = gem->createObject(4096, 0);
    if (!pml4Obj) {
        IOLog("IntelPPGTT: Failed to create PML4 object\n");
        return false;
    }
    
    // Allocate GTT space for PML4
    pml4Address = globalGtt->allocateSpace(4096, 4096);
    if (pml4Address == 0) {
        IOLog("IntelPPGTT: Failed to allocate GTT space for PML4\n");
        return false;
    }
    
    pml4Obj->setGTTAddress(pml4Address);
    
    // Bind to global GTT
    if (!globalGtt->bindObject(pml4Obj, 0)) {
        IOLog("IntelPPGTT: Failed to bind PML4 object\n");
        return false;
    }
    
    // Map for CPU access
    if (!pml4Obj->mapCPU(&pml4Virtual)) {
        IOLog("IntelPPGTT: Failed to map PML4\n");
        return false;
    }
    
    IOLog("IntelPPGTT: PML4 allocated at GPU 0x%llx, CPU %p\n",
          pml4Address, pml4Virtual);
    
    return true;
}

bool IntelPPGTT::initializeRoot()
{
    if (!pml4Virtual) {
        return false;
    }
    
    IOLog("IntelPPGTT: Initializing root page table\n");
    
    // Clear PML4
    bzero(pml4Virtual, 4096);
    
    // For a full implementation, we would:
    // 1. Allocate PDP (Page Directory Pointer) tables
    // 2. Setup PML4 entries pointing to PDPs
    // 3. Allocate PD (Page Directory) tables on demand
    // 4. Allocate PT (Page Table) tables on demand
    //
    // For now, we just have an empty root that we'll populate on demand
    
    return true;
}


// PTE Management

// Gen12 PTE Bitfield Definitions  
#define GEN12_PTE_PRESENT                (1ULL << 0)  // Bit 0: Valid/Present flag  
#define GEN12_PTE_RW                     (1ULL << 1)  // Bit 1: 0 = Read Only, 1 = Read/Write  
#define GEN12_PTE_PAT_INDEX_SHIFT        2  
#define GEN12_PTE_PAT_INDEX_MASK         (0x3ULL << 2) // Bits 2:3: PAT Index (Cache Control)  
#define GEN12_PTE_PHYS_ADDR_MASK         0x0000FFFFFFFFF000ULL // Bits 12:47: Physical Address  
#define GEN12_PTE_XD                     (1ULL << 63) // Bit 63: eXecute Disable

inline uint64_t gen12_construct_pte(uint64_t phys_addr, uint8_t pat_index, bool is_writeable) {  
    // Mask off the physical address to ensure bits 0-11 are clean  
    uint64_t pte = (phys_addr & GEN12_PTE_PHYS_ADDR_MASK);  
    pte |= GEN12_PTE_PRESENT;  
    if (is_writeable) {  
        pte |= GEN12_PTE_RW;  
    }  
    pte |= ((uint64_t)(pat_index & 0x3) << GEN12_PTE_PAT_INDEX_SHIFT);  
    return pte;  
}

void IntelPPGTT::writePTE(u64 address, u64 physAddr, u32 flags)
{
    if (!pml4Virtual) return;

    // Calculate indices for the 4-level page table
    u64 pml4e_idx = (address >> 39) & 0x1FF;
    u64 pdpe_idx  = (address >> 30) & 0x1FF;
    u64 pde_idx   = (address >> 21) & 0x1FF;
    u64 pte_idx   = (address >> 12) & 0x1FF;

    // Write the constructed PTE
    // Note: For a fully functioning driver, intermediate tables (PDP, PD, PT) 
    // must be allocated on-demand and their physical addresses stored in the parent table.
    // Assuming for now that PT is already allocated and accessible via pml4Virtual.
    
    // Construct PTE
    bool is_writeable = (flags & 1) ? true : false;
    uint8_t pat_index = (flags >> 1) & 0x3; // Simplified: extract PAT from flags
    uint64_t pte = gen12_construct_pte(physAddr, pat_index, is_writeable);

    IOLog("IntelPPGTT: writePTE at addr 0x%llx (indices %llu,%llu,%llu,%llu) -> 0x%llx\n", 
           address, pml4e_idx, pdpe_idx, pde_idx, pte_idx, pte);
}

u64 IntelPPGTT::readPTE(u64 address)
{
    // Stub
    (void)address;
    return 0;
}


// PPGTT Operations

bool IntelPPGTT::insertEntries(u64 start, IOMemoryDescriptor *mem, u32 flags)
{
    if (!mem) {
        return false;
    }
    
    size_t size = mem->getLength();
    size_t numPages = (size + pageSize - 1) / pageSize;
    
    IOLog("IntelPPGTT: Inserting %zu pages at 0x%llx\n", numPages, start);
    
    // In a full implementation, we would:
    // 1. Get physical segments from IOMemoryDescriptor
    // 2. Walk page tables and allocate intermediate levels
    // 3. Write PTEs for each physical page
    //
    // For now, track the allocation
    
    // Update statistics
    IOLockLock(statsLock);
    stats.insert_count++;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelPPGTT::clearEntries(u64 start, size_t size)
{
    size_t numPages = (size + pageSize - 1) / pageSize;
    
    IOLog("IntelPPGTT: Clearing %zu pages at 0x%llx\n", numPages, start);
    
    // Clear PTEs in page tables
    // For now, just update statistics
    
    IOLockLock(statsLock);
    stats.clear_count++;
    IOLockUnlock(statsLock);
    
    return true;
}

bool IntelPPGTT::bindObject(IntelGEMObject *obj, u32 cache_level)
{
    if (!obj) {
        return false;
    }
    
    // For PPGTT binding, we need to:
    // 1. Get a PPGTT address (may differ from GGTT address)
    // 2. Insert entries into this context's page tables
    
    IOLog("IntelPPGTT: Binding object (%zu bytes)\n", obj->getSize());
    
    // Use the object's existing GTT address for simplicity
    u64 address = obj->getGTTAddress();
    if (address == 0) {
        // Allocate PPGTT address
        address = allocateSpace(obj->getSize(), pageSize);
        if (address == 0) {
            IOLog("IntelPPGTT: Failed to allocate address\n");
            return false;
        }
        obj->setGTTAddress(address);
    }
    
    IOMemoryDescriptor *mem = obj->getMemoryDescriptor();
    if (!mem) {
        return false;
    }
    
    return insertEntries(address, mem, 0);
}

bool IntelPPGTT::unbindObject(IntelGEMObject *obj)
{
    if (!obj) {
        return false;
    }
    
    u64 address = obj->getGTTAddress();
    size_t size = obj->getSize();
    
    if (address == 0) {
        return true;
    }
    
    IOLog("IntelPPGTT: Unbinding object from 0x%llx\n", address);
    
    return clearEntries(address, size);
}


// Address Space Allocation

size_t IntelPPGTT::findFreeSpace(size_t numPages, size_t alignmentPages)
{
    IOLockLock(allocationLock);
    
    size_t totalPages = totalSize / pageSize;
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
                IOLockUnlock(allocationLock);
                return startPage;
            }
        } else {
            currentRun = 0;
        }
    }
    
    IOLockUnlock(allocationLock);
    return SIZE_MAX;
}

void IntelPPGTT::markSpaceUsed(size_t startPage, size_t numPages)
{
    IOLockLock(allocationLock);
    
    for (size_t i = 0; i < numPages; i++) {
        size_t page = startPage + i;
        size_t byteIndex = page / 8;
        size_t bitIndex = page % 8;
        allocationBitmap[byteIndex] |= (1 << bitIndex);
    }
    
    IOLockUnlock(allocationLock);
}

void IntelPPGTT::markSpaceFree(size_t startPage, size_t numPages)
{
    IOLockLock(allocationLock);
    
    for (size_t i = 0; i < numPages; i++) {
        size_t page = startPage + i;
        size_t byteIndex = page / 8;
        size_t bitIndex = page % 8;
        allocationBitmap[byteIndex] &= ~(1 << bitIndex);
    }
    
    IOLockUnlock(allocationLock);
}

u64 IntelPPGTT::allocateSpace(size_t size, size_t alignment)
{
    if (size == 0 || size > totalSize) {
        return 0;
    }
    
    size_t numPages = (size + pageSize - 1) / pageSize;
    size_t alignmentPages = (alignment + pageSize - 1) / pageSize;
    
    if (alignmentPages == 0) {
        alignmentPages = 1;
    }
    
    size_t startPage = findFreeSpace(numPages, alignmentPages);
    if (startPage == SIZE_MAX) {
        IOLog("IntelPPGTT: No free space for %zu pages\n", numPages);
        return 0;
    }
    
    markSpaceUsed(startPage, numPages);
    
    u64 address = baseAddress + (startPage * pageSize);
    
    // Update statistics
    IOLockLock(statsLock);
    stats.used_entries += numPages;
    stats.used_bytes += numPages * pageSize;
    IOLockUnlock(statsLock);
    
    IOLog("IntelPPGTT: Allocated 0x%llx - 0x%llx (%zu pages)\n",
          address, address + size, numPages);
    
    return address;
}

bool IntelPPGTT::freeSpace(u64 address, size_t size)
{
    if (address < baseAddress || size == 0) {
        return false;
    }
    
    size_t startPage = (address - baseAddress) / pageSize;
    size_t numPages = (size + pageSize - 1) / pageSize;
    
    markSpaceFree(startPage, numPages);
    
    // Update statistics
    IOLockLock(statsLock);
    stats.used_entries -= numPages;
    stats.used_bytes -= numPages * pageSize;
    IOLockUnlock(statsLock);
    
    IOLog("IntelPPGTT: Freed 0x%llx - 0x%llx (%zu pages)\n",
          address, address + size, numPages);
    
    return true;
}


// Query and Statistics

size_t IntelPPGTT::getUsedSize() const
{
    return stats.used_bytes;
}

bool IntelPPGTT::isValid(u64 address, size_t size) const
{
    if (address < baseAddress) {
        return false;
    }
    
    if (address + size > baseAddress + totalSize) {
        return false;
    }
    
    return true;
}

void IntelPPGTT::getStats(struct ppgtt_stats *out)
{
    if (!out) {
        return;
    }
    
    IOLockLock(statsLock);
    memcpy(out, &stats, sizeof(struct ppgtt_stats));
    IOLockUnlock(statsLock);
}

void IntelPPGTT::printStats()
{
    IOLog("IntelPPGTT Statistics:\n");
    IOLog("  Total entries: %zu (%.2f GB)\n",
          stats.total_entries, stats.total_bytes / (1024.0 * 1024.0 * 1024.0));
    IOLog("  Used entries: %zu (%.2f GB, %.1f%%)\n",
          stats.used_entries, stats.used_bytes / (1024.0 * 1024.0 * 1024.0),
          100.0 * stats.used_entries / stats.total_entries);
    IOLog("  Operations: insert=%u clear=%u\n",
          stats.insert_count, stats.clear_count);
}
