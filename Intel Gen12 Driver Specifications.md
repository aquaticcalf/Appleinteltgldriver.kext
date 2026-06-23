# **Architectural Specification and Driver Implementation Guide for Intel Gen12 (Tiger Lake) Integrated Graphics**

The transition to the Intel Gen12 architecture, specifically the Xe-LP variant implemented in Tiger Lake processors, marks a profound shift in integrated graphics processing. Developing a direct-to-metal, kernel-mode driver for macOS from scratch demands an exhaustive understanding of the hardware-software interface. This interface spans intricate command stream parsing, multi-level virtual memory management, asynchronous hardware synchronization, and microcontroller-based workload scheduling. Prior legacy mechanisms, such as Host-Based Submission (Execlists) and hardcoded memory caching domains, have been entirely deprecated or heavily abstracted in favor of the Graphics microController (GuC) and Page Attribute Table (PAT) indexing.  
This architectural reference details the exact memory layouts, Command Streamer (CS) packets, the GuC protocol, Per-Process Graphics Translation Table (PPGTT) structures, and memory-based synchronization mechanisms required to bootstrap a Gen12 graphics driver. The specifications provided are derived from the hardware definitions utilized by open-source kernel implementations and architectural reference manuals.

## **1\. Command Packet Structures (Command Streamer and Render Engine)**

The Command Streamer (CS) acts as the primary interface between the software driver and the physical execution engines (Render, Compute, Blitter, and Video). On Gen12 hardware, the CS parses a continuous stream of 32-bit Double Words (DWords) organized into memory-mapped ring buffers and batch buffers. Commands belong to specific structural classes, primarily Memory Interface (MI) commands for stream control, and 3DSTATE commands for fixed-function pipeline configuration.

### **1.1 Memory Interface Batch Buffer Control (MI\_BATCH\_BUFFER\_START and MI\_BATCH\_BUFFER\_END)**

The MI\_BATCH\_BUFFER\_START command is responsible for instructing the CS to suspend instruction fetching from the current ring or batch buffer and begin fetching from a new memory location. In Gen12, this command is strictly three DWords in length. The MI\_BATCH\_BUFFER\_END command signifies the completion of a batch buffer, prompting the hardware to return to the calling buffer (if chained as a second-level batch) or the primary ring buffer.  
When writing a driver, the dispatcher constructs MI\_BATCH\_BUFFER\_START inside the primary Logical Ring Context (LRC) ring buffer, setting the MI\_BATCH\_SECOND\_LEVEL bit if the command should act as a subroutine call rather than a permanent chain.

| Packet / Register | DWord | Bits | Field Name | Description |
| :---- | :---- | :---- | :---- | :---- |
| **MI\_BATCH\_BUFFER\_START** | 0 | 31:29 | Command Type | 0x0 (MI Command) |
|  | 0 | 28:23 | MI Opcode | 0x31 |
|  | 0 | 22 | Second Level Batch | 1 indicates a returnable subroutine call. |
|  | 0 | 8 | Address Space Indicator | 0 \= GGTT, 1 \= PPGTT. |
|  | 0 | 7:0 | DWord Length | Must be set to 0x1 (Total Length 3 \- 2). |
|  | 1 | 31:2 | Batch Address (Lower) | Lower 32 bits of the batch buffer address (must be 4-byte aligned, 4KB recommended). |
|  | 2 | 15:0 | Batch Address (Upper) | Upper 16 bits of the 48-bit address. |
| **MI\_BATCH\_BUFFER\_END** | 0 | 31:23 | MI Opcode | 0x0A (Type 0, Opcode 0x0A). |

Below are the exact C/C++ structure definitions required for marshaling these packets into memory.  
// Magic numbers for Command Streamer Opcodes  
\#define MI\_COMMAND\_OPCODE\_SHIFT          23  
\#define MI\_BATCH\_BUFFER\_START\_OPCODE     (0x31 \<\< MI\_COMMAND\_OPCODE\_SHIFT)  
\#define MI\_BATCH\_BUFFER\_END\_OPCODE       (0x0A \<\< MI\_COMMAND\_OPCODE\_SHIFT)

// Bitfield definitions for MI\_BATCH\_BUFFER\_START  
\#define MI\_BATCH\_ADDRESS\_SPACE\_GGTT      (0 \<\< 8\)  
\#define MI\_BATCH\_ADDRESS\_SPACE\_PPGTT     (1 \<\< 8\)  
\#define MI\_BATCH\_SECOND\_LEVEL            (1 \<\< 22\)

// MI\_BATCH\_BUFFER\_START requires the length minus 2 in the header  
\#define MI\_BATCH\_BUFFER\_START\_LENGTH     (3 \- 2\)

struct gen12\_mi\_batch\_buffer\_start {  
    uint32\_t header;         // Opcode | Second Level Bit | Addr Space | Length  
    uint32\_t address\_lower;  // Lower 32 bits of the batch buffer address  
    uint32\_t address\_upper;  // Upper 16 bits of the batch buffer address  
};

