/*
 * IntelMetalComputePipeline.cpp - Metal Compute Pipeline Implementation
 * Week 44: Compute Pipeline
 * 
 * Complete compute PSO with GPGPU state generation.
 */

#include "IntelMetalComputePipeline.h"
#include "AppleIntelTGLIOAccelerator.h"
#include <IOKit/IOLib.h>

#define super OSObject
OSDefineMetaClassAndStructors(IntelMetalComputePipeline, OSObject)

// Intel GPU compute state command sizes (Gen12+)
#define GPU_MEDIA_VFE_STATE_SIZE           72   // MEDIA_VFE_STATE (9 dwords)
#define GPU_INTERFACE_DESCRIPTOR_SIZE      64   // MEDIA_INTERFACE_DESCRIPTOR_LOAD (8 dwords)
#define GPU_THREAD_GROUP_CONFIG_SIZE       32   // Thread group configuration
#define GPU_COMPUTE_STATE_TOTAL_SIZE       (GPU_MEDIA_VFE_STATE_SIZE + \
                                            GPU_INTERFACE_DESCRIPTOR_SIZE + \
                                            GPU_THREAD_GROUP_CONFIG_SIZE)

// Intel GPU limits (Gen12+)
#define MAX_THREADS_PER_THREADGROUP        1024
#define MAX_THREADGROUP_MEMORY             65536  // 64 KB
#define MAX_SUBSLICES                      8


// MARK: - Factory & Lifecycle


IntelMetalComputePipeline* IntelMetalComputePipeline::withDescriptor(
    IntelIOAccelerator* accel,
    const MetalComputePipelineDescriptor* desc)
{
    if (!accel || !desc) {
        IOLog("IntelMetalComputePipeline: ERROR - Invalid parameters\n");
        return NULL;
    }
    
    IntelMetalComputePipeline* pipeline = new IntelMetalComputePipeline;
    if (!pipeline) {
        return NULL;
    }
    
    if (!pipeline->initWithDescriptor(accel, desc)) {
        pipeline->release();
        return NULL;
    }
    
    return pipeline;
}

IntelMetalComputePipeline* IntelMetalComputePipeline::withFunction(
    IntelIOAccelerator* accel,
    const MetalComputeFunctionDescriptor* desc)
{
    if (!accel || !desc) {
        IOLog("IntelMetalComputePipeline: ERROR - Invalid parameters\n");
        return NULL;
    }
    
    // Convert function descriptor to pipeline descriptor
    MetalComputePipelineDescriptor pipelineDesc = {};
    pipelineDesc.computeShader = desc->computeShader;
    pipelineDesc.label = desc->functionName;
    pipelineDesc.maxTotalThreadsPerThreadgroup = desc->maxThreadsPerThreadgroup;
    pipelineDesc.threadgroupMemoryLength = desc->threadgroupMemoryLength;
    pipelineDesc.threadExecutionWidth = desc->threadExecutionWidth;
    pipelineDesc.supportIndirectCommandBuffers = desc->supportIndirectDispatch;
    pipelineDesc.enableBarriers = true;
    
    return withDescriptor(accel, &pipelineDesc);
}

