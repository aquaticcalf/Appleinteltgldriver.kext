//
//  IntelIOFramebuffer.cpp
//
//
//  IOFramebuffer implementation for macOS integration
//  Week 15: Native graphics framework support
//

// IOKit headers MUST come first
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsInterfaceTypes.h>
#include <IOKit/IOLib.h>
// Add these headers to your IntelIOFramebuffer.cpp
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>

// Our framebuffer header (defines the class)
#include "AppleIntelTGLEGLFramebuffer.h"
#include "AppleIntelTGLController.h"  // For controller access
#include "IntelGTT.h"  // For GTT binding in setScanoutSurface

// External declaration for service plane
extern const IORegistryPlane * gIOServicePlane;

#define super IOFramebuffer
OSDefineMetaClassAndStructors(IntelIOFramebuffer, IOFramebuffer)

// Now include other headers AFTER the metaclass is defined
#include "AppleIntelTGLController.h"
#include "IntelUncore.h"
#include "IntelPowerManagement.h"
#include "IntelDisplay.h"
#include "IntelModeSet.h"
#include "IntelDPLinkTraining.h"



#define kConnectionSupportsAppleSense   0x00000001
#define kConnectionSupportsLLDDCSense   0x00000002
#define kConnectionSupportsHLDDCSense   0x00000004
#define kConnectionSupportsDDCSense     0x00000008
#define kConnectionDisplayParameterCount 0x00000009
#define kConnectionFlags                0x0000000A
#define kConnectionSupportsHotPlug        0x000000A1
// Connection flag values
#define kIOConnectionBuiltIn            0x00000100
#define kIOConnectionDisplayPort        0x00000800

#ifndef kConnectionIsOnline
#define kConnectionIsOnline        'ionl'
#endif

#define kIOFBNotifyDisplayAdded  0x00000010
#define kIOFBConfigChanged       0x00000020
#define kIOFBNotifyDisplayModeChanged 0x00002223
#define kIOFBNotifyDisplayModeChange 'dmod'
#define kIOFBNotifyOnlineState   'nlin'

#ifndef kIOFBVsyncNotification
#define kIOFBVsyncNotification iokit_common_msg(0x300)
#endif

#ifndef kIOFBOnlineKey
#define kIOFBOnlineKey "IOFBOnline"
#endif




// Mode ID generation (width << 16 | height)
#define MAKE_MODE_ID(w, h)  ((IODisplayModeID)(((w) << 16) | (h)))
#define MODE_WIDTH(id)      ((id) >> 16)
#define MODE_HEIGHT(id)     ((id) & 0xFFFF)

// Standard refresh rates
#define REFRESH_60HZ        60
#define REFRESH_50HZ        50

// Hotplug poll interval (1 second)
#define HOTPLUG_POLL_MS     1000

// GEN9 Power Gating register
#define GEN9_PG_ENABLE      0x8000

// Note: initPowerManagement() is now handled by AppleIntelTGLController
// The GPU is already awake when IntelIOFramebuffer::start() is called

bool IntelIOFramebuffer::start(IOService* provider) {
    IOLog(" IntelIOFramebuffer::start() - UNIFIED SINGLE-CLASS ARCHITECTURE\n");

    if (!super::start(provider)) {
        IOLog("ERR  super::start() failed\n");
        return false;
    }

    // Provider is now IOPCIDevice directly (matches GFX0)
    m_pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!m_pciDevice) {
        IOLog("ERR  Provider is not IOPCIDevice\n");
        return false;
    }

    m_pciDevice->retain();
    m_pciDevice->open(this);
    IOLog("OK  PCI device (GFX0) acquired and opened\n");

    // Enable PCI device
    m_pciDevice->setBusMasterEnable(true);
    m_pciDevice->setMemoryEnable(true);
    m_pciDevice->setIOEnable(false);
    m_pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    IOLog("OK  PCI device enabled (D0 power state)\n");

    // Map MMIO (BAR0) - REQUIRED before any GPU access
    IOMemoryMap* mmioMap = m_pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!mmioMap) {
        IOLog("ERR  Failed to map MMIO (BAR0)\n");
        return false;
    }
    mmioBase = (volatile uint8_t*)mmioMap->getVirtualAddress();
    mmioMap->retain();
    IOLog("OK  MMIO mapped - Base: %p, Size: 0x%llx\n", mmioBase, mmioMap->getLength());


    // STEP 1: WAKE GPU (CRITICAL - must happen BEFORE any register access)

    IOLog(" Waking GPU with forcewake...\n");
    
    // Force wake GT (Graphics Technology)
    safeMMIOWrite(0x130090, 0x00000001);  // FORCEWAKE_GT_GEN9 set bit
    IOSleep(10);
    
    // Wait for forcewake acknowledgement
    for (int i = 0; i < 50; i++) {
        uint32_t ack = safeMMIORead(0x130044);  // FORCEWAKE_ACK_GT_GEN9
        if (ack & 0x1) {
            IOLog("OK  GPU forcewake acknowledged (attempt %d): 0x%08X\n", i+1, ack);
            break;
        }
        IOSleep(10);
    }
    
    // Verify GPU is awake
    uint32_t gt_status = safeMMIORead(0x13805C);  // GT_STATUS
    uint32_t forcewake_ack = safeMMIORead(0x130044);  // FORCEWAKE_ACK
    IOLog(" GPU Status: GT_STATUS=0x%08X, FORCEWAKE_ACK=0x%08X\n",
          gt_status, forcewake_ack);


    // STEP 2: FRAMEBUFFER ALLOCATION

    IOLog(" Allocating framebuffer memory...\n");
    const u32 width = 1920;
    const u32 height = 1080;
    const u32 bpp = 4;
    // Changed: 8MB -> 32MB for better performance (supports 4K + buffers)
    u32 fbSize = 32 * 1024 * 1024;  // 32MB fixed
    
    vramMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared,
        fbSize,
        0x000000003FFFF000ULL
    );
    
    if (!vramMemory || vramMemory->prepare() != kIOReturnSuccess) {
        IOLog("ERR  Failed to allocate framebuffer\n");
        if (vramMemory) vramMemory->release();
        // Note: mmioMap is owned by controller, not us
        return false;
    }
    
    vramMemory->retain();
    IOLog("OK  Framebuffer allocated: 0x%llX (%u bytes)\n",
          vramMemory->getPhysicalAddress(), fbSize);
    


    // STEP 3: INITIALIZE FRAMEBUFFER STATE

    m_currentMode = 1;  // Mode ID 1 (reference uses this)
    m_currentDepth = 0;  // Depth index 0
    m_enabled = false;
    m_connected = true;
    m_connectionType = kConnectionTypeeDP;
    m_modeCount = 0;
    
    m_lock = IOLockAlloc();
    if (!m_lock) {
        IOLog("ERR  Failed to allocate lock\n");
        return false;
    }
    
    // Initialize WindowServer-critical memory descriptors
    cursorMemory = nullptr;
    vramRange = nullptr;
    framebufferSurface = nullptr;
    m_vsyncTimer = nullptr;
    m_hotplugTimer = nullptr;
    m_forceGUITimer = nullptr;
    
    // Initialize scanout surface tracking (modern macOS 10.13+)
    currentScanoutPhysicalAddress = 0;
    currentScanoutGTTOffset = 0;  // GTT offset (critical for display)
    currentScanoutWidth = 0;
    currentScanoutHeight = 0;
    currentScanoutStride = 0;
    currentScanoutFormat = 0;
    
    // Initialize interrupt list
    interruptList = OSArray::withCapacity(4);
    if (!interruptList) {
        IOLog("ERR  Failed to allocate interrupt list\n");
        return false;
    }
    
    buildModeList();
    initCursor();
    
    // Create framebuffer surface memory descriptor for WindowServer
    IOPhysicalAddress fbPhysAddr = vramMemory->getPhysicalAddress();
    framebufferSurface = IOMemoryDescriptor::withAddressRange(
        fbPhysAddr,
        fbSize,
        kIODirectionInOut,
        kernel_task
    );
    
    if (framebufferSurface) {
        framebufferSurface->prepare();
        framebufferSurface->retain();
        IOLog("OK  Framebuffer surface created for WindowServer\n");
    }


    // STEP 4: MARK AS CONSOLE/BOOT FRAMEBUFFER (CRITICAL!)

    IOService::setProperty("AAPL,boot-display", kOSBooleanTrue);
    //  IMPORTANT: Start as console, but allow WindowServer takeover
    // Setting this to FALSE allows makeUsable() to transition to GUI
    IOService::setProperty("IOFramebufferConsoleKey", kOSBooleanFalse);
    IOService::setProperty("IOFBCursorSupported", kOSBooleanTrue);
    IOService::setProperty("IOFBHardwareCursorSupported", kOSBooleanTrue);
    

    setProperty("IOFBOnline", kOSBooleanTrue);
    setProperty("IOFBDisplayModeCount", (uint64_t)1, 32);
    setProperty("IOFBIsMainDisplay", kOSBooleanTrue);
    setProperty("AAPL,boot-display", kOSBooleanTrue);

    setProperty("brightness-control", kOSBooleanTrue);
    setProperty("IOBacklight", kOSBooleanTrue);
    // backlight-index / backlight-control-type must be OSNumber
    OSNumber *idx = OSNumber::withNumber((uint64_t)1ULL, 32);
    if (idx) { setProperty("AAPL,backlight-control-type", idx); idx->release(); }
    
    setProperty("IODisplayHasBacklight", kOSBooleanTrue);
    
    //  CRITICAL: WindowServer GPU acceleration properties
    setProperty("IOAccelerated", kOSBooleanTrue);  // <- MISSING! Tells WS GPU is available
    setProperty("IOAccelTypes", "GPU");  // OK  FIXED! Was "IOService"
    setProperty("IOAccelRevision", (unsigned long long)2, 32);   // IOAcceleratorFamily2
    setProperty("IOAccelIndex", (unsigned long long)0, 32);      // Primary GPU
    
  
    setProperty("IOMatchCategory", "IOFramebuffer");
    
    // Tell WindowServer that 3D/2D acceleration is available
    setProperty("IOAcceleration2DSupported", kOSBooleanTrue);
    setProperty("AAPL,HasPanel", kOSBooleanTrue);
    setProperty("AAPL,ndrv-dev", kOSBooleanTrue);
    
    // Memory access properties for WindowServer
    setProperty("IOFBMemoryAccessable", kOSBooleanTrue);
    setProperty("IOFBCPUAccessable", kOSBooleanTrue);
    
    // Pixel format for WindowServer (CRITICAL!)
    setProperty("IOFBPixelFormat", "XRGB8888");
    setProperty("IOFBBitsPerPixel", (uint64_t)32, 32);
    setProperty("IOFBBytesPerRow", (uint64_t)7680, 32);  // 1920 * 4
    

    // GPU INFO PROPERTIES (for apps/system_profiler)

    
    // Device identification
    setProperty("vendor-id", 0x8086, 32);   // Intel vendor ID
    setProperty("device-id", 0x9A49, 32);   // Tiger Lake GT2
    setProperty("revision-id", 0x01, 8);
    setProperty("subsystem-vendor-id", 0x8086, 32);
    setProperty("subsystem-id", 0x7270, 32);
    
    // GPU model name (for System Profiler)
    setProperty("model", "Intel Iris Xe Graphics");
    setProperty("ATY,FamilyName", "Intel Graphics");
    setProperty("ATY,DeviceName", "Intel Iris Xe Graphics");
    setProperty("IOName", "Intel Iris Xe Graphics");
    
    // VRAM properties (shared system memory for TGL)
    OSNumber* vramSize = OSNumber::withNumber(1536ULL * 1024 * 1024, 64); // 1.5GB
    if (vramSize) {
        setProperty("VRAM,totalMB", 1536, 32);
        setProperty("VRAM,totalBytes", vramSize);
        vramSize->release();
    }
    
    // Aperture size (GPU accessible system memory)
    OSNumber* apertureSize = OSNumber::withNumber(4ULL * 1024 * 1024 * 1024, 64); // 4GB
    if (apertureSize) {
        setProperty("Aperture,Size", apertureSize);
        apertureSize->release();
    }
    
    // GPU type and capabilities
    setProperty("built-in", kOSBooleanTrue);           // Built-in GPU
    
    //  GPU Class identifiers for Metal framework
    setProperty("IOGPUClass", "IntelIrisXeGraphics");
    setProperty("IOGPUAcceleratorPresent", kOSBooleanTrue);
    setProperty("IOAccelDeviceShmem", kOSBooleanTrue);  // Required for Metal shared memory
    
    // Metal support
    setProperty("MetalSupported", kOSBooleanTrue);
    setProperty("MetalFamily", 2, 32);                 // Metal 2
    setProperty("MetalFeatureSet", "macOS_GPUFamily2_v1");
    setProperty("MetalPluginName", "AppleIntelTGLGraphicsMTLDriver");
    setProperty("MetalPluginClassName", "MTLIGAccelDevice");  // Principal class in the bundle
    setProperty("MetalStatisticsName", "Intel(R) Iris(R) Xe Graphics");
    
    // OpenGL/OpenCL
    setProperty("IOGLSupported", kOSBooleanTrue);
    setProperty("IOGLBundleName", "AppleIntelTGLGraphicsGLDriver");
    setProperty("IOCLSupported", kOSBooleanTrue);
    
    // Acceleration capabilities
    setProperty("AccelNativeCmdPresent", kOSBooleanTrue);
    setProperty("AccelCaps", 0x1FF, 32);               // All caps
    
    
    IOLog("OK  WindowServer GPU acceleration properties set\n");

        
    
   IODisplayConnect* displayConnect = OSTypeAlloc(IODisplayConnect);
    if (displayConnect && displayConnect->init()) {
        displayConnect->attach(this);
        displayConnect->start(this);
        displayConnect->registerService();
        // AppleDisplay will auto-attach to this
    }
    




    // STEP 5: INITIALIZE I915 CONTROLLER FOR GPU COMMAND SUBMISSION

    IOLog(" Initializing AppleIntelTGLController for GPU commands...\n");
    
    m_i915Controller = new AppleIntelTGLController;
    if (!m_i915Controller) {
        IOLog("ERR  Failed to allocate AppleIntelTGLController\n");
        return false;
    }
    
    if (!m_i915Controller->init()) {
        IOLog("ERR  AppleIntelTGLController::init() failed\n");
        m_i915Controller->release();
        m_i915Controller = nullptr;
        return false;
    }
    
    // Attach controller as child and start it
    if (!m_i915Controller->attach(this)) {
        IOLog("ERR  Failed to attach AppleIntelTGLController\n");
        m_i915Controller->release();
        m_i915Controller = nullptr;
        return false;
    }
    
    if (!m_i915Controller->start(m_pciDevice)) {
        IOLog("ERR  AppleIntelTGLController::start() failed\n");
        m_i915Controller->detach(this);
        m_i915Controller->release();
        m_i915Controller = nullptr;
        return false;
    }
    
    IOLog("OK  AppleIntelTGLController initialized - GPU command submission ready!\n");
    
   


    // STEP 6: POWER MANAGEMENT

    static IOPMPowerState powerStates[] = {
        {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0, kIOPMSleepCapability, kIOPMSleep, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, 0, kIOPMDoze, kIOPMDoze, 0, 0, 0, 0, 0, 0, 0, 0},
        {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
    };
    
    IOService::registerPowerDriver(this, powerStates, 4);
    IOService::changePowerStateTo(3);
    

    

    // STEP 7: ENABLE DISPLAY OUTPUT (CRITICAL!)

    setNumberOfDisplays(1);
    enableController();  // Map GGTT + program display registers + draw test pattern

    IOLog("OK  IntelIOFramebuffer::start() SUCCESS -!\n");
   
   
    
    // Register service (WindowServer will discover us)
    IOService::registerService();
    
    
    return true;
}




