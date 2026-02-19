# Uros

Uros is a multiserver operating system based on the OSF variant of Mach, OSF Mach (osfmk).

The goal is to build a modern, multiserver OS on top of the osfmk microkernel, originally developed by the Open Software Foundation (OSF) as part of the MkLinux DR3 project.

## Project Status

This project is in early development. The current focus is on modernizing the legacy codebase and porting the build system from the original ODE/Makefiles to CMake/Ninja.
Compiler used: gcc 15.2.1

## What Has Been Done

### Build System
- New CMake/Ninja build system replacing the legacy ODE/Makefiles
- Export directory layout with platform-specific includes (`osfmk/export/powermac/include/`) (the name of this path will be changed or adapted o the platform in the future)
- Build support for cross-compilation targets

### MIG Compiler (migcom)
- Ported migcom (Mach Interface Generator compiler) to build with modern Flex/Bison
- Fixed parser bug: MIG keywords (e.g. `reply_port`, `sreplyport`) can now be used as argument names in `.defs` files
- `device.h` is now generated at build time instead of being pre-built

### Libraries
- **libmach**: builds successfully (112 compilation units)
- **libcthreads**: fixed build (ANSI C modernization, architecture-specific thread support)
- **libsa_mach**: replaced generated machine wrappers with static headers

### Code Modernization
- Converted old-style K&R function declarations to ANSI C prototypes
- Fixed duplicate return types and implicit declarations
- Renamed `.s` assembly files to `.S` for preprocessor support

## TODO

### Short Term
- [ ] Fix `mach/machine` symlink (currently a file instead of a symlink to `ppc/`, however ppc suport actually is not planned)
- [ ] Fix `#endif` warnings in `.defs` files (`std_types.defs`, `mach_types.defs`, `device_types.defs`)
- [ ] Investigate `cthread_filter.h` in export directory (possibly misplaced)
- [ ] Build remaining user-space libraries (libflipc, librthreads, libmach_maxonstack)
- [ ] Generate all MIG stubs from `.defs` files at build time

### Medium Term
- [ ] Build the osfmk microkernel with CMake/Ninja (Actually the kernel builds and boot. It should be tested with the bootstrap server and see where it fails.)
- [ ] Port kernel bootstrap (bootstrap task)
- [ ] Port default pager
- [ ] Implement basic device drivers for target platform

### Long Term
- [ ] Design and implement multiserver architecture
- [ ] Implement file server
- [ ] Implement network server
- [ ] Implement process server
- [ ] Self-hosting capability

## Project Structure

```
osfmk/
├── export/powermac/include/   # Exported headers for user-space
├── src/
│   ├── mach_kernel/           # Microkernel source
│   ├── mach_services/
│   │   ├── lib/
│   │   │   ├── libmach/       # Core Mach user-space library
│   │   │   ├── libcthreads/   # C threads library
│   │   │   ├── libsa_mach/    # Standalone Mach library
│   │   │   ├── migcom/        # MIG compiler (Flex/Bison)
│   │   │   └── ...
│   │   └── include/           # Shared headers
│   └── ...
scripts/
└── mig                        # MIG wrapper script (cpp + migcom)
```

## Building

Requirements: CMake, Ninja, GCC (cross-compiler for PowerPC), Flex, Bison.

```sh
cmake -B build -G Ninja
ninja -C build
```

## Origins

This project is based on the osfmk code from MkLinux DR3, originally developed by the Open Software Foundation (OSF) and Carnegie Mellon University (CMU). The original code is licensed under the OSF and CMU open source licenses included in the source files.

## License

The original osfmk code retains its OSF/CMU licenses as stated in each source file.