bool IntelMetalComputePipeline::initWithDescriptor(
    IntelIOAccelerator* accel,
    const MetalComputePipelineDescriptor* desc)
{
    if (!super::init()) {
        return false;
    }
    
    if (!accel || !desc) {
        return false;
    }
    
    // Validate descriptor
    IOReturn ret = validateDescriptor(desc);
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalComputePipeline: ERROR - Invalid descriptor\n");
        return false;
    }
    
    // Store accelerator
    accelerator = accel;
    accelerator->retain();
    
    // Store compute shader
    computeShader = desc->computeShader;
    if (computeShader) {
        computeShader->retain();
    }
    
    // Store label
    if (desc->label) {
        strncpy(label, desc->label, 63);
        label[63] = '\0';
    } else {
        strncpy(label, "ComputePipeline", 63);
        label[63] = '\0';
    }
    
    // Store configuration
    maxThreadsPerThreadgroup = desc->maxTotalThreadsPerThreadgroup;
    if (maxThreadsPerThreadgroup == 0) {
        maxThreadsPerThreadgroup = MAX_THREADS_PER_THREADGROUP;
    }
    
    threadgroupMemoryLength = desc->threadgroupMemoryLength;
    if (threadgroupMemoryLength > MAX_THREADGROUP_MEMORY) {
        IOLog("IntelMetalComputePipeline: WARNING - Clamping threadgroup memory from %u to %u bytes\n",
              threadgroupMemoryLength, MAX_THREADGROUP_MEMORY);
        threadgroupMemoryLength = MAX_THREADGROUP_MEMORY;
    }
    
    threadExecutionWidth = desc->threadExecutionWidth;
    supportIndirect = desc->supportIndirectCommandBuffers;
    enableBarriers = desc->enableBarriers;
    
    // Initialize state
    gpuState = NULL;
    gpuStateSize = 0;
    memset(&stats, 0, sizeof(stats));
    initialized = true;
    compiled = false;
    
    IOLog("IntelMetalComputePipeline: OK  Compute pipeline initialized\n");
    IOLog("IntelMetalComputePipeline:   Label: %s\n", label);
    IOLog("IntelMetalComputePipeline:   Max threads per threadgroup: %u\n",
          maxThreadsPerThreadgroup);
    IOLog("IntelMetalComputePipeline:   Threadgroup memory: %u bytes\n",
          threadgroupMemoryLength);
    IOLog("IntelMetalComputePipeline:   Thread execution width: %u\n",
          threadExecutionWidth);
    IOLog("IntelMetalComputePipeline:   Indirect support: %s\n",
          supportIndirect ? "Yes" : "No");
    IOLog("IntelMetalComputePipeline:   Barriers: %s\n",
          enableBarriers ? "Enabled" : "Disabled");
    
    // Compile shader and generate GPU state
    ret = compileShader();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalComputePipeline: ERROR - Shader compilation failed\n");
        return false;
    }
    
    ret = generateGPUState();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalComputePipeline: ERROR - GPU state generation failed\n");
        return false;
    }
    
    compiled = true;
    
    return true;
}

void IntelMetalComputePipeline::free() {
    OSSafeReleaseNULL(computeShader);
    OSSafeReleaseNULL(gpuState);
    OSSafeReleaseNULL(accelerator);
    
    super::free();
}


// MARK: - Statistics


void IntelMetalComputePipeline::getStatistics(ComputePipelineStatistics* outStats) {
    if (outStats) {
        memcpy(outStats, &stats, sizeof(ComputePipelineStatistics));
    }
}

void IntelMetalComputePipeline::recordDispatch(
    uint32_t threadgroupsX, uint32_t threadgroupsY, uint32_t threadgroupsZ,
    uint32_t threadsPerGroupX, uint32_t threadsPerGroupY, uint32_t threadsPerGroupZ)
{
    stats.dispatchCount++;
    
    uint32_t totalThreadgroups = threadgroupsX * threadgroupsY * threadgroupsZ;
    stats.threadgroupsDispatched += totalThreadgroups;
    
    uint32_t threadsPerGroup = threadsPerGroupX * threadsPerGroupY * threadsPerGroupZ;
    stats.threadsExecuted += totalThreadgroups * threadsPerGroup;
    
    stats.localMemoryUsage = threadgroupMemoryLength;
}

void IntelMetalComputePipeline::recordBarrier() {
    stats.barrierCount++;
}


// MARK: - GPU State Generation


