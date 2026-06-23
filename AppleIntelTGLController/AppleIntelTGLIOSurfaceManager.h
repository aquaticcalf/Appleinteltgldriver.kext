/*
 * AppleIntelTGLIOSurfaceManager.h - IOSurface Integration for WindowServer
 *
 * Complete IOSurface integration for macOS WindowServer compositing.
 * Provides IOSurface creation, management, and GPU memory backing.
 *
 * This bridges the gap between WindowServer's IOSurface API and
 * Intel GPU memory management, enabling hardware-accelerated
 * desktop compositing.
 */

#ifndef IntelIOSurfaceManager_h
#define IntelIOSurfaceManager_h

#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include "AppleIntelTGLController.h"
#include "IntelGEMObject.h"

// Forward declarations
class AppleIntelTGLController;
class IntelGEM;
class IntelGTT;


// MARK: - IOSurface Properties (Apple's Format)


// IOSurface pixel formats (FourCC codes)
#define kIOSurfacePixelFormatBGRA8        'BGRA'  // 32-bit BGRA
#define kIOSurfacePixelFormatRGBA8        'RGBA'  // 32-bit RGBA
#define kIOSurfacePixelFormatYUV420       '420v'  // NV12 YUV 4:2:0
#define kIOSurfacePixelFormatYUV422       '422v'  // NV16 YUV 4:2:2
#define kIOSurfacePixelFormatRGB565       'R565'  // 16-bit RGB565
#define kIOSurfacePixelFormatRGB101010    'R10k'  // 10-bit RGB

// IOSurface creation flags
#define kIOSurfaceBacked       (1 << 0)  // GPU memory backed
#define kIOSurfaceGlobal       (1 << 1)  // Global surface
#define kIOSurfacePurgeable    (1 << 2)  // Purgeable memory
#define kIOSurfaceDisplayable  (1 << 3)  // Can be displayed
#define kIOSurfaceCompressable (1 << 4)  // Can be compressed

// IOSurface allocation flags
#define kIOSurfaceAllocationNormal      0
#define kIOSurfaceAllocationVRAM       1  // Prefer VRAM
#define kIOSurfaceAllocationSystem     2  // System memory fallback
#define kIOSurfaceAllocationPurgeable  3  // Purgeable allocation


// MARK: - IOSurface Data Structure


// Complete IOSurface information (matches Apple's IOSurfaceProperties)
struct IntelIOSurfaceProperties {
    // Surface dimensions
    uint32_t width;                  // Width in pixels
    uint32_t height;                 // Height in pixels
    uint32_t bytesPerRow;            // Bytes per row (stride)
    
    // Pixel format information
    uint32_t pixelFormat;            // FourCC pixel format
    uint32_t bytesPerPixel;          // Bytes per pixel
    uint32_t bitsPerPixel;           // Bits per pixel
    
    // Memory information
    uint64_t size;                  // Total size in bytes
    uint64_t gpuAddress;            // GPU virtual address
    uint64_t physAddress;           // Physical address
    uint32_t allocationType;         // VRAM/System/Purgeable
    
    // Multi-planar formats (YUV)
    uint32_t planeCount;            // Number of planes
    uint32_t planeOffsets[4];       // Plane offsets
    uint32_t planeWidths[4];       // Plane widths
    uint32_t planeHeights[4];      // Plane heights
    uint32_t planeBytesPerRow[4];   // Plane strides
    
    // Compression
    bool compressionEnabled;         // Frame buffer compression
    uint32_t compressionType;       // Compression algorithm
    uint64_t compressionSize;        // Compressed size
    
    // Display properties
    bool displayable;               // Can be displayed
    bool globalSurface;             // Global surface
    uint32_t displayID;             // Display ID (if applicable)
    
    // Performance
    bool cacheable;                 // CPU cacheable
    bool purgeable;                 // Purgeable by system
    uint32_t priority;              // Surface priority
    
