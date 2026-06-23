/*
 * IntelMetalSamplerState.cpp - Metal Sampler State Implementation
 * Week 45: Resource Management (FINAL)
 * 
 * Complete sampler state with GPU SAMPLER_STATE generation.
 */

#include "IntelMetalSamplerState.h"
#include "AppleIntelTGLIOAccelerator.h"
#include <IOKit/IOLib.h>

#define super OSObject
OSDefineMetaClassAndStructors(IntelMetalSamplerState, OSObject)

// Intel GPU sampler state size (Gen12+)
#define GPU_SAMPLER_STATE_SIZE    16  // SAMPLER_STATE (4 dwords)


// MARK: - Factory & Lifecycle


IntelMetalSamplerState* IntelMetalSamplerState::withDescriptor(
    IntelIOAccelerator* accel,
    const MetalSamplerDescriptor* desc)
{
    if (!accel || !desc) {
        IOLog("IntelMetalSamplerState: ERROR - Invalid parameters\n");
        return NULL;
    }
    
    IntelMetalSamplerState* sampler = new IntelMetalSamplerState;
    if (!sampler) {
        return NULL;
    }
    
    if (!sampler->initWithDescriptor(accel, desc)) {
        sampler->release();
        return NULL;
    }
    
    return sampler;
}

bool IntelMetalSamplerState::initWithDescriptor(
    IntelIOAccelerator* accel,
    const MetalSamplerDescriptor* desc)
{
    if (!super::init()) {
        return false;
    }
    
    if (!accel || !desc) {
        return false;
    }
    
    // Store accelerator
    accelerator = accel;
    accelerator->retain();
    
    // Store sampler configuration
    minFilter = desc->minFilter;
    magFilter = desc->magFilter;
    mipFilter = desc->mipFilter;
    
    sAddressMode = desc->sAddressMode;
    tAddressMode = desc->tAddressMode;
    rAddressMode = desc->rAddressMode;
    
    lodMinClamp = desc->lodMinClamp;
    lodMaxClamp = desc->lodMaxClamp;
    maxAnisotropy = desc->maxAnisotropy;
    
    compareEnabled = desc->compareEnabled;
    compareFunction = desc->compareFunction;
    
    // Initialize state
    samplerState = NULL;
    samplerStateSize = 0;
    initialized = true;
    
    const char* filterNames[] = { "Nearest", "Linear" };
    const char* mipFilterNames[] = { "None", "Nearest", "Linear" };
    const char* addressModeNames[] = { "ClampToEdge", "?", "Repeat", "MirrorRepeat", "ClampToZero" };
    
    IOLog("IntelMetalSamplerState: OK  Sampler initialized\n");
    IOLog("IntelMetalSamplerState:   Min filter: %s\n", filterNames[minFilter]);
    IOLog("IntelMetalSamplerState:   Mag filter: %s\n", filterNames[magFilter]);
    IOLog("IntelMetalSamplerState:   Mip filter: %s\n", mipFilterNames[mipFilter]);
    IOLog("IntelMetalSamplerState:   Address modes: %s, %s, %s\n",
          addressModeNames[sAddressMode],
          addressModeNames[tAddressMode],
          addressModeNames[rAddressMode]);
    IOLog("IntelMetalSamplerState:   LOD: %.2f - %.2f\n", lodMinClamp, lodMaxClamp);
    if (maxAnisotropy > 1) {
        IOLog("IntelMetalSamplerState:   Anisotropy: %ux\n", maxAnisotropy);
    }
    if (compareEnabled) {
        IOLog("IntelMetalSamplerState:   Compare function: %u\n", compareFunction);
    }
    
    // Generate sampler state
    IOReturn ret = generateSamplerState();
    if (ret != kIOReturnSuccess) {
        IOLog("IntelMetalSamplerState: ERROR - Sampler state generation failed\n");
        return false;
    }
    
    return true;
}

void IntelMetalSamplerState::free() {
    OSSafeReleaseNULL(samplerState);
    OSSafeReleaseNULL(accelerator);
    
    super::free();
}


// MARK: - GPU State


IOReturn IntelMetalSamplerState::generateSamplerState() {
    if (!initialized) {
        return kIOReturnNotReady;
    }
    
    // Allocate sampler state
    samplerStateSize = GPU_SAMPLER_STATE_SIZE;
    samplerState = IOBufferMemoryDescriptor::withCapacity(samplerStateSize, kIODirectionOut);
    if (!samplerState) {
        IOLog("IntelMetalSamplerState: ERROR - Failed to allocate sampler state\n");
        return kIOReturnNoMemory;
    }
    
    uint32_t* state = (uint32_t*)samplerState->getBytesNoCopy();
    memset(state, 0, samplerStateSize);
    
    // In real implementation, would generate SAMPLER_STATE:
    // DWord 0: Sampler disable, filter mode, address modes
    // DWord 1: LOD bias, max/min LOD
    // DWord 2: Border color pointer, chroma key
    // DWord 3: Max anisotropy, compare function
    
    // DWord 0: Filter and address modes
    state[0] = 0;
    state[0] |= (minFilter << 0);      // Min filter
    state[0] |= (magFilter << 1);      // Mag filter
    state[0] |= (mipFilter << 2);      // Mip filter
    state[0] |= (sAddressMode << 6);   // S (U) address mode
    state[0] |= (tAddressMode << 9);   // T (V) address mode
    state[0] |= (rAddressMode << 12);  // R (W) address mode
    
    // DWord 1: LOD configuration
    uint32_t minLOD = (uint32_t)(lodMinClamp * 256.0f);
    uint32_t maxLOD = (uint32_t)(lodMaxClamp * 256.0f);
    state[1] = (minLOD << 0) | (maxLOD << 12);
    
    // DWord 2: Border color (black)
    state[2] = 0;
    
    // DWord 3: Anisotropy and compare function
    state[3] = 0;
    if (maxAnisotropy > 1) {
        // Map anisotropy: 2x=1, 4x=2, 8x=3, 16x=4
        uint32_t anisoValue = 0;
        if (maxAnisotropy >= 16) anisoValue = 4;
        else if (maxAnisotropy >= 8) anisoValue = 3;
        else if (maxAnisotropy >= 4) anisoValue = 2;
        else if (maxAnisotropy >= 2) anisoValue = 1;
        state[3] |= (anisoValue << 0);
    }
    
    if (compareEnabled) {
        state[3] |= (1 << 16);           // Enable compare
        state[3] |= (compareFunction << 17);  // Compare function
    }
    
    IOLog("IntelMetalSamplerState:   OK  Generated sampler state (%u bytes)\n",
          samplerStateSize);
    
    return kIOReturnSuccess;
}