IOReturn IntelMetalComputePipeline::generateGPUState() {
    if (!initialized) {
        return kIOReturnNotReady;
    }
    
    IOLog("IntelMetalComputePipeline: GENERATING GPU STATE\n");
    
    // Allocate GPU state buffer
    gpuStateSize = GPU_COMPUTE_STATE_TOTAL_SIZE;
    gpuState = IOBufferMemoryDescriptor::withCapacity(gpuStateSize, kIODirectionOut);
    if (!gpuState) {
        IOLog("IntelMetalComputePipeline: ERROR - Failed to allocate GPU state\n");
        return kIOReturnNoMemory;
    }
    
    void* stateBuffer = gpuState->getBytesNoCopy();
    memset(stateBuffer, 0, gpuStateSize);
    
    IOReturn ret;
    
    // Generate MEDIA_VFE_STATE
    IOLog("IntelMetalComputePipeline: [1/3] Generating MEDIA_VFE_STATE...\n");
    ret = generateMediaVFEState();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalComputePipeline: ERROR - MEDIA_VFE_STATE generation failed\n");
        return ret;
    }
    
    // Generate interface descriptor
    IOLog("IntelMetalComputePipeline: [2/3] Generating interface descriptor...\n");
    ret = generateInterfaceDescriptor();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalComputePipeline: ERROR - Interface descriptor generation failed\n");
        return ret;
    }
    
    // Generate thread group configuration
    IOLog("IntelMetalComputePipeline: [3/3] Generating thread group config...\n");
    ret = generateThreadGroupConfig();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalComputePipeline: ERROR - Thread group config generation failed\n");
        return ret;
    }
    
    IOLog("IntelMetalComputePipeline: OK  GPU STATE GENERATION COMPLETE\n");
    IOLog("IntelMetalComputePipeline:   Total state size: %u bytes\n", gpuStateSize);
    
    return kIOReturnSuccess;
}


// MARK: - Internal Methods