    // Usage hints
    uint32_t usage;                 // Usage hints (render, texture, etc.)
    uint32_t protectionLevel;       // Content protection
    
    // Timestamp and sequencing
    uint64_t creationTime;          // Surface creation timestamp
    uint64_t lastAccessTime;       // Last access timestamp
    uint32_t sequenceNumber;        // Surface sequence number
    
    // Apple compatibility
    uint32_t iosurfaceID;           // IOSurface ID
    mach_port_t iosurfacePort;       // IOSurface port
    char name[64];                  // Surface name for debugging
} __attribute__((packed));


// MARK: - Surface Management


// Surface lookup table entry
struct IntelIOSurfaceEntry {
    uint32_t iosurfaceID;            // IOSurface identifier
    IntelGEMObject* gemObject;       // GPU memory backing
    IntelIOSurfaceProperties props;   // Surface properties
    IOMemoryDescriptor* backing;     // Memory descriptor
    mach_port_t port;               // Mach port for sharing
    uint32_t refCount;              // Reference count
    bool inUse;                    // Currently in use?
    uint64_t lastAccess;            // Last access time
    char owner[32];                // Owning process name
    IntelIOSurfaceEntry* next;      // Hash table chaining
};


// MARK: - Statistics and Performance


struct IOSurfaceStatistics {
    // Allocation statistics
    uint64_t totalSurfacesCreated;    // Total surfaces created
    uint64_t totalSurfacesDestroyed;   // Total surfaces destroyed
    uint64_t activeSurfaces;          // Currently active
    uint64_t peakActiveSurfaces;      // Peak active count
    
    // Memory usage
    uint64_t totalMemoryAllocated;    // Total VRAM allocated
    uint64_t currentMemoryUsage;      // Current VRAM usage
    uint64_t peakMemoryUsage;         // Peak memory usage
    uint64_t compressionSavings;      // Memory saved by compression
    
    // Performance
    uint64_t totalCreateTime;        // Total creation time
    uint64_t averageCreateTime;      // Average creation time
    uint64_t lookupHits;            // Cache lookup hits
    uint64_t lookupMisses;          // Cache lookup misses
    float hitRatio;                  // Hit ratio percentage
    
    // Format breakdown
    uint64_t bgra8Surfaces;         // BGRA8 surfaces
    uint64_t yuv420Surfaces;         // NV12 surfaces
    uint64_t yuv422Surfaces;         // NV16 surfaces
    uint64_t otherSurfaces;          // Other formats
    
    // Usage breakdown
    uint64_t displaySurfaces;        // Display surfaces
    uint64_t textureSurfaces;        // Texture surfaces
    uint64_t renderSurfaces;         // Render target surfaces
    uint64_t sharedSurfaces;         // Shared surfaces
};


// MARK: - Configuration


#define MAX_IOSURFACES              4096      // Maximum surfaces
#define IOSURFACE_HASH_BUCKETS        256        // Hash table buckets
#define IOSURFACE_CREATION_TIMEOUT    1000       // Creation timeout (ms)
#define IOSURFACE_MAX_SIZE           (64 * 1024 * 1024) // 64MB max
#define IOSURFACE_MIN_SIZE           (16 * 1024)        // 16KB min


// MARK: - IntelIOSurfaceManager Class


class IntelIOSurfaceManager : public OSObject {
    OSDeclareDefaultStructors(IntelIOSurfaceManager)
    
public:

    // MARK: - Singleton and Lifecycle

    
    static IntelIOSurfaceManager* sharedInstance();
    static void destroySharedInstance();
    
    virtual bool init() override;
    bool initWithController(AppleIntelTGLController* controller);
    virtual void free() override;
    

    // MARK: - IOSurface Creation and Management

    
    // Create IOSurface with specified properties
    IOReturn createSurface(const IntelIOSurfaceProperties* props, uint32_t* outIOSurfaceID);
    
