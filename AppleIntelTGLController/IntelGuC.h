/*
 * IntelGuC.h - Graphics Microcontroller (GuC) Support
 * Week 38: GuC Foundation
 *
 * The GuC is Intel's firmware-based GPU scheduler and power controller.
 * It's REQUIRED for Tiger Lake (Gen12) and newer GPUs.
 *
 * Key responsibilities:
 * - Hardware-accelerated command scheduling
 * - Context switching
 * - Power management (SLPC)
 * - Error capture and logging
 * - Firmware-based hang detection
 */

#ifndef INTELGUC_H
#define INTELGUC_H

#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOInterruptEventSource.h>

// Forward declarations
class AppleIntelTGLController;
class IntelContext;
class IntelRequest;

// GuC Firmware versions for Tiger Lake
#define GUC_FIRMWARE_MAJOR 70
#define GUC_FIRMWARE_MINOR 1
#define GUC_FIRMWARE_PATCH 1

// GuC register offsets
#define GUC_STATUS              0xC000
#define GUC_SEND_INTERRUPT      0xC004
#define SOFT_SCRATCH(n)         (0xC180 + (n) * 4)
#define GUC_MAX_MMIO_MSG_LEN    32

// GuC MMIO Base address
#define GUC_MMIO_BASE          0x800000

// G2H (GuC-to-Host) Interrupt registers
#define GUC_HOST_INTERRUPT_ENABLE    0x1C8
#define GUC_HOST_INTERRUPT_CTB       (1 << 0)   // CTB has data
#define GUC_HOST_INTERRUPT_RING_BUFFER (1 << 1) // Ring buffer event
#define GUC_HOST_INTERRUPT_ERROR     (1 << 2)   // Error occurred
#define GUC_HOST_INTERRUPT_WATCHDOG  (1 << 3)   // GuC watchdog

// CTB (Command Transfer Buffer) registers
#define GUC_CTB_ADDRESS_LOW     0x1C0
#define GUC_CTB_ADDRESS_HIGH     0x1C4
#define GUC_CTB_SIZE             0x1C8

// Doorbell registers
#define GUC_DOORBELL_BASE        0x10700
#define GUC_DOORBELL_OFFSET(id)  (GUC_DOORBELL_BASE + (id) * 64)

// H2G Actions for command submission
#define GUC_ACTION_REGISTER_WORKQUEUE     0x4501
#define GUC_ACTION_DEREGISTER_WORKQUEUE   0x4502
#define GUC_ACTION_SETUP_FENCE_BUFFER     0x6020

// GuC WOPCM (Write-Once Power Context Memory) registers
#define GUC_WOPCM_SIZE          0xC050
#define DMA_GUC_WOPCM_OFFSET    0xC340

// GuC communication actions (H2G = Host to GuC)
enum GuCAction {
    // Core actions
    GUC_ACTION_UK_LOG_ENABLE_LOGGING        = 0x0001,
    GUC_ACTION_REQUEST_UPGRADE              = 0x0002,
    GUC_ACTION_UK_MMIO_READ                 = 0x0003,
    GUC_ACTION_REGISTER_CONTEXT             = 0x0004,
    GUC_ACTION_DEREGISTER_CONTEXT           = 0x0005,
    GUC_ACTION_SCHEDULE_CONTEXT             = 0x0006,
    
    // SLPC (Power management)
    GUC_ACTION_SLPC_REQUEST                 = 0x3003,
    
    // Logging
    GUC_ACTION_UK_LOG_ENABLE_LOGGING_V2     = 0x4002,
};

// GuC firmware status
enum GuCFirmwareStatus {
    GUC_STATUS_NO_FIRMWARE      = 0,
    GUC_STATUS_FIRMWARE_PENDING = 1,
    GUC_STATUS_FIRMWARE_FAIL    = 2,
    GUC_STATUS_FIRMWARE_RUNNING = 3,
};

// GuC initialization status
enum GuCInitStatus {
    GUC_INIT_STATUS_NOT_STARTED     = 0,
    GUC_INIT_STATUS_IN_PROGRESS     = 1,
    GUC_INIT_STATUS_WOPCM_CONFIGURED= 2,
    GUC_INIT_STATUS_FIRMWARE_LOADED = 3,
    GUC_INIT_STATUS_READY           = 4,
    GUC_INIT_STATUS_FAILED          = 5,
};