void IntelIOFramebuffer::stop(IOService* provider) {
    IOLog("IntelIOFramebuffer: Stopping\n");
    
    // Clean up force GUI timer
    if (m_forceGUITimer) {
        m_forceGUITimer->cancelTimeout();
        IOFramebuffer::getWorkLoop()->removeEventSource(m_forceGUITimer);
        m_forceGUITimer->release();
        m_forceGUITimer = nullptr;
        IOLog("OK  Force GUI timer stopped\n");
    }
    
    // Clean up VSync timer
    if (m_vsyncTimer) {
        m_vsyncTimer->cancelTimeout();
        IOFramebuffer::getWorkLoop()->removeEventSource(m_vsyncTimer);
        m_vsyncTimer->release();
        m_vsyncTimer = nullptr;
        IOLog("OK  VSync timer stopped\n");
    }
    
    if (m_hotplugTimer) {
        m_hotplugTimer->cancelTimeout();
        IOFramebuffer::getWorkLoop()->removeEventSource(m_hotplugTimer);
        m_hotplugTimer->release();
        m_hotplugTimer = nullptr;
    }
    
    // Clean up I915 controller
    if (m_i915Controller) {
        m_i915Controller->stop(m_pciDevice);
        m_i915Controller->detach(this);
        m_i915Controller->release();
        m_i915Controller = nullptr;
        IOLog("OK  AppleIntelTGLController cleaned up\n");
    }
    
    if (vramMemory) {
        vramMemory->release();
        vramMemory = nullptr;
    }
    
    // Clean up WindowServer memory descriptors
    if (cursorMemory) {
        cursorMemory->release();
        cursorMemory = nullptr;
    }
    
    if (vramRange) {
        vramRange->release();
        vramRange = nullptr;
    }
    
    if (framebufferSurface) {
        framebufferSurface->complete();
        framebufferSurface->release();
        framebufferSurface = nullptr;
    }
    
    // Clean up interrupt list
    if (interruptList) {
        for (unsigned int i = 0; i < interruptList->getCount(); i++) {
            OSData* data = OSDynamicCast(OSData, interruptList->getObject(i));
            if (data) {
                InterruptInfo* info = (InterruptInfo*)data->getBytesNoCopy();
                IOFree(info, sizeof(InterruptInfo));
            }
        }
        interruptList->release();
        interruptList = nullptr;
    }
    
    if (m_cursorBuffer) {
        m_cursorBuffer->release();
        m_cursorBuffer = nullptr;
    }
    
    if (m_lock) {
        IOLockFree(m_lock);
        m_lock = nullptr;
    }
    
    IOFramebuffer::stop(provider);
}




void IntelIOFramebuffer::deliverFramebufferNotification(IOIndex index, UInt32 event, void* info) {
    IOLog(" deliverFramebufferNotification() index=%u event=0x%08X\n", index, event);
    
    // Create proper notification info structure if needed
    switch (event) {
        case kIOFBNotifyDisplayModeChange:
        case kIOFBNotifyDisplayAdded:
        case kIOFBConfigChanged:
        case kIOFBVsyncNotification:
            super::deliverFramebufferNotification(index, (void*)(uintptr_t)event);
            break;
        default:
            super::deliverFramebufferNotification(index, info);
            break;
    }
}


IOReturn IntelIOFramebuffer::setPowerState(unsigned long powerState, IOService* device) {
    IOLog("IntelIOFramebuffer: setPowerState(%lu)\n", powerState);
    
    if (powerState == 0) {
        // Power off - disable display
        m_enabled = false;
        // TODO: Disable pipe and port
    } else {
        // Power on - re-enable display
        m_enabled = true;
        // TODO: Re-train link and enable display
    }
    
    return IOPMAckImplied;
}

IOReturn IntelIOFramebuffer::getPixelInformation(
    IODisplayModeID displayMode,
    IOIndex depth,
    IOPixelAperture aperture,
    IOPixelInformation* pixelInfo)
{
    IOLog("IntelIOFramebuffer: >>> getPixelInformation() CALLED mode=%d depth=%d aperture=%d <<<\n",
          displayMode, (int)depth, aperture);
    
    // EXACT from reference - must match mode/depth we advertise elsewhere
    if (!pixelInfo ||
        displayMode != 1 ||          // mode 1 (NOT 0)
        depth != 0 ||                // depth index 0
        aperture != kIOFBSystemAperture)
    {
        IOLog("IntelIOFramebuffer: getPixelInformation(): bad args (mode=%u depth=%u ap=%u)\n",
              displayMode, depth, (unsigned)aperture);
        return kIOReturnBadArgument;
    }
    
    IOLog("IntelIOFramebuffer: getPixelInformation() - valid args\n");
    
    // EXACT copy from reference_working.cpp
    bzero(pixelInfo, sizeof(IOPixelInformation));
    
    //  CRITICAL: Use XRGB8888 (not ARGB) - matches hardware format
    pixelInfo->pixelType = kIO32RGBAPixelFormat;  // NOT kIO32ARGBPixelFormat
    strlcpy(pixelInfo->pixelFormat, "XRGB8888", sizeof(pixelInfo->pixelFormat));
    
    pixelInfo->bitsPerComponent = 8;
    pixelInfo->bitsPerPixel     = 32;
    pixelInfo->componentCount   = 3;  // RGB (no alpha)
    pixelInfo->bytesPerRow      = 1920 * 4;
    pixelInfo->activeWidth      = 1920;
    pixelInfo->activeHeight     = 1080;
    
    // XRGB8888 component masks (X = unused, not alpha)
    pixelInfo->componentMasks[0] = 0x00FF0000;  // R
    pixelInfo->componentMasks[1] = 0x0000FF00;  // G
    pixelInfo->componentMasks[2] = 0x000000FF;  // B
    pixelInfo->componentMasks[3] = 0x00000000;  // X (unused)
    
    IOLog("IntelIOFramebuffer: OK  Pixel info: 1920x1080 XRGB8888 stride=7680 bpp=32\n");
    
    return kIOReturnSuccess;
}