struct gen12\_mi\_batch\_buffer\_end {  
    uint32\_t header;         // MI\_BATCH\_BUFFER\_END\_OPCODE  
};

### **1.2 Synchronization and Cache Management (PIPE\_CONTROL)**

PIPE\_CONTROL is the most critical instruction for hardware synchronization and cache coherency within the Render and Compute engines. It spans six DWords on Gen12 and allows the driver to stall the CS until previous pipeline stages complete, flush internal caches (L3, L1, texture caches), and write an immediate value (fence) to memory.  
To reliably write a hardware fence after a workload, the driver must issue a PIPE\_CONTROL with the Command Streamer Stall (PIPE\_CONTROL\_CS\_STALL) and Post-Sync Write Immediate (PIPE\_CONTROL\_POST\_SYNC\_WRITE\_IMM) bits asserted. On Gen12, to guarantee memory coherency, it is strictly required to also set PIPE\_CONTROL\_RENDER\_TARGET\_FLUSH and PIPE\_CONTROL\_DC\_FLUSH. Without these flushes, pixel or compute data might remain trapped in the GPU's Last Level Cache (LLC) or L3 fabric when the fence sequence number is written, causing the CPU to read stale framebuffer data.

| DWord | Bits | Field Name | Description / Mask |
| :---- | :---- | :---- | :---- |
| **0** | 31:29 | Command Type | 0x3 (GFX Command) |
|  | 28:27 | Command Subtype | 0x3 (3D Command) |
|  | 26:24 | 3D Opcode | 0x2 (PIPE\_CONTROL) |
|  | 7:0 | DWord Length | 0x4 (Total Length 6 \- 2\) |
| **1** | 27 | Render Target Cache Flush | 1 \<\< 27: Flushes the render target cache. |
|  | 26 | Texture Cache Invalidate | 1 \<\< 26: Invalidates read-only texture caches. |
|  | 24 | Global Snapshot Reset | 1 \<\< 24: Required for certain context restorations. |
|  | 23 | TLB Invalidate | 1 \<\< 23: Invalidates Translation Lookaside Buffer. |
|  | 21 | Store Data Index | 1 \<\< 21: Modifies how post-sync data is routed. |
|  | 20 | Command Streamer Stall | 1 \<\< 20: Halts CS parsing until preceding pipeline stages complete. |
|  | 15:14 | Post-Sync Operation | 01b \= Write Immediate Data. 00b \= No Write. |
|  | 9 | Flush Enable | 1 \<\< 9: Global flush enable bit. |
|  | 8 | Notify Enable | 1 \<\< 8: Generates a hardware interrupt upon completion. |
|  | 5 | DC Flush Enable | 1 \<\< 5: Data Cache flush. |
|  | 4 | VF Cache Invalidate | 1 \<\< 4: Vertex Fetch cache invalidate. |
|  | 3 | Constant Cache Invalidate | 1 \<\< 3: Invalidates constant buffers. |
|  | 2 | State Cache Invalidate | 1 \<\< 2: Invalidates dynamic state caches. |
|  | 1 | Stall at Scoreboard | 1 \<\< 1: Hardware execution barrier. |
|  | 0 | Depth Cache Flush | 1 \<\< 0: Flushes hierarchical depth and stencil buffers. |
| **2** | 31:2 | Address (Lower) | Post-sync write address (lower 32 bits, must be QWord aligned). |
|  | 2 | Destination Address Type | 0 \= PPGTT, 1 \= Global GTT. |
| **3** | 15:0 | Address (Upper) | Post-sync write address (upper 16 bits). |
| **4** | 31:0 | Immediate Data (Lower) | Lower 32 bits of the fence value to write. |
| **5** | 31:0 | Immediate Data (Upper) | Upper 32 bits of the fence value to write. |

The C/C++ mappings for generating this synchronization barrier require careful masking to prevent reserved bits from triggering parsing faults.  
\#define GFX\_CMD\_TYPE\_3D                  (3 \<\< 29\)  
\#define GFX\_CMD\_SUBTYPE\_3D               (3 \<\< 27\)  
\#define PIPE\_CONTROL\_OPCODE              (2 \<\< 24\)  
\#define PIPE\_CONTROL\_LENGTH              (6 \- 2\)  
\#define PIPE\_CONTROL\_HEADER              (GFX\_CMD\_TYPE\_3D | GFX\_CMD\_SUBTYPE\_3D | PIPE\_CONTROL\_OPCODE | PIPE\_CONTROL\_LENGTH)

// Post-Sync Operation Definitions (DWord 1, bits 14:15)  
\#define PIPE\_CONTROL\_POST\_SYNC\_NONE      (0 \<\< 14\)  
\#define PIPE\_CONTROL\_POST\_SYNC\_WRITE\_IMM (1 \<\< 14\) // Write immediate data