// GuC statistics
struct GuCStats {
    uint64_t h2gMessages;           // Host-to-GuC messages sent
    uint64_t g2hInterrupts;         // GuC-to-Host interrupts received
    uint64_t contextsRegistered;    // Contexts registered with GuC
    uint64_t commandsSubmitted;     // Commands submitted via GuC
    uint64_t doorbellRings;         // Doorbell rings
    uint64_t firmwareReloads;       // Firmware reload count
    uint64_t errors;                // Error count
};

// G2H (GuC-to-Host) message types
enum GuCG2HMessageType {
    GUC_G2H_MSG_CRASH_DUMP_POSTED       = 0x0001,
    GUC_G2H_MSG_REQUEST_COMPLETE        = 0x0002,
    GUC_G2H_MSG_CONTEXT_COMPLETE        = 0x0003,
    GUC_G2H_MSG_ENGINE_RESET            = 0x0004,
    GUC_G2H_MSG_EXCEPTION               = 0x0005,
};

// CTB (Command Transport Buffer) constants
#define GUC_CTB_SIZE_DWORDS     4096    // 16KB buffer
#define GUC_CTB_MSG_MIN_LEN     1
#define GUC_CTB_MSG_MAX_LEN     256
#define GUC_CTB_HDR_LEN         1

// CTB message header format
// [31:16] - length in dwords (including header)
// [15:8]  - flags
// [7:0]   - message type
#define GUC_CTB_MSG_TYPE(hdr)       ((hdr) & 0xFF)
#define GUC_CTB_MSG_FLAGS(hdr)      (((hdr) >> 8) & 0xFF)
#define GUC_CTB_MSG_LEN(hdr)        (((hdr) >> 16) & 0xFFFF)
#define GUC_CTB_MSG_MAKE_HDR(type, len) (((len) << 16) | (type))

// CTB Message Header Layout (Gen12 H2G)
#define GUC_HXG_MSG_0_ORIGIN                 (1U << 31) // 0 = Host, 1 = GuC
#define GUC_HXG_MSG_0_TYPE_SHIFT             28
#define GUC_HXG_MSG_0_TYPE_MASK              (0x7U << 28)
#define GUC_HXG_MSG_0_ACTION_SHIFT           16
#define GUC_HXG_MSG_0_ACTION_MASK            (0xFFFU << 16)
#define GUC_HXG_MSG_0_LEN_SHIFT              0
#define GUC_HXG_MSG_0_LEN_MASK               0x7FFU

// H2G Action Codes
#define INTEL_GUC_ACTION_REGISTER_CONTEXT    0x1000
#define INTEL_GUC_ACTION_DEREGISTER_CONTEXT  0x1001

// CTB buffer descriptor (shared with GuC firmware)
struct GuCCTBDesc {
    uint32_t head;          // Updated by host (for G2H) or GuC (for H2G)
    uint32_t tail;          // Updated by GuC (for G2H) or host (for H2G)
    uint32_t status;        // Error status
    uint32_t reserved;
};

// G2H message structure
struct GuCG2HMessage {
    uint32_t header;        // Type | Flags | Length
    uint32_t data[15];      // Message-specific data
};

// Request completion message data
struct GuCRequestCompleteData {
    uint32_t contextId;     // Context that completed
    uint32_t seqno;         // Sequence number
    uint32_t fenceId;       // Fence to signal
    uint32_t engineId;      // Which engine
};

// CTB buffer (in shared memory)
struct GuCCTBBuffer {
    GuCCTBDesc desc;
    uint32_t messages[GUC_CTB_SIZE_DWORDS];
};

/*
 * IntelGuC Class
 * Manages the Graphics Microcontroller (GuC) firmware and communication
 */
class IntelGuC : public OSObject {
    OSDeclareDefaultStructors(IntelGuC)
    
public:
    // Factory method
    static IntelGuC* withController(AppleIntelTGLController* controller);
    
    // Initialization (Week 38)
    virtual bool init() APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;
    bool initWithController(AppleIntelTGLController* controller);
    bool initializeHardware();
    void shutdown();
    