const char* IntelIOFramebuffer::getPixelFormats() {
    //  CRITICAL: Return "XRGB8888" to match hardware (not ARGB)
    static const char pixelFormats[] = "XRGB8888\0";
    return pixelFormats;
}

UInt64 IntelIOFramebuffer::getPixelFormatsForDisplayMode(
    IODisplayModeID displayMode,
    IOIndex depth)
{
    IOLog("IntelIOFramebuffer: getPixelFormatsForDisplayMode(mode=%u depth=%u)\n", displayMode, depth);
    
    // EXACT from reference - Only support our single mode / depth
    if (displayMode != 1 || depth != 0)
        return 0;
    
    // Bit 0 -> first pixel format from getPixelFormats()
    // (getPixelFormats() returns "ARGB8888\0")
    return (1ULL << 0);
}

IOReturn IntelIOFramebuffer::getCurrentDisplayMode(
    IODisplayModeID* displayMode,
    IOIndex* depth)
{
    IOLog("IntelIOFramebuffer: >>> getCurrentDisplayMode() CALLED <<<\n");
    
    if (!displayMode || !depth) {
        return kIOReturnBadArgument;
    }
    
    // EXACT from reference - always return mode 1, depth 0
    *displayMode = 1;
    *depth = 0;
    
    IOLog("IntelIOFramebuffer: OK  Current mode: ID=1, depth=0 (1920x1080@60Hz)\n");
    
    return kIOReturnSuccess;
}

IOItemCount IntelIOFramebuffer::getDisplayModeCount() {
    IOLog("IntelIOFramebuffer: >>> getDisplayModeCount() CALLED - returning 1 <<<\n");
    return 1;  // EXACT from reference - only 1 mode
}

IOReturn IntelIOFramebuffer::getDisplayModes(IODisplayModeID* allDisplayModes) {
    IOLog("IntelIOFramebuffer: >>> getDisplayModes() CALLED <<<\n");
    
    if (!allDisplayModes) {
        IOLog("IntelIOFramebuffer: getDisplayModes(): null pointer\n");
        return kIOReturnBadArgument;
    }
    
    // EXACT copy from reference_working.cpp - return SINGLE mode with ID 1
    allDisplayModes[0] = 1;
    IOLog("IntelIOFramebuffer: getDisplayModes(): returning modeID=1 (1920x1080@60Hz)\n");
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::getInformationForDisplayMode(
    IODisplayModeID displayMode,
    IODisplayModeInformation* info)
{
    IOLog("IntelIOFramebuffer: >>> getInformationForDisplayMode() CALLED for mode=%d <<<\n", displayMode);
    
    if (!info || displayMode != 1) {   // EXACT from reference - mode MUST be 1
        IOLog("IntelIOFramebuffer: Invalid info pointer or mode not supported (expected mode 1)\n");
        return kIOReturnUnsupportedMode;
    }
    
    // EXACT copy from reference_working.cpp
    bzero(info, sizeof(IODisplayModeInformation));
    
    info->maxDepthIndex = 0;           // one depth index
    info->nominalWidth  = 1920;
    info->nominalHeight = 1080;
    info->refreshRate   = (60 << 16);  // 60 Hz fixed-point
    
    // Timing info flags (use available constants)
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    
    IOLog("IntelIOFramebuffer: OK  Returning display mode info: 1920x1080 @ 60Hz\n");
    return kIOReturnSuccess;
}

// CRITICAL: getStartupDisplayMode() - WindowServer calls this FIRST!
IOReturn IntelIOFramebuffer::getStartupDisplayMode(
    IODisplayModeID* displayMode,
    IOIndex* depth)
{
    IOLog("IntelIOFramebuffer: >>> getStartupDisplayMode() CALLED <<<\n");
    
    if (displayMode) *displayMode = 1;   // MUST match getDisplayModes()
    if (depth) *depth = 0;   // depth index 0 (32-bpp)
    
    IOLog("IntelIOFramebuffer: OK  Startup mode: ID=1, depth=0\n");
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID displayMode,
    IOTimingInformation* info)
{
    if (!info) {
        return kIOReturnBadArgument;
    }
    
    DisplayModeInfo* mode = findMode(displayMode);
    if (!mode) {
        return kIOReturnUnsupportedMode;
    }
    
    *info = mode->timing;
    
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::setDisplayMode(
    IODisplayModeID displayMode,
    IOIndex depth)
{
    IOLog("IntelIOFramebuffer: >>> setDisplayMode(mode=%u, depth=%u) CALLED <<<\n",
          (unsigned int)displayMode, (int)depth);
    
    // EXACT from reference - we only support mode 1, depth index 0
    if (displayMode != 1 || depth != 0) {
        IOLog("IntelIOFramebuffer: setDisplayMode: unsupported mode/depth (expected mode=1, depth=0)\n");
        return kIOReturnUnsupportedMode;
    }
    
    IOLockLock(m_lock);
    m_currentMode = displayMode;
    m_currentDepth = depth;
    IOLockUnlock(m_lock);
    
    IOLog("IntelIOFramebuffer: OK  Display mode set to mode=1 depth=0 (1920x1080@60Hz)\n");
    return kIOReturnSuccess;
}



IOReturn IntelIOFramebuffer::setNumberOfDisplays(UInt32 count)
{
    IOLog("setNumberOfDisplays(%u)\n", count);
    return kIOReturnSuccess;
}



IODeviceMemory* IntelIOFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    // EXACT copy from reference_working.cpp (IntelIOFramebuffer::getApertureRange)
    IOLog("IntelIOFramebuffer:  getApertureRange(aperture=%d) CALLED BY WINDOWSERVER! <<<\n", aperture);

    if (!vramMemory) {
        IOLog("IntelIOFramebuffer: ERR  No framebuffer for aperture %d\n", aperture);
        return nullptr;
    }

    IOPhysicalAddress phys = vramMemory->getPhysicalAddress();
    IOByteCount len = vramMemory->getLength();

    // Return shared memory for ALL apertures (WindowServer needs VRAM/cursor)
    if (aperture == 1) {  // VRAM aperture
        IOLog("IntelIOFramebuffer: VRAM aperture - using shared FB\n");
    } else if (aperture == 2) {  // Cursor aperture
        IOLog("IntelIOFramebuffer: Cursor aperture - using shared FB\n");
    } else if (aperture != kIOFBSystemAperture) {
        IOLog("IntelIOFramebuffer:  Unsupported aperture %d - fallback to system\n", aperture);
    }

    // Create and return new IODeviceMemory (WindowServer expects fresh each call)
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, len);
    if (!mem) {
        IOLog("IntelIOFramebuffer: getApertureRange failed to create IODeviceMemory\n");
        return nullptr;
    }

    IOLog("IntelIOFramebuffer: OK OK OK  getApertureRange -> phys=0x%llx len=0x%llx for aperture %d\n",
          (unsigned long long)phys, (unsigned long long)len, aperture);
    return mem;
}

IOIndex IntelIOFramebuffer::getAperture() const {
    return kIOFBSystemAperture;
}

IOItemCount IntelIOFramebuffer::getConnectionCount() {
    IOLog("IntelIOFramebuffer: >>> getConnectionCount() CALLED - returning 1 <<<\n");
    // Single connection per framebuffer
    return 1;
}

IOReturn IntelIOFramebuffer::getAttributeForConnection(
    IOIndex connectIndex,
    IOSelect attribute,
    uintptr_t* value)
{
    IOLog("IntelIOFramebuffer: getAttributeForConnection(conn=%u, attr=0x%08x)\n",
          (unsigned)connectIndex, (unsigned)attribute);
    
    if (!value) {
        return kIOReturnBadArgument;
    }
    
    // Default
    *value = 0;
    
    switch (attribute) {

            case kConnectionSupportsAppleSense:     // 'cena' / sense support
            case kConnectionSupportsDDCSense:
            case kConnectionSupportsHLDDCSense:
            case kConnectionSupportsLLDDCSense:    // 'lddc'
            case kConnectionSupportsHotPlug:
                *value = 1;   // yes, supported
                return kIOReturnSuccess;
        

            case kConnectionDisplayParameterCount:  // 'pcnt'
                *value = 1;   // at least one param
                return kIOReturnSuccess;


            case kConnectionFlags:
                *value = kIOConnectionBuiltIn | kIOConnectionDisplayPort;
                return kIOReturnSuccess;


            case kConnectionIsOnline:              // 'ionl' if asked
                *value = 1;   // panel is online
                return kIOReturnSuccess;

            default:
                // For unknown attributes, just say “no info”
                *value = 0;
                return kIOReturnSuccess;
        }
    }

            
            
            
            
            
            
            

IOReturn IntelIOFramebuffer::setAttributeForConnection(
    IOIndex connect,
    IOSelect attribute,
    uintptr_t value)
{
    IOLog("[IntelIOFramebuffer] setAttributeForConnection(conn=%u, attr=0x%08x, value=0x%lx)\n",
          (unsigned)connect, (unsigned)attribute, (unsigned long)value);

            
                return kIOReturnSuccess;
        }







IOReturn IntelIOFramebuffer::setCursorImage(void* cursorImage) {
    if (!cursorImage) {
        m_cursorImage = nullptr;
        return kIOReturnSuccess;
    }
    
    // Copy cursor image
    // TODO: Upload to cursor plane hardware
    m_cursorImage = cursorImage;
    
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible) {
    IOLockLock(m_lock);
    
    m_cursorX = x;
    m_cursorY = y;
    m_cursorVisible = visible;
    
    updateCursor();
    
    IOLockUnlock(m_lock);
    
    return kIOReturnSuccess;
}

bool IntelIOFramebuffer::hasDDCConnect(IOIndex connectIndex) {
    if (connectIndex != 0) {
        return false;
    }
    
    // We have EDID access via DisplayPort AUX
    return m_connected;
}