// Cache Flush and Stall Bits (DWord 1\)  
\#define PIPE\_CONTROL\_DEPTH\_CACHE\_FLUSH           (1 \<\< 0\)  
\#define PIPE\_CONTROL\_STALL\_AT\_SCOREBOARD         (1 \<\< 1\)  
\#define PIPE\_CONTROL\_STATE\_CACHE\_INVALIDATE      (1 \<\< 2\)  
\#define PIPE\_CONTROL\_CONSTANT\_CACHE\_INVALIDATE   (1 \<\< 3\)  
\#define PIPE\_CONTROL\_VF\_CACHE\_INVALIDATE         (1 \<\< 4\)  
\#define PIPE\_CONTROL\_DC\_FLUSH                    (1 \<\< 5\)  
\#define PIPE\_CONTROL\_PROTECTED\_MEMORY\_DISABLE    (1 \<\< 6\)  
\#define PIPE\_CONTROL\_PIPE\_FLUSH                  (1 \<\< 7\)  
\#define PIPE\_CONTROL\_NOTIFY\_ENABLE               (1 \<\< 8\) // Generates a user interrupt  
\#define PIPE\_CONTROL\_FLUSH\_ENABLE                (1 \<\< 9\)  
\#define PIPE\_CONTROL\_CS\_STALL                    (1 \<\< 20\)  
\#define PIPE\_CONTROL\_STORE\_DATA\_INDEX            (1 \<\< 21\)  
\#define PIPE\_CONTROL\_SYNC\_GFDT                   (1 \<\< 22\)  
\#define PIPE\_CONTROL\_TLB\_INVALIDATE              (1 \<\< 23\)  
\#define PIPE\_CONTROL\_GLOBAL\_SNAPSHOT\_RESET       (1 \<\< 24\)  
\#define PIPE\_CONTROL\_TEXTURE\_CACHE\_INVALIDATE    (1 \<\< 26\)  
\#define PIPE\_CONTROL\_RENDER\_TARGET\_FLUSH         (1 \<\< 27\)

// Address Space (DWord 1, bit 2\)  
\#define PIPE\_CONTROL\_GLOBAL\_GTT                  (1 \<\< 2\) 

struct gen12\_pipe\_control {  
    uint32\_t header;         // DWord 0: Instruction header  
    uint32\_t flags;          // DWord 1: Flush bits, Stall bits, Post-sync op  
    uint32\_t address\_lower;  // DWord 2: Lower 32 bits of post-sync address  
    uint32\_t address\_upper;  // DWord 3: Upper 16 bits of post-sync address  
    uint32\_t imm\_data\_lower; // DWord 4: Lower 32 bits of immediate data (fence)  
    uint32\_t imm\_data\_upper; // DWord 5: Upper 32 bits of immediate data  
};

### **1.3 Pipeline Routing (3DSTATE\_PIPELINE\_SELECT)**

This state command instructs the hardware to route subsequent commands to either the 3D (Render) pipeline or the GPGPU (Compute) pipeline. It must be issued prior to dispatching any compute walker or 3D primitive. In Gen12, the hardware strictly enforces pipeline boundaries; attempting to issue a compute command while the 3D pipeline is active results in an execution fault. Furthermore, changing pipelines requires a full pipeline flush via PIPE\_CONTROL prior to the selection to ensure no transient data corrupts the state transition.

| DWord | Bits | Field Name | Description |
| :---- | :---- | :---- | :---- |
| **0** | 31:16 | Command Header | 0x6904 |
|  | 1:0 | Pipeline Selection | 0 \= 3D, 1 \= Media, 2 \= GPGPU |

\#define CMD\_3DSTATE\_PIPELINE\_SELECT      (0x6904 \<\< 16\)  
\#define PIPELINE\_SELECT\_3D               0  
\#define PIPELINE\_SELECT\_MEDIA            1  
\#define PIPELINE\_SELECT\_GPGPU            2

struct gen12\_3dstate\_pipeline\_select {  
    uint32\_t header; // GFX\_CMD\_TYPE\_3D | GFX\_CMD\_SUBTYPE\_3D | CMD\_3DSTATE\_PIPELINE\_SELECT | (length \= 0\)  
};

### **1.4 Compute Dispatch (COMPUTE\_WALKER / GPGPU\_WALKER)**

While earlier generations relied heavily on GPGPU\_WALKER, Gen12 transitions to the highly complex COMPUTE\_WALKER mechanism to dispatch compute shaders. This packet dictates how Thread Groups are spawned across the Execution Units (EUs).  
The COMPUTE\_WALKER inherently requires the driver to calculate Shared Local Memory (SLM) limits and coordinate Thread Group Dispatch. On Tiger Lake, ensuring that local\_x\_max aligns with the SIMD lane size (SIMD8, SIMD16, or SIMD32) compiled into the Instruction Set Architecture (ISA) is paramount for optimum occupancy. A mismatch will cause silent hardware faults or GPU hangs.