IOReturn IntelMetalComputePipeline::validateDescriptor(
    const MetalComputePipelineDescriptor* desc)
{
    // Validate compute shader
    if (!desc->computeShader) {
        IOLog("IntelMetalComputePipeline: ERROR - Missing compute shader\n");
        return kIOReturnBadArgument;
    }
    
    // Validate shader type
    if (desc->computeShader->getType() != kMetalShaderTypeKernel) {
        IOLog("IntelMetalComputePipeline: ERROR - Shader is not a compute kernel\n");
        return kIOReturnBadArgument;
    }
    
    // Validate thread group size
    if (desc->maxTotalThreadsPerThreadgroup > MAX_THREADS_PER_THREADGROUP) {
        IOLog("IntelMetalComputePipeline: ERROR - Thread group size %u exceeds maximum %u\n",
              desc->maxTotalThreadsPerThreadgroup, MAX_THREADS_PER_THREADGROUP);
        return kIOReturnBadArgument;
    }
    
    // Validate threadgroup memory
    if (desc->threadgroupMemoryLength > MAX_THREADGROUP_MEMORY) {
        IOLog("IntelMetalComputePipeline: WARNING - Threadgroup memory %u exceeds recommended %u\n",
              desc->threadgroupMemoryLength, MAX_THREADGROUP_MEMORY);
    }
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalComputePipeline::compileShader() {
    IOLog("IntelMetalComputePipeline: Compiling compute shader...\n");
    
    if (!computeShader) {
        return kIOReturnBadArgument;
    }
    
    if (!computeShader->isCompiled()) {
        IOReturn ret = computeShader->compile();
        if (ret != kIOReturnSuccess) {
            IOLog("IntelMetalComputePipeline: ERROR - Compute shader compilation failed\n");
            return ret;
        }
    }
    
    IOLog("IntelMetalComputePipeline:   OK  Compute shader compiled\n");
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalComputePipeline::generateMediaVFEState() {
    // In real implementation, would generate MEDIA_VFE_STATE command:
    // DWord 0: Command opcode (0x7000)
    // DWord 1: Scratch space base pointer
    // DWord 2: Maximum number of threads
    // DWord 3: Number of URB entries
    // DWord 4: CURBE allocation size
    // DWord 5: Scoreboard mask
    // DWord 6-8: Scoreboard configuration
    
    uint8_t* stateBuffer = (uint8_t*)gpuState->getBytesNoCopy();
    uint32_t offset = 0;
    
    if (offset + GPU_MEDIA_VFE_STATE_SIZE <= gpuStateSize) {
        uint32_t* vfeState = (uint32_t*)(stateBuffer + offset);
        
        // DWord 0: Command header
        vfeState[0] = 0x70000000;  // MEDIA_VFE_STATE opcode
        vfeState[0] |= (9 - 2);    // DWord count - 2
        
        // DWord 1: Scratch space (0 for now)
        vfeState[1] = 0;
        
        // DWord 2: Maximum threads
        uint32_t maxThreads = maxThreadsPerThreadgroup;
        vfeState[2] = maxThreads;
        
        // DWord 3: URB entries (unified return buffer)
        vfeState[3] = 64;  // Number of URB entries
        
        // DWord 4: CURBE allocation (constant URB entry)
        uint32_t curbeSize = (threadgroupMemoryLength + 31) / 32;  // In 256-bit units
        vfeState[4] = curbeSize;
        
        // DWord 5: Scoreboard mask (thread dependency)
        vfeState[5] = 0xFF;  // All 8 dependency checks enabled
        
        // DWord 6-8: Scoreboard deltas (thread synchronization)
        vfeState[6] = 0x00010001;  // Delta X/Y for dependency 0-1
        vfeState[7] = 0x00010001;  // Delta X/Y for dependency 2-3
        vfeState[8] = 0x00010001;  // Delta X/Y for dependency 4-5
    }
    
    IOLog("IntelMetalComputePipeline:   OK  MEDIA_VFE_STATE (%u threads, %u bytes SLM)\n",
          maxThreadsPerThreadgroup, threadgroupMemoryLength);
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalComputePipeline::generateInterfaceDescriptor() {
    // In real implementation, would generate MEDIA_INTERFACE_DESCRIPTOR:






    
    uint8_t* stateBuffer = (uint8_t*)gpuState->getBytesNoCopy();
    uint32_t offset = GPU_MEDIA_VFE_STATE_SIZE;
    
    if (offset + GPU_INTERFACE_DESCRIPTOR_SIZE <= gpuStateSize) {
        uint32_t* descriptor = (uint32_t*)(stateBuffer + offset);
        
        // DWord 0: Kernel start pointer (low 32 bits)
        const void* kernel = computeShader->getKernel();
        if (kernel) {
            descriptor[0] = (uint32_t)((uintptr_t)kernel & 0xFFFFFFFF);
        }
        
        // DWord 1: Kernel start pointer (high 16 bits) + flags
        descriptor[1] = (uint32_t)(((uintptr_t)kernel >> 32) & 0xFFFF);
        if (enableBarriers) {
            descriptor[1] |= (1 << 21);  // Barrier enable
        }
        
        // DWord 2: Binding table pointer
        descriptor[2] = 0;  // Set at dispatch time
        
        // DWord 3: Sampler state pointer
        descriptor[3] = 0;  // Set at dispatch time
        
        // DWord 4: Thread group dimensions
        descriptor[4] = maxThreadsPerThreadgroup;
        
        // DWord 5: Shared local memory size
        // SLM size is encoded as log2(size in KB)
        uint32_t slmKB = (threadgroupMemoryLength + 1023) / 1024;
        uint32_t slmLog2 = 0;
        if (slmKB > 0) {
            slmLog2 = 32 - __builtin_clz(slmKB - 1);
        }
        descriptor[5] = slmLog2;
        
        // DWord 6: Number of threads in thread group
        descriptor[6] = maxThreadsPerThreadgroup;
        
        // DWord 7: Cross-thread constant data length
        descriptor[7] = 0;
    }
    
    IOLog("IntelMetalComputePipeline:   OK  Interface descriptor (kernel @ %p)\n",
          computeShader->getKernel());
    
    return kIOReturnSuccess;
}

IOReturn IntelMetalComputePipeline::generateThreadGroupConfig() {
    // Store thread group configuration for dispatch
    
    uint8_t* stateBuffer = (uint8_t*)gpuState->getBytesNoCopy();
    uint32_t offset = GPU_MEDIA_VFE_STATE_SIZE + GPU_INTERFACE_DESCRIPTOR_SIZE;
    
    if (offset + GPU_THREAD_GROUP_CONFIG_SIZE <= gpuStateSize) {
        uint32_t* config = (uint32_t*)(stateBuffer + offset);
        
        // Thread execution width
        config[0] = threadExecutionWidth;
        
        // Maximum threads per thread group
        config[1] = maxThreadsPerThreadgroup;
        
        // Threadgroup memory length
        config[2] = threadgroupMemoryLength;
        
        // Feature flags
        config[3] = 0;
        if (supportIndirect) {
            config[3] |= (1 << 0);
        }
        if (enableBarriers) {
            config[3] |= (1 << 1);
        }
    }
    
    IOLog("IntelMetalComputePipeline:   OK  Thread group config (width: %u)\n",
          threadExecutionWidth);
    
    return kIOReturnSuccess;
}