IOReturn IntelIOFramebuffer::getDDCBlock(
    IOIndex connectIndex,
    UInt32 blockNumber,
    IOSelect blockType,
    IOOptionBits options,
    UInt8* data,
    IOByteCount* length)
{
    if (connectIndex != 0 || !data || !length) {
        return kIOReturnBadArgument;
    }
    
    if (blockNumber != 0 || blockType != kIODDCBlockTypeEDID) {
        return kIOReturnUnsupported;
    }
    
    // Read EDID via mode set
    if (m_modeSet && m_pipe) {
        EDID edid;
        if (m_modeSet->readEDID(m_pipe, &edid)) {
            // Copy first 128 bytes of EDID
            bcopy(&edid, data, 128);
            *length = 128;
            return kIOReturnSuccess;
        }
    }
    
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::processConnectChange(IOOptionBits* connectChanged) {
    if (!connectChanged) {
        return kIOReturnBadArgument;
    }
    
    bool nowConnected = detectConnection();
    
    if (nowConnected != m_connected) {
        m_connected = nowConnected;
        *connectChanged = 1;
        
        IOLog("IntelIOFramebuffer: Connection %s\n",
              m_connected ? "established" : "lost");
        
        if (m_connected) {
            // Re-train link on connect
            trainDisplayPort();
        }
        
        return kIOReturnSuccess;
    }
    
    *connectChanged = 0;
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::enableController() {
    //  ONLY RUN ONCE - Subsequent calls would overwrite  address!
    static bool enableControllerRan = false;
    if (enableControllerRan) {
        IOLog("IntelIOFramebuffer::enableController() - SKIPPED (already ran, would reset IOSurface!)\n");
        return kIOReturnSuccess;
    }
    
    IOLog("IntelIOFramebuffer::enableController() - USING SAFE MMIO (FIRST CALL ONLY)\n");
    enableControllerRan = true;  // Never run again!
    
    IOSleep(30);

    if (!mmioBase || !vramMemory) {
        IOLog("ERR  MMIO or framebuffer not set\n");
        return kIOReturnError;
    }

    IOLockLock(m_lock);
    
    // Don't skip GGTT mapping even if already enabled - we need to update PLANE_SURF!
    bool wasEnabled = m_enabled;
    if (wasEnabled) {
        IOLog("OK  Display already enabled by BIOS - will update framebuffer pointer\n");
    }

    // Tiger Lake display registers
    const uint32_t PIPE_SRC_A       = 0x6001C;
    const uint32_t PIPECONF_A       = 0x70008;
    const uint32_t TRANS_CONF_A     = 0x70008;
    const uint32_t DDI_BUF_CTL_A_REG    = 0x64000;  // Renamed to avoid macro conflict
    const uint32_t PLANE_CTL_1_A    = 0x70180;
    const uint32_t PLANE_SURF_1_A   = 0x7019C;
    const uint32_t PLANE_STRIDE_1_A = 0x70188;
    const uint32_t PLANE_SIZE_1_A   = 0x70190;
    const uint32_t PLANE_POS_1_A    = 0x7018C;

    // Lambda helpers for safe reads/writes
    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };

    const uint32_t width  = 1920;
    const uint32_t height = 1080;

    IOLog("DEBUG: Reading initial state...\n");
    IOLog("  PLANE_CTL_1_A (before):   0x%08X\n", rd(PLANE_CTL_1_A));
    IOLog("  PLANE_SURF_1_A (before):  0x%08X\n", rd(PLANE_SURF_1_A));
    IOLog("  PIPECONF_A (before):      0x%08X\n", rd(PIPECONF_A));


    uint32_t pipeSrcFromBIOS = rd(PIPE_SRC_A);
    IOLog(" PIPE_SRC_A from BIOS: 0x%08X\n", pipeSrcFromBIOS);
    
    // Only set PIPE_SRC if BIOS didn't set it (0 or uninitialized)
    if (pipeSrcFromBIOS == 0 || pipeSrcFromBIOS == 0xFFFFFFFF) {
        wr(PIPE_SRC_A, ((width - 1) << 16) | (height - 1));
        IOLog("OK  PIPE_SRC_A set to %ux%u (reg=0x%08X)\n", width, height, rd(PIPE_SRC_A));
    } else {
        IOLog("OK  Using BIOS PIPE_SRC_A (not overwriting): 0x%08X\n", pipeSrcFromBIOS);
    }


    wr(PLANE_POS_1_A, 0x00000000);
    wr(PLANE_SIZE_1_A, ((height - 1) << 16) | (width - 1));
    IOLog("OK  PLANE_POS_1_A=0x%08X, PLANE_SIZE_1_A=0x%08X\n",
          rd(PLANE_POS_1_A), rd(PLANE_SIZE_1_A));


    IOLog("Mapping framebuffer into GGTT...\n");
    
    // Read BAR1 (GTTMMADR)
    uint64_t bar1Lo = m_pciDevice->configRead32(0x18) & ~0xF;
    uint64_t bar1Hi = m_pciDevice->configRead32(0x1C);
    uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;
    
    IOLog("BAR1 (GTTMMADR) = 0x%llX\n", gttPhys);
    
    IOMemoryDescriptor* gttDesc = IOMemoryDescriptor::withPhysicalAddress(
        gttPhys, 0x1000000, kIODirectionInOut);
    
    if (!gttDesc) {
        IOLog("ERR  Failed to create GGTT descriptor\n");
        IOLockUnlock(m_lock);
        return kIOReturnError;
    }
    
    IOMemoryMap* gttMap = gttDesc->map();
    if (!gttMap) {
        gttDesc->release();
        IOLog("ERR  Failed to map GTTMMADR\n");
        IOLockUnlock(m_lock);
        return kIOReturnError;
    }
    
    volatile uint64_t* ggtt = (volatile uint64_t*)gttMap->getVirtualAddress();
    
    // Map framebuffer at offset 0x800 (reference value)
    const uint32_t fbGGTTOffset = 0x00000800;
    const uint32_t kPageSize = 4096;
    uint32_t ggttBaseIndex = fbGGTTOffset >> 12;
    
    IOByteCount offset = 0;
    uint32_t page = 0;
    uint32_t fbSize = vramMemory->getLength();
    
    while (offset < fbSize) {
        IOByteCount segLen = 0;
        IOPhysicalAddress segPhys = vramMemory->getPhysicalSegment(offset, &segLen);
        
        if (!segPhys || segLen == 0) break;
        
        segLen &= ~(kPageSize - 1);
        
        for (IOByteCount segOff = 0; segOff < segLen && offset < fbSize;
             segOff += kPageSize, offset += kPageSize, ++page) {
            uint64_t phys = (uint64_t)(segPhys + segOff);
            uint32_t ggttIndex = ggttBaseIndex + page;
            uint64_t pte = (phys & ~0xFFFULL) | 0x3;  // Present + writable
            
            ggtt[ggttIndex] = pte;
        }
    }
    
    gttMap->release();
    gttDesc->release();
    
    IOLog("OK  GGTT mapping complete (%u pages)\n", page);


    IOLog(" Disabling plane before surface update...\n");
    uint32_t planeCtlBefore = rd(PLANE_CTL_1_A);
    wr(PLANE_CTL_1_A, planeCtlBefore & ~(1u << 31));  // Clear bit 31 (PLANE_ENABLE)
    rd(PLANE_SURF_1_A);  // Flush
    IOSleep(2);
    IOLog("  PLANE_CTL_1_A disabled: 0x%08X\n", rd(PLANE_CTL_1_A));


    wr(PLANE_SURF_1_A, fbGGTTOffset);
    IOLog("OK  PLANE_SURF_1_A = 0x%08X (GGTT offset)\n", rd(PLANE_SURF_1_A));


    const uint32_t strideBytes  = 7680;  // 1920 * 4
    const uint32_t strideBlocks = strideBytes / 64;
    wr(PLANE_STRIDE_1_A, strideBlocks);
    IOLog("OK  PLANE_STRIDE_1_A = %u blocks (%u bytes)\n", strideBlocks, strideBytes);


    IOLog(" Applying Tiger Lake FIFO/watermark fix...\n");
    wr(0xC4060, 0x00003FFF);   // WM_LINETIME_A - increase line time watermark
    wr(0xC4064, 0x00000010);   // WM0_PIPE_A - conservative level 0
    wr(0xC4068, 0x00000020);   // WM1_PIPE_A - level 1
    wr(0xC406C, 0x00000040);   // WM2_PIPE_A - level 2
    wr(0xC4070, 0x00000080);   // WM3_PIPE_A - level 3
    wr(0xC4020, 0x0000000F);   // ARB_CTL - give plane highest priority
    IOLog("OK  Tiger Lake watermark/FIFO fix applied\n");


    // Reference: PLANE_CTL = 0x84000008 = bit31=1(enable), bits27:24=0(XRGB8888), bit3=1(PipeA)
    uint32_t planeCtl = 0;
    planeCtl |= (1u << 31);           // Enable plane (bit 31)
    planeCtl &= ~(0xFu << 24);       // Clear format bits (4 bits: 27:24)
    planeCtl |= (0u << 24);           // Format 0 = XRGB8888 (match reference 0x84000008)
    planeCtl &= ~(3u << 10);          // Linear (no tiling, bits 11:10)
    planeCtl &= ~(3u << 14);          // No rotation (bits 15:14)
    planeCtl |= (1u << 3);            // Pipe select: Pipe A (bit 3)
    wr(PLANE_CTL_1_A, planeCtl);
    IOLog("  PLANE_CTL_1_A configured: 0x%08X (ENABLED, format=0 XRGB8888, linear, no rotation)\n", rd(PLANE_CTL_1_A));


    IOLog(" Enabling plane with new surface...\n");
    planeCtl = rd(PLANE_CTL_1_A);
    planeCtl |= (1u << 31);  // Enable plane
    wr(PLANE_CTL_1_A, planeCtl);
    
    // CRITICAL: Must write PLANE_SURF *LAST* to trigger double-buffer flip
    // Re-write PLANE_SURF to trigger atomic update on next VBlank
    IOLog(" Triggering double-buffer update by re-writing PLANE_SURF...\n");
    wr(PLANE_SURF_1_A, fbGGTTOffset);  // Re-write to trigger flip
    
    // Reading PLANE_SURF ensures the write completes
    rd(PLANE_SURF_1_A);
    IOLog("OK  PLANE_CTL_1_A enabled: 0x%08X\n", rd(PLANE_CTL_1_A));
    IOLog("OK  PLANE_SURF_1_A after update: 0x%08X\n", rd(PLANE_SURF_1_A));
    

    uint32_t pipeconf = rd(PIPECONF_A);
    if (!(pipeconf & (1u << 31))) {
        IOLog(" PIPECONF_A not enabled, enabling now...\n");
        pipeconf |= (1u << 31);  // Enable
        pipeconf |= (1u << 30);  // Progressive
        wr(PIPECONF_A, pipeconf);
        IOSleep(5);
    }
    IOLog("OK  PIPECONF_A = 0x%08X (enabled by BIOS)\n", rd(PIPECONF_A));

    uint32_t trans = rd(TRANS_CONF_A);
    if (!(trans & (1u << 31))) {
        IOLog(" TRANS_CONF_A not enabled, enabling now...\n");
        trans |= (1u << 31);
        wr(TRANS_CONF_A, trans);
        IOSleep(5);
    }
    IOLog("OK  TRANS_CONF_A = 0x%08X (enabled by BIOS)\n", rd(TRANS_CONF_A));


    uint32_t ddi = rd(DDI_BUF_CTL_A_REG);
    if (!(ddi & (1u << 31))) {
        IOLog(" DDI_BUF_CTL_A not enabled, enabling now...\n");
        ddi |= (1u << 31);
        ddi &= ~(7u << 24);
        ddi |= (1u << 24);
        wr(DDI_BUF_CTL_A_REG, ddi);
        IOSleep(5);
    }
    IOLog("OK  DDI_BUF_CTL_A = 0x%08X (enabled by BIOS)\n", rd(DDI_BUF_CTL_A_REG));

    


    IOLog(" Enabling panel power...\n");
    wr(0xC7200, rd(0xC7200) | 1);  // PP_CONTROL bit 0 = panel power enable
    IOLog("OK  PP_CONTROL panel power enabled: 0x%08X\n", rd(0xC7200));
    IOLog(" FINAL REGISTER DUMP:\n");
    IOLog("  PIPE_SRC_A       = 0x%08X\n", rd(PIPE_SRC_A));
    IOLog("  PLANE_POS_1_A    = 0x%08X\n", rd(PLANE_POS_1_A));
    IOLog("  PLANE_SIZE_1_A   = 0x%08X\n", rd(PLANE_SIZE_1_A));
    IOLog("  PLANE_SURF_1_A   = 0x%08X\n", rd(PLANE_SURF_1_A));
    IOLog("  PLANE_STRIDE_1_A = 0x%08X\n", rd(PLANE_STRIDE_1_A));
    IOLog("  PLANE_CTL_1_A    = 0x%08X\n", rd(PLANE_CTL_1_A));
    IOLog("  PIPECONF_A       = 0x%08X\n", rd(PIPECONF_A));
    IOLog("  TRANS_CONF_A     = 0x%08X\n", rd(TRANS_CONF_A));
    IOLog("  DDI_BUF_CTL_A    = 0x%08X\n", rd(DDI_BUF_CTL_A_REG));
    



    // STEP 12: Save initial scanout configuration

    // This allows setScanoutSurface() to be called later by IOAccelerator
    // to switch from framebuffer -> IOSurface scanout (modern macOS)
    currentScanoutPhysicalAddress = 0;  // Framebuffer is GTT-mapped, not direct physical
    currentScanoutGTTOffset = fbGGTTOffset;
    currentScanoutWidth = width;
    currentScanoutHeight = height;
    currentScanoutStride = strideBytes;
    currentScanoutFormat = 0;  // XRGB8888
    
    IOLog("OK  Initial scanout configured:\n");
    IOLog("   GTT Offset: 0x%x (framebuffer)\n", currentScanoutGTTOffset);
    IOLog("   Size: %ux%u\n", currentScanoutWidth, currentScanoutHeight);
    IOLog("   Stride: %u bytes\n", currentScanoutStride);
    IOLog("   Format: XRGB8888\n");
    IOLog("    NOTE: WindowServer can call setScanoutSurface() to switch to IOSurface\n");
    
    m_enabled = true;
    IOLockUnlock(m_lock);

    IOLog("OK OK OK  enableController() COMPLETE - Display should be visible! OK OK OK \n");
    return kIOReturnSuccess;
}






IOReturn IntelIOFramebuffer::newUserClient(task_t owningTask,
                                              void* securityID,
                                              UInt32 type,
                                              OSDictionary* properties,
                                              IOUserClient **handler)
{
    IOLog("[IntelIOFramebuffer] newUserClient(type=%u)\n", type);

    //
    // Call the REAL IOFramebuffer::newUserClient !!!
    // We do this because WindowServer REQUIRES the real framebuffer UC.
    //

    IOFramebuffer* fb = OSDynamicCast(IOFramebuffer, this);

    if (!fb) {
        IOLog("[IntelIOFramebuffer] ERROR: this is not an IOFramebuffer\n");
        return kIOReturnUnsupported;
    }

    //
    // Real IOFramebuffer::newUserClient has signature:
    // IOReturn newUserClient(task_t, void*, UInt32, IOUserClient**)
    //
    // So we pass ONLY 4 args, not 5.
    //

    IOReturn ret = fb->IOFramebuffer::newUserClient(owningTask,
                                                    securityID,
                                                    type,
                                                    handler);

    if (ret != kIOReturnSuccess) {
        IOLog("[IntelIOFramebuffer] real IOFramebuffer::newUserClient failed (%x)\n", ret);
        return ret;
    }

    IOLog("[IntelIOFramebuffer] returned REAL IOFramebufferUserClient OK\n");
    return kIOReturnSuccess;
}







IOReturn IntelIOFramebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
    IOLog("set attribute called");
    return kIOReturnSuccess;
}

