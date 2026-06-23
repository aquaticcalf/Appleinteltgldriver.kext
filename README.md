# AppleIntelTGL

Intel Tiger Lake (Gen12 / Xe-LP) Graphics Driver for macOS

## Overview

AppleIntelTGL is a direct-to-metal, kernel-mode graphics driver designed for Intel Tiger Lake (Gen12) integrated GPUs on macOS. This driver completely implements the Gen12 architectural shifts, bypassing legacy Execlist models in favor of GuC (Graphics microController) based command submission and multi-level PPGTT memory management.

## Features

- **Gen12 Command Submission**: Hardware-accelerated rendering utilizing the modern Gen12 GuC (Graphics Microcontroller) H2G protocol, replacing legacy Ring Tail/Execlist MMIO submissions.
- **Metal Framework Integration**: Fully functional Command Streamer execution! Translates Metal workloads directly into Gen12 Instruction Set Architecture (COMPUTE_WALKER, PIPE_CONTROL barriers, 3DSTATE commands) and queues them to the execution rings via the GuC.
- **Advanced Memory Management**: Features a complete Gen12 4-level Page Table (PPGTT) implementation with PAT caching optimizations (Write-Combined/Write-Back routing for Shared Virtual Memory coherence).
- **Drawable Surface Tracking**: Deep IOAccelerator functionality, managing displayable and global IOSurfaces across the OS window server.
- **Display Support**: Framebuffer, mode setting, and DP (DisplayPort) integration via the IntelDisplayPipeClient.
- **Power Management**: Runtime PM and GT power management.

## System Requirements

- macOS (10.15+)
- Intel Tiger Lake GPU (Gen12 / Xe-LP)
- Supported PCI Device Match: `8086:9a49` (Device IDs: `0x9a40`-`0x9a78`)

## Project Structure

```
AppleIntelTGLController/
├── AppleIntelTGLController.cpp/h    # Main driver controller and PCI bridge
├── AppleIntelTGLEGLFramebuffer.cpp/h # Framebuffer service
├── AppleIntelTGLIOAccelerator.cpp/h # IOAccelerator for Metal and WindowServer integration
├── AppleIntelTGLIOAcceleratorClients.cpp/h # Implementation of Context, Surface, and Display user clients
├── AppleIntelTGLIOSurfaceManager.cpp/h # Fast IOSurface routing and caching
├── IntelGuCSubmission.cpp/h         # Hardware ring and GuC CTB interaction
├── IntelMetalCommandTranslator.cpp/h # Metal API to Gen12 ISA compiler
└── Info.plist                       # KEXT configuration
```

## Building

1. Ensure you have Xcode installed and the Command Line Tools set up.
2. Open `AppleIntelTGLController.xcodeproj` in Xcode.
3. Select your desired build configuration (`Release` is recommended for stable performance).
4. Build the project (Product -> Build).
5. The compiled `.kext` bundle will be located in your derived data / products folder.

## Installation & Loading

1. Build the kext as detailed above.
2. Disable SIP (System Integrity Protection) if you are testing unsigned kernel extensions.
3. Copy the compiled bundle to `/Library/Extensions/`:
   ```bash
   sudo cp -R /path/to/AppleIntelTGLController.kext /Library/Extensions/
   ```
4. Fix the ownership and permissions on the kext:
   ```bash
   sudo chown -R root:wheel /Library/Extensions/AppleIntelTGLController.kext
   sudo chmod -R 755 /Library/Extensions/AppleIntelTGLController.kext
   ```
5. Rebuild the system kernel cache so macOS recognizes the new driver module:
   ```bash
   sudo kextcache -i /
   ```
6. Reboot your machine.

## Testing & Validation

Once the system has booted, you can test the driver implementation:

1. **Verify Kext Loading**: Open Terminal and run the following command to verify if the kernel successfully loaded the module:
   ```bash
   kextstat | grep AppleIntelTGL
   ```
2. **Review Kernel Logs**: To verify that the hardware initialization, GuC boot, and Metal acceleration contexts are engaging, monitor the kernel logs:
   ```bash
   log show --predicate 'process == "kernel"' --last 10m | grep TGL
   ```
   *Note: You should see logs indicating `[TGL][GLDrawableClient] create_drawable` and `IntelGuCSubmission: OK Submitted request with fence` when window server UI elements load.*
3. **Metal Validation**: You can test Metal compatibility by launching any lightweight Metal benchmarking app, or using macOS's built-in tools like OpenGL Extensions Viewer / Metal Caps.
   Ensure that there are no panics or visual artifacting, which would indicate improper PPGTT mapping or missed `PIPE_CONTROL` flushes.

## License

This project is provided for educational and development purposes.