    // WOPCM Configuration (Write-Once Power Context Memory)
    bool configureWOPCM();
    bool isWOPCMConfigured();
    uint32_t getWOPCMSize();
    uint32_t getWOPCMBase();
    
    // Firmware Loading (Week 38)
    bool loadFirmware();
    bool loadFirmwareBlob(const char* path);
    bool verifyFirmware();
    bool uploadFirmware();
    const char* getFirmwarePath();
    
    // GuC Status
    GuCFirmwareStatus getFirmwareStatus();
    GuCInitStatus getInitStatus();
    bool isReady();
    uint32_t getStatusRegister();
    
    // H2G Communication (Host to GuC)
    bool sendH2GMessage(uint32_t action, uint32_t* data, uint32_t len, uint32_t* response);
    bool sendH2GMessageFast(uint32_t action, uint32_t data0, uint32_t data1);
    
    // G2H Communication (GuC to Host)
    static void handleG2HInterrupt(OSObject* owner, IOInterruptEventSource* source, int count);
    bool processG2HMessage();
    bool processG2HMessages();  // Process all pending messages
    bool handleG2HRequestComplete(GuCG2HMessage* msg);
    bool handleG2HContextComplete(GuCG2HMessage* msg);
    
    // CTB Management
    bool initializeCTB();
    bool initializeCTBCommunication();  // Register CTB with hardware and enable G2H interrupts
    bool allocateCTBBuffer();
    void cleanupCTBBuffer();
    uint32_t readCTBHead();
    uint32_t readCTBTail();
    void writeCTBHead(uint32_t head);
    bool readCTBMessage(uint32_t index, GuCG2HMessage* msg);
    
    // Context Management (Week 39)
    bool registerContext(IntelContext* context);
    bool registerWorkQueue(uint32_t contextId, uint64_t wqPhysicalAddress, uint32_t wqSize);
    bool deregisterContext(IntelContext* context);
    bool scheduleContext(IntelContext* context);
    
    // Command Submission (Week 39)
    bool submitCommand(IntelRequest* request);
    bool ringDoorbell(IntelContext* context, uint32_t doorbellId);
    
    // SLPC - Single Loop Power Control (Week 40)
    bool initializeSLPC();
    bool setSLPCParameter(uint32_t param, uint32_t value);
    bool getSLPCStatus(uint32_t* status);
    
    // Logging (Week 40)
    bool enableLogging();
    bool disableLogging();
    bool captureLog(void* buffer, uint32_t size);
    
    // Statistics
    void getStatistics(GuCStats* stats);
    void resetStatistics();
    
private:
    // Hardware access
    AppleIntelTGLController*    controller;
    bool                    initialized;
    GuCInitStatus          initStatus;
    
    // Firmware
    IOBufferMemoryDescriptor*   firmwareMemory;
    void*                       firmwareData;
    uint32_t                    firmwareSize;
    uint32_t                    firmwareVersion;
    GuCFirmwareStatus          firmwareStatus;
    
    // WOPCM
    uint32_t                wopcmSize;
    uint32_t                wopcmBase;
    bool                    wopcmConfigured;
    
    // Communication
    IOBufferMemoryDescriptor*   sharedMemory;
    IORecursiveLock*            messageLock;
    IOInterruptEventSource*     g2hInterruptSource;
    
    // CTB (Command Transport Buffer)
    IOBufferMemoryDescriptor*   ctbMemory;
    GuCCTBBuffer*               ctbBuffer;       // Virtual address of CTB
    uint64_t                    ctbPhysAddr;     // Physical address for GuC
    uint32_t                    ctbHead;         // Cached head position
    
    // Statistics
    GuCStats                stats;
    
    // Helper methods
    bool waitForStatus(uint32_t mask, uint32_t value, uint32_t timeout_us);
    bool mmioWrite(uint32_t offset, uint32_t value);
    uint32_t mmioRead(uint32_t offset);
    
    // Firmware parsing
    bool parseFirmwareHeader();
    bool validateFirmwareVersion();
    
    // WOPCM helpers
    uint32_t calculateWOPCMSize();
    uint32_t calculateWOPCMOffset();
};

#endif /* INTELGUC_H */