IOReturn IntelIOFramebuffer::getAttribute(IOSelect attribute, uintptr_t* value) {
    if (!value) {
        return kIOReturnBadArgument;
    }
    
    IOLog("get attribute called");
    
    return kIOReturnSuccess;
}

// CRITICAL WINDOWSERVER METHODS - REQUIRED FOR DISPLAY OUTPUT

//  CRITICAL #0: isConsoleDevice() - Tell WindowServer this WAS the console
bool IntelIOFramebuffer::isConsoleDevice() {
    return true;
}

//  CRITICAL #1: makeUsable() - WindowServer calls this before using framebuffer
bool IntelIOFramebuffer::makeUsable() {
    IOLog("IntelIOFramebuffer: >>> makeUsable() CALLED BY WINDOWSERVER! <<<\n");
    
    return IOFramebuffer::makeUsable();
}

//  CRITICAL #2: clientMemoryForType() - WindowServer gets framebuffer memory here
IOReturn IntelIOFramebuffer::clientMemoryForType(
    UInt32 type,
    UInt32* flags,
    IOMemoryDescriptor** memory)
{
    IOLog("IntelIOFramebuffer: >>> clientMemoryForType(type=%u/0x%08X) CALLED BY WINDOWSERVER! <<<\n",
          type, type);
    
    // Define standard memory types (from IOFramebufferShared.h)
    enum {
        kIOFBSystemAperture  = 0,    // Main framebuffer memory
        kIOFBCursorMemory    = 1,    // Cursor memory
        kIOFBVRAMMemory      = 2     // General VRAM
    };
    
    // System aperture (main framebuffer) - type 0
    if (type == kIOFBSystemAperture) {
        IODeviceMemory* devMem = getVRAMRange();
        if (devMem) {
            *memory = devMem;
            if (flags) *flags = 0;
            IOLog("IntelIOFramebuffer: OK  Returning system aperture memory\n");
            return kIOReturnSuccess;
        }
        IOLog("IntelIOFramebuffer: ERR  Failed to get VRAM range\n");
        return kIOReturnNotReady;
    }
    
    // Cursor memory - type 1
    if (type == kIOFBCursorMemory && cursorMemory) {
        cursorMemory->retain();
        *memory = cursorMemory;
        if (flags) *flags = 0;
        IOLog("IntelIOFramebuffer: OK  Returning cursor memory\n");
        return kIOReturnSuccess;
    }
    
    // VRAM memory - type 2 (for textures/acceleration)
    if (type == kIOFBVRAMMemory) {
        if (framebufferSurface) {
            framebufferSurface->retain();
            *memory = framebufferSurface;
            if (flags) *flags = 0;
            IOLog("IntelIOFramebuffer: OK  Returning VRAM memory\n");
            return kIOReturnSuccess;
        }
        IOLog("IntelIOFramebuffer: ERR  No framebuffer surface available\n");
        return kIOReturnNotReady;
    }
    
    IOLog("IntelIOFramebuffer: ERR  Unsupported memory type: 0x%08X\n", type);
    return kIOReturnUnsupported;
}

//  CRITICAL #3: getVRAMRange() - Helper for clientMemoryForType
IODeviceMemory* IntelIOFramebuffer::getVRAMRange() {
    IOLog("IntelIOFramebuffer:  getVRAMRange() called\n");
    IOLog("IntelIOFramebuffer:   vramRange=%p vramMemory=%p\n", vramRange, vramMemory);
    
    if (!vramMemory) {
        IOLog("IntelIOFramebuffer: ERR ERR ERR  CRITICAL: vramMemory is NULL! Framebuffer not allocated!\n");
        return NULL;
    }
    
    if (!vramRange) {
        IOPhysicalAddress physAddr = vramMemory->getPhysicalAddress();
        IOByteCount length = vramMemory->getLength();
        
        IOLog("IntelIOFramebuffer:  Creating VRAM range: phys=0x%llx len=0x%llx (%llu MB)\n",
              (uint64_t)physAddr, (uint64_t)length, (uint64_t)length / 1024 / 1024);
        
        vramRange = IODeviceMemory::withRange(physAddr, length);
        if (vramRange) {
            vramRange->retain();
            IOLog("IntelIOFramebuffer: OK OK OK  VRAM range created successfully!\n");
        } else {
            IOLog("IntelIOFramebuffer: ERR ERR ERR  CRITICAL: Failed to create IODeviceMemory range!\n");
        }
    } else {
        IOLog("IntelIOFramebuffer:   Using cached vramRange=%p\n", vramRange);
    }
    
    return vramRange;
}