| DWord | Bits | Field Name | Description |
| :---- | :---- | :---- | :---- |
| **0** | 31:16 | Command Header | 0x79A0 |
|  | 7:0 | DWord Length | Varies based on post-sync operations (typically 37). |
| **1** | 31:0 | Debug ID | Driver-defined identifier for hardware debugging tools. |
| **2** | 31:0 | Indirect Data Length | Maximum size of indirect parameter data. |
| **3** | 31:6 | Indirect Data Start | Base address for indirect data payload. |
| **4** | 31:0 | Thread Group X | Number of Thread Groups in the X dimension. |
| **5** | 31:0 | Thread Group Y | Number of Thread Groups in the Y dimension. |
| **6** | 31:0 | Thread Group Z | Number of Thread Groups in the Z dimension. |
| **10** | 31:0 | Execution Mask | SIMD execution mask to disable specific lanes. |
| **11-13** | 31:0 | Local Dimensions | Threads per group in X, Y, Z coordinates. |

\#define CMD\_COMPUTE\_WALKER               (0x79A0 \<\< 16\)  
\#define COMPUTE\_WALKER\_LENGTH            (39 \- 2\) // Length varies based on extended fields

struct gen12\_compute\_walker {  
    uint32\_t header;                   // DWord 0: Header and Length  
    uint32\_t debug\_id;                 // DWord 1: Debug string identifier  
    uint32\_t indirect\_data\_length;     // DWord 2: Indirect data limit  
    uint32\_t indirect\_data\_start;      // DWord 3: Pointer to indirect data  
    uint32\_t thread\_group\_x;           // DWord 4: Thread Groups X dimension  
    uint32\_t thread\_group\_y;           // DWord 5: Thread Groups Y dimension  
    uint32\_t thread\_group\_z;           // DWord 6: Thread Groups Z dimension  
    uint32\_t thread\_group\_id\_x;        // DWord 7: Starting X ID  
    uint32\_t thread\_group\_id\_y;        // DWord 8: Starting Y ID  
    uint32\_t thread\_group\_id\_z;        // DWord 9: Starting Z ID  
    uint32\_t execution\_mask;           // DWord 10: SIMD execution mask  
    uint32\_t local\_x\_max;              // DWord 11: Local threads per group X  
    uint32\_t local\_y\_max;              // DWord 12: Local threads per group Y  
    uint32\_t local\_z\_max;              // DWord 13: Local threads per group Z  
    uint32\_t thread\_group\_id\_mapping;  // DWord 14: Thread group mapping control  
    uint32\_t post\_sync\_data\[4\];        // DWord 15-18: Optional post-sync operation  
    // DWords 19+ contain interface descriptors and structural bounds  
};

### **1.5 Vertex Geometry State (3DSTATE\_VERTEX\_BUFFERS and 3DSTATE\_VF\_INSTANCING)**

These packets define the inputs to the Vertex Fetch (VF) stage of the 3D pipeline. 3DSTATE\_VERTEX\_BUFFERS binds memory addresses containing vertex data, while 3DSTATE\_VF\_INSTANCING configures the hardware instancing multiplier.  
For Gen12, memory caching characteristics are tightly controlled by the Memory Object Control State (MOCS) index embedded within the vertex buffer state. Selecting the correct MOCS index ensures the vertex data is fetched directly into the L3 cache, avoiding costly DRAM roundtrips. The driver must upload the 3DSTATE\_VERTEX\_BUFFERS dynamically based on the bound geometry of the draw call.

| Packet | DWord | Bits | Field Name | Description |
| :---- | :---- | :---- | :---- | :---- |
| **3DSTATE\_VERTEX\_BUFFERS** | 0 | 31:16 | Command Header | 0x7808 |
|  | 1 | 31:26 | VB Index | Identifier for the vertex buffer binding. |
|  | 1 | 22:16 | MOCS | Memory Object Control State for L3 caching routing. |
|  | 1 | 11:0 | Buffer Pitch | Byte offset between vertices. |
|  | 2 | 31:0 | Address Lower | Lower 32 bits of physical address. |
|  | 3 | 15:0 | Address Upper | Upper 16 bits of physical address. |
|  | 4 | 31:0 | Size | Total size of the buffer in bytes. |
| **3DSTATE\_VF\_INSTANCING** | 0 | 31:16 | Command Header | 0x7801 |
|  | 1 | 5:0 | Element Index | The vertex element index to apply instancing to. |
|  | 2 | 31:0 | Instancing Step | Step rate (0 \= per vertex, \>0 \= per N instances). |

\#define CMD\_3DSTATE\_VERTEX\_BUFFERS       (0x7808 \<\< 16\)  
\#define CMD\_3DSTATE\_VF\_INSTANCING        (0x7801 \<\< 16\)

// Structure for a single Vertex Buffer state (multiple can be appended)  
struct gen12\_vertex\_buffer\_state {  
    uint32\_t vb\_index\_and\_mocs; // Bits 26:31 \= VB Index, Bits 16:22 \= MOCS   
    uint32\_t buffer\_pitch;      // Bits 0:11 \= Buffer Pitch  
    uint32\_t address\_lower;     // Lower 32 bits of VB address  
    uint32\_t address\_upper;     // Upper 47:32 bits  
    uint32\_t size;              // Size of buffer in bytes  
};