    // Create surface with simplified parameters
    IOReturn createSurface(uint32_t width, uint32_t height, uint32_t pixelFormat,
                         uint32_t flags, uint32_t* outIOSurfaceID);
    
    // Create surface from existing GPU memory
    IOReturn createSurfaceFromGEMObject(IntelGEMObject* gemObject,
                                     const IntelIOSurfaceProperties* props,
                                     uint32_t* outIOSurfaceID);

    // Create surface from existing memory descriptor
    IOReturn createSurfaceFromDescriptor(IOMemoryDescriptor* descriptor,
                                      const IntelIOSurfaceProperties* props,
                                      uint32_t* outIOSurfaceID);
    
    // Surface lifecycle
    IOReturn destroySurface(uint32_t iosurfaceID);
    IOReturn retainSurface(uint32_t iosurfaceID);
    IOReturn releaseSurface(uint32_t iosurfaceID);
    

    // MARK: - Surface Lookup and Properties

    
    // Find surface by ID
    IntelIOSurfaceEntry* findSurface(uint32_t iosurfaceID);
    
    // Get surface properties
    IOReturn getSurfaceProperties(uint32_t iosurfaceID, IntelIOSurfaceProperties* props);
    IOReturn setSurfaceProperties(uint32_t iosurfaceID, const IntelIOSurfaceProperties* props);
    
    // Get GPU backing object
    IntelGEMObject* getSurfaceBacking(uint32_t iosurfaceID);
    
    // Mach port sharing
    IOReturn getSurfacePort(uint32_t iosurfaceID, mach_port_t* outPort);
    IOReturn setSurfacePort(uint32_t iosurfaceID, mach_port_t port);
    

    // MARK: - Surface Operations

    
    // Locking operations (for cross-process access)
    IOReturn lockSurface(uint32_t iosurfaceID, uint32_t lockType, uint32_t timeoutMs);
    IOReturn unlockSurface(uint32_t iosurfaceID);
    
    // Compression operations
    IOReturn compressSurface(uint32_t iosurfaceID, uint32_t compressionType);
    IOReturn decompressSurface(uint32_t iosurfaceID);
    
    // Cache operations
    IOReturn flushSurfaceCache(uint32_t iosurfaceID);
    IOReturn invalidateSurfaceCache(uint32_t iosurfaceID);
    

    // MARK: - Display Integration

    
    // Mark surface for display
    IOReturn markForDisplay(uint32_t iosurfaceID, uint32_t displayID);
    IOReturn removeFromDisplay(uint32_t iosurfaceID);
    
    // Get displayable surfaces
    uint32_t getDisplayableSurfaces(uint32_t* surfaceIDs, uint32_t maxCount);
    
    // Frame buffer operations
    IOReturn setAsFramebuffer(uint32_t iosurfaceID);
    IOReturn getFramebuffer(uint32_t* outIOSurfaceID);
    

    // MARK: - WindowServer Integration

    
    // WindowServer callback registration
    typedef void (*WindowServerCallback)(uint32_t iosurfaceID, uint32_t event);
    IOReturn registerWindowServerCallback(WindowServerCallback callback);
    IOReturn unregisterWindowServerCallback();
    
    // Surface event notification
    IOReturn notifySurfaceEvent(uint32_t iosurfaceID, uint32_t event);
    
    // Global surface operations (for WindowServer)
    IOReturn setGlobalSurface(uint32_t iosurfaceID, bool global);
    IOReturn getGlobalSurfaces(uint32_t* surfaceIDs, uint32_t* count);
    

    // MARK: - Memory Management

    
    // Memory pressure handling
    void handleMemoryPressure(uint32_t pressureLevel);
    IOReturn purgeSurfaces(uint32_t amountToPurge, uint64_t* actualPurged);
    
    // VRAM management
    uint64_t getVRAMUsage();
    uint64_t getVRAMPressure();
    IOReturn trimMemory(uint64_t targetSize);
    
    // Optimization
    IOReturn optimizeMemoryLayout();
    IOReturn compressIdleSurfaces();
    