//  IMPORTANT #4: createSharedCursor() - Cursor support
IOReturn IntelIOFramebuffer::createSharedCursor(IOIndex index, int version) {
    IOLog("IntelIOFramebuffer: createSharedCursor(index=%u, version=%d)\n",
          (unsigned)index, version);
    
    if (index != 0 || version != 2) {
        IOLog("IntelIOFramebuffer: Invalid cursor parameters\n");
        return kIOReturnBadArgument;
    }
    
    if (!cursorMemory) {
        cursorMemory = IOBufferMemoryDescriptor::withOptions(
            kIOMemoryKernelUserShared | kIODirectionInOut,
            4096,  // 4KB for cursor
            page_size
        );
        
        if (!cursorMemory) {
            IOLog("IntelIOFramebuffer: ERR  Failed to allocate cursor memory\n");
            return kIOReturnNoMemory;
        }
        
        bzero(cursorMemory->getBytesNoCopy(), 4096);
        cursorMemory->retain();
        IOLog("IntelIOFramebuffer: OK  Shared cursor created\n");
    }
    
    return kIOReturnSuccess;
}

//  IMPORTANT #5: setBounds() - Cursor bounds
IOReturn IntelIOFramebuffer::setBounds(IOIndex index, IOGBounds* bounds) {
    IOLog("IntelIOFramebuffer: setBounds(index=%u)\n", (unsigned)index);
    
    if (bounds) {
        bounds->minx = 0;
        bounds->miny = 0;
        bounds->maxx = 1920;
        bounds->maxy = 1080;
        IOLog("IntelIOFramebuffer: OK  Bounds set to 1920x1080\n");
    }
    
    return kIOReturnSuccess;
}

//  IMPORTANT #6: registerForInterruptType() - VSync notifications
IOReturn IntelIOFramebuffer::registerForInterruptType(
    IOSelect interruptType,
    IOFBInterruptProc proc,
    OSObject* target,
    void* ref,
    void** interruptRef)
{
    IOLog("IntelIOFramebuffer: registerForInterruptType(type=%d)\n", (int)interruptType);
    
    if (interruptType != kIOFBVBLInterruptType) {
        IOLog("IntelIOFramebuffer: Unsupported interrupt type\n");
        return kIOReturnUnsupported;
    }
    
    if (!interruptList) {
        IOLog("IntelIOFramebuffer: ERR  No interrupt list\n");
        return kIOReturnNotReady;
    }
    
    // Store interrupt info
    InterruptInfo* info = (InterruptInfo*)IOMalloc(sizeof(InterruptInfo));
    if (!info) {
        IOLog("IntelIOFramebuffer: ERR  Failed to allocate interrupt info\n");
        return kIOReturnNoMemory;
    }
    
    info->proc = proc;
    info->target = target;
    info->ref = ref;
    
    OSData* data = OSData::withBytes(info, sizeof(InterruptInfo));
    if (data) {
        interruptList->setObject(data);
        data->release();
        *interruptRef = info;
        IOLog("IntelIOFramebuffer: OK  VSync interrupt registered\n");
        

        // START VSYNC TIMER NOW (WindowServer is ready for VSync events)

        if (!m_vsyncTimer) {
            IOLog("  Starting VSync timer (60Hz refresh)...\n");
            
            m_vsyncTimer = IOTimerEventSource::timerEventSource(this,
                reinterpret_cast<IOTimerEventSource::Action>(&IntelIOFramebuffer::vsyncTimerFired));
            
            if (m_vsyncTimer) {
                IOWorkLoop* workLoop = IOFramebuffer::getWorkLoop();
                if (workLoop && workLoop->addEventSource(m_vsyncTimer) == kIOReturnSuccess) {
                    // Start timer at 60Hz (16.67ms intervals)
                    m_vsyncTimer->setTimeoutMS(16);  // First fire in 16ms
                    IOLog("OK  VSync timer started at 60Hz - display should now refresh!\n");
                } else {
                    IOLog("  Failed to add VSync timer to work loop (workLoop=%p)\n", workLoop);
                    m_vsyncTimer->release();
                    m_vsyncTimer = nullptr;
                }
            } else {
                IOLog("  Failed to create VSync timer\n");
            }
        }
        
        return kIOReturnSuccess;
    }
    
    IOFree(info, sizeof(InterruptInfo));
    IOLog("IntelIOFramebuffer: ERR  Failed to create interrupt data\n");
    return kIOReturnNoMemory;
}

//  IMPORTANT #7: unregisterInterrupt() - Cleanup VSync
IOReturn IntelIOFramebuffer::unregisterInterrupt(void* interruptRef) {
    IOLog("IntelIOFramebuffer: unregisterInterrupt()\n");
    
    if (!interruptList) {
        return kIOReturnNotFound;
    }
    
    for (unsigned int i = 0; i < interruptList->getCount(); i++) {
        OSData* data = OSDynamicCast(OSData, interruptList->getObject(i));
        if (data && data->getBytesNoCopy() == interruptRef) {
            IOFree(interruptRef, sizeof(InterruptInfo));
            interruptList->removeObject(i);
            IOLog("IntelIOFramebuffer: OK  Interrupt unregistered\n");
            
            // Stop VSync timer if no more interrupts registered
            if (interruptList->getCount() == 0 && m_vsyncTimer) {
                IOLog("  Stopping VSync timer (no more handlers)\n");
                m_vsyncTimer->cancelTimeout();
                IOWorkLoop* workLoop = IOFramebuffer::getWorkLoop();
                if (workLoop) {
                    workLoop->removeEventSource(m_vsyncTimer);
                }
                m_vsyncTimer->release();
                m_vsyncTimer = nullptr;
            }
            
            return kIOReturnSuccess;
        }
    }
    
    IOLog("IntelIOFramebuffer: ERR  Interrupt reference not found\n");
    return kIOReturnNotFound;
}

//  NICE TO HAVE #8: flushDisplay() - Screen updates
IOReturn IntelIOFramebuffer::flushDisplay() {
    // This can be stubbed for now - hardware auto-updates from framebuffer
    // Could be used for manual flush if needed
    return kIOReturnSuccess;
}

// END OF CRITICAL WINDOWSERVER METHODS

// Private methods

bool IntelIOFramebuffer::initializeHardware() {
    // Detect connection
    m_connected = detectConnection();
    
    if (m_connected) {
        IOLog("IntelIOFramebuffer: Display connected\n");
        
        // Train DisplayPort link
        if (!trainDisplayPort()) {
            IOLog("IntelIOFramebuffer: Link training failed\n");
            return false;
        }
    } else {
        IOLog("IntelIOFramebuffer: No display connected\n");
    }
    
    return true;
}

bool IntelIOFramebuffer::buildModeList() {
    m_modeCount = 0;
    
    // Add common resolutions
    addMode(1920, 1080, REFRESH_60HZ);  // 1080p
    addMode(1920, 1080, REFRESH_50HZ);
    addMode(1680, 1050, REFRESH_60HZ);  // WSXGA+
    addMode(1600, 900, REFRESH_60HZ);   // HD+
    addMode(1440, 900, REFRESH_60HZ);   // WXGA+
    addMode(1366, 768, REFRESH_60HZ);   // WXGA
    addMode(1280, 1024, REFRESH_60HZ);  // SXGA
    addMode(1280, 800, REFRESH_60HZ);   // WXGA
    addMode(1280, 720, REFRESH_60HZ);   // 720p
    addMode(1024, 768, REFRESH_60HZ);   // XGA
    addMode(800, 600, REFRESH_60HZ);    // SVGA
    
    // TODO: Parse EDID for additional modes
    
    IOLog("IntelIOFramebuffer: Built mode list with %u modes\n", m_modeCount);
    
    return m_modeCount > 0;
}

bool IntelIOFramebuffer::addMode(uint32_t width, uint32_t height, uint32_t refreshRate) {
    if (m_modeCount >= kMaxDisplayModes) {
        return false;
    }
    
    DisplayModeInfo* mode = &m_modes[m_modeCount];
    
    mode->modeID = MAKE_MODE_ID(width, height);
    mode->width = width;
    mode->height = height;
    mode->refreshRate = refreshRate;
    mode->flags = 0;
    
    // Fill in timing information (simplified)
    bzero(&mode->timing, sizeof(IOTimingInformation));
    mode->timing.appleTimingID = 0;
    mode->timing.flags = kIODetailedTimingValid;
    mode->timing.detailedInfo.v2.horizontalActive = width;
    mode->timing.detailedInfo.v2.verticalActive = height;
    
    m_modeCount++;
    
    return true;
}

DisplayModeInfo* IntelIOFramebuffer::findMode(IODisplayModeID modeID) {
    for (uint32_t i = 0; i < m_modeCount; i++) {
        if (m_modes[i].modeID == modeID) {
            return &m_modes[i];
        }
    }
    return nullptr;
}

bool IntelIOFramebuffer::applyDisplayMode(DisplayModeInfo* mode) {
    if (!mode) {
        return false;
    }
    
    IOLog("IntelIOFramebuffer: Applying mode %ux%u@%uHz\n",
          mode->width, mode->height, mode->refreshRate);
    
    // TODO: Use IntelModeSet to set mode
    // TODO: Update IntelFramebuffer size
    // TODO: Enable pipe with new mode
    
    return true;
}

bool IntelIOFramebuffer::detectConnection() {
    // TODO: Check port connection status
    // For now, assume connected
    return true;
}

bool IntelIOFramebuffer::trainDisplayPort() {
    if (!m_linkTraining) {
        IOLog("IntelIOFramebuffer: No link training available\n");
        return false;
    }
    
    IOLog("IntelIOFramebuffer: Training DisplayPort link\n");
    
    if (!m_linkTraining->autoTrainLink()) {
        IOLog("IntelIOFramebuffer: Link training failed\n");
        return false;
    }
    
    IOLog("IntelIOFramebuffer: Link training successful\n");
    return true;
}

bool IntelIOFramebuffer::initCursor() {
    // Allocate cursor buffer (128x128 ARGB8888 for HiDPI support)
    // Using kIOMapWriteCombineCache for better performance
    m_cursorBuffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared,
        128 * 128 * 4,  // 128x128 pixels, 4 bytes per pixel (supports HiDPI)
        0x000000003FFFF000ULL
    );
    
    if (!m_cursorBuffer) {
        IOLog("IntelIOFramebuffer: Failed to allocate cursor buffer\n");
        return false;
    }
    
    if (m_cursorBuffer->prepare() != kIOReturnSuccess) {
        IOLog("IntelIOFramebuffer: Failed to prepare cursor buffer\n");
        m_cursorBuffer->release();
        m_cursorBuffer = nullptr;
        return false;
    }
    
    IOLog("IntelIOFramebuffer: Cursor buffer allocated: 128x128 (16KB)\n");
    return true;
}

void IntelIOFramebuffer::updateCursor() {
    // TODO: Program cursor plane registers



}

void IntelIOFramebuffer::hotplugTimerFired(OSObject* target, IOTimerEventSource* sender) {
    IntelIOFramebuffer* fb = OSDynamicCast(IntelIOFramebuffer, target);
    if (fb) {
        fb->handleHotplug();
        sender->setTimeoutMS(HOTPLUG_POLL_MS);
    }
}