struct gen12\_3dstate\_vf\_instancing {  
    uint32\_t header;            // DWord 0: Header (length \= 1\)  
    uint32\_t element\_index;     // DWord 1: Vertex Element Index  
    uint32\_t instancing\_step;   // DWord 2: Step rate  
};

## **2\. GuC (Graphics microController) H2G Protocol**

The transition from Host-Based Submission (Execlists) to GuC-based submission is fully cemented in Gen12 architectures. The GuC is an internal microcontroller that handles all Context Scheduling, offloading context-switching and workload queuing from the host CPU. Interaction with the GuC occurs via the Host-to-GuC (H2G) protocol using Command Transport Buffers (CTBs) and Memory-Mapped I/O (MMIO) doorbells.  
Because the GuC operates entirely asynchronously, macOS kernel developers must orchestrate communications precisely to avoid system hangs.

### **2.1 GuC MMIO Addresses and Status Polling**

Communication initialization relies on key MMIO registers. On Tiger Lake, the registers are mapped relative to the GT base address.

| Register Name | MMIO Offset | Description |
| :---- | :---- | :---- |
| GUC\_STATUS | 0xC000 | Contains the current operational state of the GuC firmware. |
| GUC\_DOORBELL\_BASE | 0x10700 | The base offset for the doorbell cacheline array. |

During driver initialization, the CPU must poll the GUC\_STATUS register at 0xC000. The CPU checks this 32-bit register to ensure the GuC firmware has booted and is ready to accept commands (specifically checking that the firmware has exited the GS\_MIA\_IN\_RESET state).  
The GuC maintains GUC\_NUM\_DOORBELLS, which corresponds to 256 independent doorbell cachelines. The offset base is typically located at 0x10700 (depending on the exact Tiger Lake stepping), mapped in memory as an array of struct guc\_doorbell\_info.

### **2.2 Command Transport Buffer (CTB) Format**

CTBs are circular ring buffers located in memory shared between the CPU and GuC. The driver writes messages into the Send (H2G) buffer and reads from the Receive (G2H) buffer. Messages are strictly formatted in 32-bit DWords. The protocol uses the highest bit to denote origin, and specific shifts to encode the action requested.

| DWord | Bits | Field Name | Description |
| :---- | :---- | :---- | :---- |
| **0** | 31 | Origin | 0 \= Host, 1 \= GuC. |
|  | 30:28 | Type | Message category type. |
|  | 27:16 | Action | Specific H2G Action Code. |
|  | 10:0 | Length | Length of the payload in DWords. |
| **1+** | 31:0 | Payload | Variable data dependent on the Action Code. |

// CTB Message Header Layout  
\#define GUC\_HXG\_MSG\_0\_ORIGIN                 (1 \<\< 31\) // 0 \= Host, 1 \= GuC  
\#define GUC\_HXG\_MSG\_0\_TYPE\_SHIFT             28  
\#define GUC\_HXG\_MSG\_0\_TYPE\_MASK              (0x7 \<\< 28\)  
\#define GUC\_HXG\_MSG\_0\_ACTION\_SHIFT           16  
\#define GUC\_HXG\_MSG\_0\_ACTION\_MASK            (0xFFF \<\< 16\)  
\#define GUC\_HXG\_MSG\_0\_LEN\_SHIFT              0  
\#define GUC\_HXG\_MSG\_0\_LEN\_MASK               0x7FF

// H2G Action Codes  
\#define INTEL\_GUC\_ACTION\_REGISTER\_CONTEXT    0x1000  
\#define INTEL\_GUC\_ACTION\_DEREGISTER\_CONTEXT  0x1001

struct guc\_ctb\_msg {  
    uint32\_t header;  // HXG Header containing Type, Action, and Length  
    uint32\_t data\[\];  // Variable payload dependent on the Action  
};

### **2.3 Registering a Context Descriptor**

Before the GuC can schedule a logical ring context (LRC), it must be formally registered. The CPU allocates an LRC and assigns it a unique 16-bit Context ID. It then constructs a INTEL\_GUC\_ACTION\_REGISTER\_CONTEXT message.  
The payload requires the exact physical address of the LRC descriptor, which instructs the GuC on how to load the hardware context state into the execution units. Once sent, the CPU updates the CTB tail pointer. The GuC parses the CTB asynchronously and registers the context in its internal schedule queue.  
struct guc\_h2g\_register\_context {  
    uint32\_t header;         // Origin=Host, Action=REGISTER\_CONTEXT, Length=3  
    uint32\_t context\_id;     // 16-bit ID assigned by the driver  
    uint32\_t lrc\_desc\_lower; // Lower 32 bits of LRC descriptor physical address  
    uint32\_t lrc\_desc\_upper; // Upper 32 bits of LRC descriptor  
};

### **2.4 Submitting a Workload (Ringing the Doorbell)**