    // MARK: - Statistics and Debug

    
    // Statistics
    void getStatistics(IOSurfaceStatistics* stats);
    void resetStatistics();
    void printStatistics();
    void printActiveSurfaces();
    
    // Debug and diagnostics
    bool validateSurface(uint32_t iosurfaceID);
    IOReturn dumpSurfaceInfo(uint32_t iosurfaceID);
    void dumpMemoryUsage();
    

    // MARK: - Public Properties

    
    bool isInitialized() const { return m_initialized; }
    uint32_t getActiveSurfaceCount() const { return m_activeSurfaceCount; }
    uint64_t getTotalMemoryUsage() const { return m_totalMemoryUsage; }
    
private:

    // MARK: - Internal Methods

    
    // Hash table operations
    uint32_t hashFunction(uint32_t iosurfaceID);
    IntelIOSurfaceEntry* findEntry(uint32_t iosurfaceID);
    void insertEntry(IntelIOSurfaceEntry* entry);
    void removeEntry(uint32_t iosurfaceID);
    
    // Surface creation helpers
    IOReturn allocateBackingMemory(IntelIOSurfaceProperties* props,
                               IntelGEMObject** outGEMObject);
    IOReturn setupMultiplanarFormats(IntelIOSurfaceProperties* props);
    IOReturn calculateMemoryRequirements(const IntelIOSurfaceProperties* props,
                                     uint64_t* outSize, uint32_t* outAlignment);
    
    // Mach port management
    IOReturn createMachPort(uint32_t iosurfaceID, mach_port_t* outPort);
    IOReturn destroyMachPort(mach_port_t port);
    
    // Compression helpers
    IOReturn setupCompression(IntelIOSurfaceProperties* props);
    uint64_t calculateCompressedSize(uint32_t width, uint32_t height, uint32_t format);
    
    // Reference counting
    uint32_t incrementRefCount(uint32_t iosurfaceID);
    uint32_t decrementRefCount(uint32_t iosurfaceID);
    
    // Statistics helpers
    void updateCreationStats(uint64_t creationTime);
    void updateLookupStats(bool hit);
    void updateMemoryStats(uint64_t delta, bool allocation);
    void updateFormatStats(uint32_t pixelFormat);
    void updateUsageStats(uint32_t usage);
    
    // Cleanup and maintenance
    void cleanupStaleSurfaces();
    void performMaintenance();
    
    // Validation
    bool validateProperties(const IntelIOSurfaceProperties* props);
    bool validatePixelFormat(uint32_t pixelFormat);
    bool validateDimensions(uint32_t width, uint32_t height);
    

    // MARK: - Member Variables

    
    // Singleton
    static IntelIOSurfaceManager* gSharedInstance;
    
    // Core references
    AppleIntelTGLController* m_controller;
    IntelGEM* m_gem;
    IntelGTT* m_gtt;
    
    // Surface management
    IntelIOSurfaceEntry* m_hashTable[IOSURFACE_HASH_BUCKETS];
    uint32_t m_nextIOSurfaceID;
    uint32_t m_activeSurfaceCount;
    IOLock* m_surfaceLock;
    
    // Memory management
    uint64_t m_totalMemoryUsage;
    uint64_t m_peakMemoryUsage;
    uint32_t m_memoryPressure;
    IOTimerEventSource* m_maintenanceTimer;
    
    // Statistics
    IOSurfaceStatistics m_stats;
    IOLock* m_statsLock;
    
    // WindowServer integration
    WindowServerCallback m_windowServerCallback;
    mach_port_t m_notificationPort;
    
    // Frame buffer tracking
    uint32_t m_framebufferSurfaceID;
    bool m_framebufferSet;
    
    // Initialization
    bool m_initialized;
    bool m_compressionEnabled;
    uint32_t m_maxSurfaceSize;
};

#endif /* IntelIOSurfaceManager_h */