void IntelIOFramebuffer::handleHotplug() {
    IOOptionBits connectChanged = 0;
    processConnectChange(&connectChanged);
    
    if (connectChanged) {
        // Notify system of connection change
        // This will trigger resolution re-detection
    }
}

// VSYNC TIMER - CRITICAL FOR DISPLAY REFRESH

void IntelIOFramebuffer::vsyncTimerFired(OSObject* target, IOTimerEventSource* sender) {
    IntelIOFramebuffer* fb = OSDynamicCast(IntelIOFramebuffer, target);
    if (fb) {
        fb->fireVSyncInterrupt();
        // Re-arm timer for next VSync (16.67ms for 60Hz)
        sender->setTimeoutMS(16);
    }
}

void IntelIOFramebuffer::fireVSyncInterrupt() {
    //  CRITICAL: Force display update by re-writing PLANE_SURF
    // This ensures the display scans out the latest framebuffer content
    if (mmioBase && vramMemory) {
        const uint32_t PLANE_SURF_1_A = 0x7019C;
        // Use saved GTT offset from setScanoutSurface, fallback to enableController's 0x800
        const uint32_t gttOffset = currentScanoutGTTOffset ? currentScanoutGTTOffset : 0x00000800;
        
        // Single log for plane surf updates
        static bool planeSurfLogged = false;
        if (!planeSurfLogged) {
            IOLog(" VSync: Starting plane surf updates at 60Hz (GTT offset 0x%08x)\n", gttOffset);
            planeSurfLogged = true;
        }
        
        // Always update PLANE_SURF with the mapped GTT offset
        safeMMIOWrite(PLANE_SURF_1_A, gttOffset);
        // Read back to ensure write completes
        safeMMIORead(PLANE_SURF_1_A);
    }
    
    // Call all registered VSync interrupt handlers
    if (interruptList && interruptList->getCount() > 0) {
        for (unsigned int i = 0; i < interruptList->getCount(); i++) {
            OSData* data = OSDynamicCast(OSData, interruptList->getObject(i));
            if (data) {
                InterruptInfo* info = (InterruptInfo*)data->getBytesNoCopy();
                if (info && info->proc) {
                    // Call WindowServer's VSync handler
                    (info->proc)(info->target, info->ref);
                }
            }
        }
    }
}

unsigned long IntelIOFramebuffer::maxCapabilityForDomainState(IOPMPowerFlags domainState) {
    // Return maximum power state we can support
    if (domainState & kIOPMPowerOn) {
        return 3;  // Full power
    } else if (domainState & kIOPMDoze) {
        return 2;  // Doze
    } else if (domainState & kIOPMSleepCapability) {
        return 1;  // Sleep
    }
    return 0;  // Off
}

unsigned long IntelIOFramebuffer::initialPowerStateForDomainState(IOPMPowerFlags domainState) {
    // Start in powered-on state
    return 3;
}

IOReturn IntelIOFramebuffer::powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                     unsigned long stateNumber,
                                                     IOService* whatDevice) {
    IOLog("IntelIOFramebuffer: Power will change to state %lu\n", stateNumber);
    
    // Prepare for power state change
    if (stateNumber <= 1 && m_displayPowered) {
        // About to power down - save state if needed
        IOLog("IntelIOFramebuffer: Preparing for power down\n");
    }
    
    return IOPMAckImplied;
}

IOReturn IntelIOFramebuffer::powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                                    unsigned long stateNumber,
                                                    IOService* whatDevice) {
    IOLog("IntelIOFramebuffer: Power did change to state %lu\n", stateNumber);
    
    // Handle post-power-state-change tasks
    if (stateNumber >= 2 && m_displayPowered) {
        // Just powered up - restore state if needed
        IOLog("IntelIOFramebuffer: Display powered up, restoring state\n");
        
        // TODO: Restore display mode, retrain link if needed
    }
    
    return IOPMAckImplied;
}

// FORCE GUI TRANSITION - CRITICAL FOR CONSOLE TO GUI HANDOFF

void IntelIOFramebuffer::forceGUITimerFired(OSObject* target, IOTimerEventSource* sender) {
    IntelIOFramebuffer* fb = OSDynamicCast(IntelIOFramebuffer, target);
    if (fb) {
        IOLog(" Force GUI timer fired - initiating console unlock...\n");
        fb->forceGUITransition();
        
        // Cancel timer after firing once
        if (sender) {
            sender->cancelTimeout();
        }
    }
}

void IntelIOFramebuffer::forceGUITransition() {
    IOLog(" FORCE GUI TRANSITION - Unlocking console for WindowServer\n");
    

    // STEP 1: Set all critical online/usable properties

    IOLog("📋 Setting critical GUI properties...\n");
    
    setProperty("IOFBOnline", kOSBooleanTrue);
    setProperty("online", kOSBooleanTrue);
    setProperty("IOFBIsMainDisplay", kOSBooleanTrue);
    setProperty("AAPL,boot-display", kOSBooleanTrue);
    setProperty("IOFBCPUAccessable", kOSBooleanTrue);
    setProperty("IOFBMemoryAccessable", kOSBooleanTrue);
    
    // Remove console lock
    setProperty("IOFramebufferConsoleKey", kOSBooleanFalse);
    
    IOLog("OK  GUI properties set\n");
    

    // STEP 2: Send all display online notifications

    IOLog(" Sending display online notifications...\n");
    
    // Notify that display is online
    deliverFramebufferNotification(0, kIOFBNotifyOnlineState, (void*)(uintptr_t)1);
    
    // Notify display added
    deliverFramebufferNotification(0, kIOFBNotifyDisplayAdded, nullptr);
    
    // Notify configuration changed
    deliverFramebufferNotification(0, kIOFBConfigChanged, nullptr);
    
    // Power on notifications
    deliverFramebufferNotification(0, kIOFBNotifyWillPowerOn, nullptr);
    deliverFramebufferNotification(0, kIOFBNotifyDidPowerOn, nullptr);
    
    // Display mode notifications
    deliverFramebufferNotification(0, kIOFBNotifyDisplayModeWillChange, nullptr);
    deliverFramebufferNotification(0, kIOFBNotifyDisplayModeDidChange, nullptr);
    
    IOLog("OK  Notifications sent\n");
    

    // STEP 3: Post to IOConsoleUsers to unlock screen

    IOLog(" Posting console unlock request...\n");
    
    // Set properties that IOConsoleUsers looks for
    setProperty("IOConsoleUsers", kOSBooleanTrue);
    setProperty("IOScreenLockState", (unsigned long long)0, 32);  // 0 = unlocked
    
    IOLog("OK  Console unlock posted\n");
    

    // STEP 4: Trigger display refresh

    IOLog("  Triggering display refresh...\n");
    
    if (mmioBase) {
        const uint32_t PLANE_SURF_1_A = 0x7019C;
        const uint32_t fbGGTTOffset = 0x00000800;
        
        // Force display update
        safeMMIOWrite(PLANE_SURF_1_A, fbGGTTOffset);
        safeMMIORead(PLANE_SURF_1_A);
    }
    
    IOLog("OK  Display refresh triggered\n");
    

    // STEP 5: Call parent open for display

    IOLog(" Opening framebuffer for display...\n");
    
    // Re-call handleOpen to allow WindowServer access
    handleOpen(this, 0, nullptr);
    
    IOLog("OK  Framebuffer opened\n");
    
    IOLog(" FORCE GUI TRANSITION COMPLETE - GUI should appear now!\n");
}


// MARK: - Modern Scanout Surface (macOS 10.13+ Metal Compositor)


/**
 * Detect which pipe is currently active on the display.
 * Returns: 0=Pipe A, 1=Pipe B, 2=Pipe C, -1=none active
 */
int IntelIOFramebuffer::detectActivePipe() {
    IOLog(" Scanning for active pipe:\n");
    
    for (int pipe = 0; pipe < 3; pipe++) {
        uint32_t pipeConfReg = 0x70008 + (pipe * 0x1000);  // PIPE_CONF_A/B/C
        uint32_t pipeConf = safeMMIORead(pipeConfReg);
        bool enabled = (pipeConf & 0x80000000) != 0;  // Bit 31 = enabled
        
        IOLog("  Pipe %c: CONF=0x%08x @ 0x%05x (enabled=%d)\n",
              'A' + pipe, pipeConf, pipeConfReg, enabled);
        
        if (enabled) {
            IOLog(" Active pipe found: Pipe %c\n", 'A' + pipe);
            return pipe;
        }
    }
    
    IOLog(" No active pipe found, defaulting to Pipe A\n");
    return 0;  // Default to Pipe A if nothing enabled
}

/*!
 * @function setScanoutSurface
 * @abstract CRITICAL: Direct IOSurface -> Plane scanout (NO BLIT!)
 *
 * Modern macOS (10.13+) never blits to framebuffer memory.
 * Instead, IOSurface-backed GPU memory is scanned out DIRECTLY via plane registers.
 *
 * This is why:
 * - VNC works (reads IOSurface)
 * - Console text works (legacy fbcon writes to framebuffer)
 * - GUI was stuck (we were blitting, not programming scanout)
 *
 * Flow:
 *   IOSurface (GPU address) -> PLANE_SURF -> Pipe -> Transcoder -> DDI -> Display
  */
// MARK: - Intel eDP Panel Native Timing (1920x1080 @ 60Hz)
// Internal eDP panel is FIXED TIMING - only supports native resolution
// We program pipe ONCE to 1920x1080 and never change it
// For each surface: only program PLANE registers

#define NATIVE_WIDTH  1920
#define NATIVE_HEIGHT 1080