To submit a batch buffer, the driver does *not* interact directly with the CS ring tail pointer MMIO as it did in legacy Execlists. The doorbell mechanism decouples the CPU from the strict timing requirements of the GPU engine resets. By relying on the GuC to handle engine stalls, hangs, and preemption, the CPU is insulated from transient hardware states.  
The workflow is as follows:

1. The driver appends the MI\_BATCH\_BUFFER\_START packet to the context's memory-mapped ring buffer.  
2. The driver updates the context's local ring tail pointer in system memory.  
3. The driver alerts the GuC that the context has new work by ringing the doorbell.

Ringing the doorbell involves writing the Context ID to the specific 64-byte doorbell cacheline assigned to that context. However, it requires the driver to strictly maintain Shared Virtual Memory coherency; if the doorbell is rung before the ring buffer contents have successfully flushed from CPU caches to main memory, the CS will parse garbage data and hang.  
// Pseudo-code for ringing the GuC doorbell  
void ring\_guc\_doorbell(uint16\_t context\_id, void\* doorbell\_base\_ptr) {  
    // Each doorbell is a 64-byte aligned structure  
    // The first 32-bit integer is written to trigger the interrupt  
    volatile uint32\_t\* doorbell \= (uint32\_t\*)((uintptr\_t)doorbell\_base\_ptr \+ (context\_id \* 64));  
      
    // Write the context ID to trigger the GuC wake-up  
    \*doorbell \= context\_id;  
      
    // Memory barrier to ensure the write reaches the PCIe root complex / internal fabric  
    \_\_sync\_synchronize();   
}

## **3\. PPGTT (Per-Process Graphics Translation Table) Layout**

The Gen12 PPGTT provides rigorous process isolation by implementing a 4-level page table structure that maps a 48-bit GPU virtual address space to physical memory. This is architecturally identical to the x86-64 CPU paging structures, facilitating efficient Shared Virtual Memory (SVM) implementations between the macOS kernel and the GPU execution units.

### **3.1 4-Level Page Table Structure**

The memory management unit (MMU) walks through four tables, each 4KB in size, containing 512 64-bit entries (8 bytes per entry).

| Level | Table Name | Virtual Address Bits Resolved | Description |
| :---- | :---- | :---- | :---- |
| **4** | PML4 (Page Map Level 4\) | 47:39 | The highest level directory. |
| **3** | PDP (Page Directory Pointer) | 38:30 | Points to the Page Directory. |
| **2** | PD (Page Directory) | 29:21 | Can directly map 2MB huge pages or point to PT. |
| **1** | PT (Page Table) | 20:12 | Maps standard 4KB physical pages. |

### **3.2 Page Table Entry (PTE) Bitfield Layout**

The 64-bit Gen12 PTE dictates the physical address mapping and memory access permissions. The physical address in the PTE is precisely shifted and masked. Since physical pages are 4KB aligned, bits 0-11 of the physical address are implicitly zero, which is why the mask is 0x0000FFFFFFFFF000ULL. Any bits written to 0-11 are interpreted as flags by the GPU MMU.

| Bits | Field Name | Description |
| :---- | :---- | :---- |
| **63** | Execute Disable (XD) | 1 \= Prevents instruction fetching from this page. |
| **47:12** | Physical Address | The 4KB-aligned physical memory address. |
| **3:2** | PAT Index | Page Attribute Table index determining cache policy. |
| **1** | Read/Write (R/W) | 0 \= Read Only, 1 \= Read/Write. |
| **0** | Present | 1 \= Valid and resident in memory. |

// Gen12 PTE Bitfield Definitions  
\#define GEN12\_PTE\_PRESENT                (1ULL \<\< 0\)  // Bit 0: Valid/Present flag  
\#define GEN12\_PTE\_RW                     (1ULL \<\< 1\)  // Bit 1: 0 \= Read Only, 1 \= Read/Write  
\#define GEN12\_PTE\_PAT\_INDEX\_SHIFT        2  
\#define GEN12\_PTE\_PAT\_INDEX\_MASK         (0x3ULL \<\< 2\) // Bits 2:3: PAT Index (Cache Control)  
\#define GEN12\_PTE\_PHYS\_ADDR\_MASK         0x0000FFFFFFFFF000ULL // Bits 12:47: Physical Address  
\#define GEN12\_PTE\_XD                     (1ULL \<\< 63\) // Bit 63: eXecute Disable

// Macro to construct a PTE  
inline uint64\_t gen12\_construct\_pte(uint64\_t phys\_addr, uint8\_t pat\_index, bool is\_writeable) {  
    // Mask off the physical address to ensure bits 0-11 are clean  
    uint64\_t pte \= (phys\_addr & GEN12\_PTE\_PHYS\_ADDR\_MASK);  
    pte |= GEN12\_PTE\_PRESENT;  
    if (is\_writeable) {  
        pte |= GEN12\_PTE\_RW;  
    }  
    pte |= ((uint64\_t)(pat\_index & 0x3) \<\< GEN12\_PTE\_PAT\_INDEX\_SHIFT);  
    return pte;  
}