IOReturn IntelIOFramebuffer::setScanoutSurface(uint64_t gpuSystemAddress,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t stride,
                                               uint32_t pixelFormat,
                                               uint32_t tiling)
{
    if (!mmioBase) {
        IOLog("ERR  setScanoutSurface: MMIO not available\n");
        return kIOReturnNotReady;
    }


    // STEP 1: ONLY ACCEPT NATIVE 1920x1080 SURFACES
    // Internal eDP panel has fixed timing - cannot change
    // macOS may send scaled surfaces (1280x1024, etc) - these are NOT for scanout

    
    if (width != NATIVE_WIDTH || height != NATIVE_HEIGHT) {
        IOLog(" IGNORE: surface %ux%u != native %ux%u (not scanout surface)\n",
              width, height, NATIVE_WIDTH, NATIVE_HEIGHT);
        return kIOReturnSuccess;
    }
    
    // Caller passes a GGTT offset (NOT a physical address).
    uint32_t fbGGTTOffset = (uint32_t)gpuSystemAddress;

    IOLog("  SCANOUT: %ux%u @ GTT offset 0x%08X\n", width, height, fbGGTTOffset);
    IOLog("  Stride:     %u bytes\n", stride);
    IOLog("  Format:     0x%X\n", pixelFormat);

    auto rd = [&](uint32_t off) { return safeMMIORead(off); };
    auto wr = [&](uint32_t off, uint32_t val) { safeMMIOWrite(off, val); };

#define MMIO_BARRIER() do { \
    __asm__ volatile("mfence" ::: "memory"); \
    OSSynchronizeIO(); \
    IODelay(1); \
} while(0)


    static int s_cachedActivePipe = -1;
    int activePipe = s_cachedActivePipe;
    if (activePipe < 0 || activePipe > 2) {
        activePipe = detectActivePipe();
        s_cachedActivePipe = activePipe;
    }
    const uint32_t pipeOffset = (uint32_t)activePipe * 0x1000;

    // Pipe registers - DO NOT TOUCH - fixed timing for eDP
    const uint32_t PIPE_SRC        = 0x6001C + pipeOffset;
    const uint32_t PIPECONF        = 0x70008 + pipeOffset;
    const uint32_t TRANS_CONF      = 0x70008 + pipeOffset;
    const uint32_t DDI_BUF_CTL_A_REG = 0x64000;

    // Plane registers - only these change per surface
    const uint32_t PLANE_CTL_1    = 0x70180 + pipeOffset;
    const uint32_t PLANE_SURF_1   = 0x7019C + pipeOffset;
    const uint32_t PLANE_STRIDE_1 = 0x70188 + pipeOffset;
    const uint32_t PLANE_SIZE_1   = 0x70190 + pipeOffset;
    const uint32_t PLANE_POS_1    = 0x7018C + pipeOffset;
    const uint32_t PLANE_OFFSET_1 = 0x701A4 + pipeOffset;

    // Boot framebuffer is typically at GTT offset 0x800
    // We need to make sure we're switching away from it
    const uint32_t BOOT_FB_GTT_OFFSET = 0x800;

    // Resolve defaults
    if (stride == 0) stride = width * 4;

    // PLANE_STRIDE encoding: value = stride_in_bytes / 64
    const uint32_t strideBlocks = stride / 64;
    if ((stride % 64) != 0) {
        IOLog(" Stride %u not 64-byte aligned\n", stride);
    }


    // STEP 2: READ CURRENT STATE

    uint32_t curCtl = rd(PLANE_CTL_1);
    uint32_t curStride = rd(PLANE_STRIDE_1);
    uint32_t curPlaneSize = rd(PLANE_SIZE_1);
    uint32_t curPlanePos = rd(PLANE_POS_1);
    uint32_t pipeSrc = rd(PIPE_SRC);
    uint32_t curPlaneSurf = rd(PLANE_SURF_1);
    
    // Check DDI status - verify display output is enabled
    uint32_t ddiStatus = rd(DDI_BUF_CTL_A_REG);
    IOLog("  Current: PLANE_CTL=0x%08X STRIDE=0x%08X SIZE=0x%08X\n",
          curCtl, curStride, curPlaneSize);
    IOLog("  Current: PIPE_SRC=0x%08X DDI_STATUS=0x%08X\n", pipeSrc, ddiStatus);
    
    // Check if we're switching from boot framebuffer
    if (curPlaneSurf == BOOT_FB_GTT_OFFSET) {
        IOLog("    Boot framebuffer detected (GTT 0x%X) - switching to IOSurface\n",
              BOOT_FB_GTT_OFFSET);
    } else if (curPlaneSurf != 0 && curPlaneSurf != fbGGTTOffset) {
        IOLog("    Previous surface: GTT 0x%X -> New surface: GTT 0x%X\n",
              curPlaneSurf, fbGGTTOffset);
    }
    IOLog("   Surface switch: 0x%X -> 0x%X\n", curPlaneSurf, fbGGTTOffset);
    
    // Check for other planes that might be covering ours
    // Cursor plane: 0x70080 + pipeOffset
    const uint32_t CUR_CTL = 0x70080 + pipeOffset;
    const uint32_t CUR_SURF = 0x7009C + pipeOffset;
    // Sprite planes: 0x70280 + pipeOffset (Plane 2), 0x70380 (Plane 3)
    const uint32_t SPRITE_CTL_1 = 0x70280 + pipeOffset;
    const uint32_t SPRITE_CTL_2 = 0x70380 + pipeOffset;
    
    uint32_t curCurCtl = rd(CUR_CTL);
    uint32_t curSprite1Ctl = rd(SPRITE_CTL_1);
    uint32_t curSprite2Ctl = rd(SPRITE_CTL_2);
    
    IOLog("  Other planes: CURSOR=0x%08X SPRITE1=0x%08X SPRITE2=0x%08X\n",
          curCurCtl, curSprite1Ctl, curSprite2Ctl);
    
    // Disable cursor and sprite planes to ensure our plane is visible
    if (curCurCtl & (1u << 31)) {
        IOLog("   Disabling cursor plane...\n");
        wr(CUR_CTL, curCurCtl & ~(1u << 31));
        MMIO_BARRIER();
    }
    if (curSprite1Ctl & (1u << 31)) {
        IOLog("   Disabling sprite plane 1...\n");
        wr(SPRITE_CTL_1, curSprite1Ctl & ~(1u << 31));
        MMIO_BARRIER();
    }
    if (curSprite2Ctl & (1u << 31)) {
        IOLog("   Disabling sprite plane 2...\n");
        wr(SPRITE_CTL_2, curSprite2Ctl & ~(1u << 31));
        MMIO_BARRIER();
    }
    
    // Verify DDI is enabled - if not, enable it
    if (!(ddiStatus & (1u << 31))) {
        IOLog("   DDI not enabled, enabling display output...\n");
        ddiStatus |= (1u << 31) | (1u << 24);  // Enable + normal link
        wr(DDI_BUF_CTL_A_REG, ddiStatus);
        MMIO_BARRIER();
        IOLog("     DDI now = 0x%08X\n", rd(DDI_BUF_CTL_A_REG));
    }
    
    // PIPE_SRC should already be 0x0437077F for 1920x1080
    // If it's wrong, we need to fix it ONCE (not per surface)
    static bool pipeTimingsInitialized = false;
    if (!pipeTimingsInitialized) {
        IOLog("   Initializing pipe timing to native 1920x1080...\n");
        
        // Program PIPE_SRC to native
        uint32_t pipeSrcVal = ((NATIVE_HEIGHT - 1) << 16) | (NATIVE_WIDTH - 1); // 0x0437077F
        wr(PIPE_SRC, pipeSrcVal);
        MMIO_BARRIER();
        
        pipeTimingsInitialized = true;
        IOLog("     PIPE_SRC=0x%08X\n", rd(PIPE_SRC));
    }
    
    // Verify pipe is enabled
    uint32_t pipeConf = rd(PIPECONF);
    IOLog("  Pipe config: PIPECONF=0x%08X\n", pipeConf);
    if (!(pipeConf & (1u << 31))) {
        IOLog("   Pipe not enabled, enabling...\n");
        pipeConf |= (1u << 31) | (1u << 30);  // Enable + BLC
        wr(PIPECONF, pipeConf);
        MMIO_BARRIER();
        IOLog("     PIPECONF now = 0x%08X\n", rd(PIPECONF));
    }
    
    // Verify transcoder is enabled
    uint32_t transConf = rd(TRANS_CONF);
    IOLog("  Transcoder: TRANS_CONF=0x%08X\n", transConf);
    if (!(transConf & (1u << 31))) {
        IOLog("   Transcoder not enabled, enabling...\n");
        transConf |= (1u << 31);
        wr(TRANS_CONF, transConf);
        MMIO_BARRIER();
        IOLog("     TRANS_CONF now = 0x%08X\n", rd(TRANS_CONF));
    }


    // STEP 3: PROGRAM PLANE REGISTERS (only these change per surface)

    
    // PLANE_SIZE = surface dimensions
    uint32_t planeSizeVal = ((height - 1) << 16) | (width - 1);

    // Always do full reprogram for debugging - don't use fast path
    IOLog("   Reprogramming plane (debug mode - no fast path)...\n");
    
    // Disable plane
    curCtl &= ~(1u << 31);
    wr(PLANE_CTL_1, curCtl);
    MMIO_BARRIER();

    // Program plane geometry
    wr(PLANE_POS_1, 0);
    wr(PLANE_SIZE_1, planeSizeVal);
    wr(PLANE_OFFSET_1, 0);
    wr(PLANE_STRIDE_1, strideBlocks);
    MMIO_BARRIER();

    // Program plane control: enable, XRGB8888, linear, no rotation
    uint32_t newCtl = rd(PLANE_CTL_1);
    newCtl |= (1u << 31);              // Enable
    newCtl &= ~(0xFu << 24);           // Clear format
    newCtl |= (0x4u << 24);           // XRGB8888
    newCtl &= ~(3u << 10);             // Linear tiling
    newCtl &= ~(3u << 14);             // No rotation
    newCtl |= (1u << 3);               // Pipe A
    wr(PLANE_CTL_1, newCtl);
    MMIO_BARRIER();

    // Program surface address
    wr(PLANE_SURF_1, fbGGTTOffset);
    MMIO_BARRIER();
    wr(PLANE_SURF_1, fbGGTTOffset);  // Double write for latch
    MMIO_BARRIER();

    // Trigger flip - write to PLANE_SURF again to signal hardware
    IODelay(10);
    wr(PLANE_SURF_1, fbGGTTOffset);
    MMIO_BARRIER();


    // STEP 4: VERIFY

    IOLog("  OK  FINAL STATE:\n");
    IOLog("     PLANE_CTL   = 0x%08X\n", rd(PLANE_CTL_1));
    IOLog("     PLANE_SURF  = 0x%08X\n", rd(PLANE_SURF_1));
    IOLog("     PLANE_STRIDE= 0x%08X (%u blocks)\n", rd(PLANE_STRIDE_1), rd(PLANE_STRIDE_1));
    IOLog("     PLANE_SIZE  = 0x%08X\n", rd(PLANE_SIZE_1));
    IOLog("     PIPE_SRC    = 0x%08X\n", rd(PIPE_SRC));
    IOLog("     DDI_STATUS  = 0x%08X\n", rd(DDI_BUF_CTL_A_REG));

    return kIOReturnSuccess;
}