### **3.3 Cache Control and the PAT Index**

In Gen12, the caching strategy is heavily modified from previous generations. Instead of hardcoding memory domains (like Uncached, Write-Combined, Write-Back) globally into the PTE formats, Gen12 relies on the Page Attribute Table (PAT) index located in bits 2:3 of the PTE.  
The driver programs a set of global PAT registers in the GPU MMIO space, mapping index values (0-3) to hardware caching policies. When the GPU MMU walks the PPGTT, it cross-references the PAT index in the PTE against the global table.  
For a macOS driver, establishing explicit Write-Combined (WC) and Write-Back (WB) mappings is critical. Framebuffers and display scanout surfaces must utilize WC indexing to prevent visual artifacting and ensure the display engine immediately sees the finalized render without stale LLC data. Conversely, compute shaders utilizing scratch memory heavily benefit from WB indexing to leverage the GPU's internal caches.

## **4\. Hardware Synchronization (Fences)**

Synchronization between the host CPU and the asynchronous GPU hardware is the cornerstone of a stable graphics driver. Modern Vulkan and Metal APIs rely heavily on "Timeline Fences" which require monotonically increasing 64-bit sequence numbers to determine workload completion.

### **4.1 Fencing Mechanisms on Gen12**

A common point of confusion for new driver developers transitioning from discrete graphics or older architectures is searching for a specific MMIO register that contains the current hardware sequence number.  
**On Gen12, there is no standard MMIO register used to read the current hardware fence sequence number.**  
Instead, synchronization is done exclusively by reading a memory value written by the Command Streamer via a PIPE\_CONTROL command. The sequence is executed as follows:

1. **Memory Allocation:** The driver allocates a coherent memory page, known as the Hardware Status Page (HWSP). This page is mapped to the GPU via the PPGTT and mapped to the CPU as cached memory.  
2. **Command Construction:** The driver appends a PIPE\_CONTROL command to the end of a workload batch.  
3. **Data Payload:** The PIPE\_CONTROL is programmed with PIPE\_CONTROL\_POST\_SYNC\_WRITE\_IMM (Post-Sync Opcode \= 1). The target address points to a specific offset in the HWSP, and the immediate 64-bit data contains the newly incremented sequence number.  
4. **CPU Polling/Interrupt:** The GPU executes the workload, flushes its caches, and writes the 64-bit sequence number directly into RAM. The CPU driver can either spin-poll this RAM location (comparing the memory value to the expected sequence number) or rely on a user-interrupt generated by the PIPE\_CONTROL to wake a sleeping thread.

### **4.2 Implementation of Memory-Based Synchronization**

Relying on memory writes over MMIO polling prevents the CPU from flooding the PCIe bus with register read requests, thereby significantly lowering host latency and power consumption.  
// Constructing the sync packet  
struct gen12\_pipe\_control sync\_flush;  
sync\_flush.header \= PIPE\_CONTROL\_HEADER;

// Stall the CS and flush all preceding write caches to ensure data is globally visible  
sync\_flush.flags \= PIPE\_CONTROL\_CS\_STALL |   
                   PIPE\_CONTROL\_RENDER\_TARGET\_FLUSH |   
                   PIPE\_CONTROL\_DC\_FLUSH |  
                   PIPE\_CONTROL\_POST\_SYNC\_WRITE\_IMM;

// Address within the Hardware Status Page  
sync\_flush.address\_lower \= (uint32\_t)(hwsp\_phys\_addr & 0xFFFFFFFF);  
sync\_flush.address\_upper \= (uint32\_t)(hwsp\_phys\_addr \>\> 32);

// The unique timeline sequence number for this batch  
sync\_flush.imm\_data\_lower \= (uint32\_t)(fence\_seq\_no & 0xFFFFFFFF);  
sync\_flush.imm\_data\_upper \= (uint32\_t)(fence\_seq\_no \>\> 32);

Once the CS reaches this instruction, it will halt fetching new commands until the pixel and compute pipelines are entirely empty. It then bypasses all internal caching to write fence\_seq\_no directly to system memory. The driver's wait function can then safely perform a standard volatile read on the host-mapped hwsp\_phys\_addr.

## **5\. Synthesis and Architectural Implications**

Developing a bespoke kernel driver for the Intel Gen12 (Tiger Lake) architecture demands strict adherence to the hardware's expected pipeline sequencing and memory topologies. The insights derived from analyzing the bare-metal mechanics yield several definitive engineering principles.  
The architectural shift towards unified caching necessitates rigorous flushing discipline. Gen12’s architecture unifies compute and 3D data flows more aggressively than prior generations. As demonstrated in the PIPE\_CONTROL layouts, omitting a CS\_STALL or a Data Cache (DC) flush before a fence write guarantees race conditions, as the GuC may signal workload completion while pixel data remains stranded in the L3 cache.  
Furthermore, microcontroller sovereignty entirely redefines context management. The shift to GuC scheduling requires the host driver to treat the GuC as an autonomous operating system. Driver engineering must focus heavily on maintaining the CTB queues and adhering to the doorbell mechanics without attempting to micromanage the execution ring pointers via MMIO, which leads to immediate hardware hangs.  
Memory management benefits immensely from page table symmetry. The 1-to-1 correlation between the Gen12 4-level PPGTT and standard x86-64 MMU pagetables simplifies memory pinning. By correctly mapping the physical address masks and the PAT indices in the PTE, developers can achieve zero-copy Shared Virtual Memory (SVM) architectures between the macOS kernel and the GPU execution units. Finally, deprecating MMIO polling for hardware sequencing forces drivers into an inherently more efficient memory-based polling model. By designating the Hardware Status Page (HWSP) as the single source of truth for synchronization, drivers avoid expensive PCIe transitions, significantly improving overall system latency and stability.

#### **Works cited**

1\. drivers/gpu/drm/i915/gt/intel\_gpu\_commands.h ... \- GitLab, https://pages.sdu.dk/sdurobotics/linux-kernels/kernel/-/blob/1091c8fce8aa9c5abe1a73acab4bcaf58a729005/drivers/gpu/drm/i915/gt/intel\_gpu\_commands.h 2\. linux/drivers/gpu/drm/i915/gt/uc/intel\_uc.c at master \- GitHub, https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/uc/intel\_uc.c 3\. linux/drivers/gpu/drm/i915/gt/intel\_workarounds.c at master \- GitHub, https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/intel\_workarounds.c 4\. linux/drivers/gpu/drm/i915/i915\_cmd\_parser.c at master \- GitHub, https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/i915\_cmd\_parser.c 5\. linux.git \- Linux kernel, https://git.paulk.fr/projects/linux.git/diff/drivers/gpu/drm/i915/gt/intel\_workarounds.c?h=sunxi/cedrus/jpeg-nv16\&id=5664896ba29e6d8c60b6a73564d0a97d380c0f92\&id2=5429c9dbc9025f9a166f64e22e3a69c94fd5b29b 6\. NootedGreen.kext is on air\! It's a long long road to complete.. \- Page 20 \- Intel | InsanelyMac, https://www.insanelymac.com/forum/topic/362634-nootedgreenkext-is-on-air-its-a-long-long-road-to-complete/page/20/ 7\. i915\_drm.h source code \[linux/include/uapi/drm/i915\_drm.h\] \- Codebrowser, https://codebrowser.dev/linux/linux/include/uapi/drm/i915\_drm.h.html 8\. 万字长文，GPU Render Engine 详细介绍 \- 稀土掘金, https://juejin.cn/post/7234355976458485818 9\. intel\_guc.c source code \[linux/drivers/gpu/drm/i915/gt/uc/intel\_guc.c\] \- Codebrowser, https://codebrowser.dev/linux/linux/drivers/gpu/drm/i915/gt/uc/intel\_guc.c.html 10\. drivers/gpu/drm/i915/gt/uc/intel\_guc\_reg.h ... \- in https://gitlab.sdu.dk, https://gitlab.sdu.dk/sdurobotics/linux-kernels/kernel/-/blob/06abb9f2acf8ed69859bdb157882962b8cb67695/drivers/gpu/drm/i915/gt/uc/intel\_guc\_reg.h 11\. drivers/gpu/drm/i915/intel\_guc\_reg.h ... \- GitLab \- Eclipse Foundation, https://gitlab.eclipse.org/eclipse/oniro-core/linux/-/blob/076d9f965e561de3557c0cf9263b157b1c7380b9/drivers/gpu/drm/i915/intel\_guc\_reg.h 12\. drivers/gpu/drm/i915/gt/uc/intel\_guc\_reg.h \- in https://gitlab.sdu.dk, https://gitlab.sdu.dk/sdurobotics/linux-kernels/kernel/-/blob/stable/drivers/gpu/drm/i915/gt/uc/intel\_guc\_reg.h 13\. drivers/gpu/drm/i915/gt/uc/intel\_guc\_reg.h ... \- GitLab, https://git.whoi.edu/dgiaya/linux/-/blob/f8394f232b1eab649ce2df5c5f15b0e528c92091/drivers/gpu/drm/i915/gt/uc/intel\_guc\_reg.h 14\. How to draw graphics to a high resolution and high refresh rate monitor with vsync? \- Page 3 \- OSDev.org, https://forum.osdev.org/viewtopic.php?t=57979\&start=30 15\. i915 GPU driver development, release and maintenance \- WebThesis, https://webthesis.biblio.polito.it/35459/1/tesi.pdf 16\. DRM Driver uAPI \- The Linux Kernel documentation, https://docs.kernel.org/gpu/driver-uapi.html 17\. DRM Driver uAPI \- The Linux Kernel Archives, https://www.kernel.org/doc/html/v6.8/gpu/driver-uapi.html 18\. drm/i915 Intel GFX Driver \- The Linux Kernel documentation, https://docs.kernel.org/6.12/gpu/i915.html