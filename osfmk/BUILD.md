# OSFMK Build Instructions

This document describes how to build the OSFMK project using CMake presets.

## Requirements

- **CMake** 3.21 or later
- **Ninja** build system (recommended) or Make
- **GCC** or **Clang** compiler
- **Flex** and **Bison** for lexer/parser generation

### Installing dependencies (Arch Linux)

```bash
sudo pacman -S cmake ninja gcc flex bison
```

### Installing dependencies (Debian/Ubuntu)

```bash
sudo apt install cmake ninja-build gcc flex bison
```

---

## Available Build Presets

| Preset | Description | Use Case |
|--------|-------------|----------|
| `debug` | Debug with ASAN + UBSAN | Catching memory errors and undefined behavior |
| `debug-nosan` | Debug without sanitizers | GDB/Valgrind debugging, stepping through code |
| `release` | Optimized release build | Production use |
| `relwithdebinfo` | Release with debug symbols | Profiling, crash analysis in production |

---

## Quick Start

### 1. Configure

```bash
cmake --preset debug
```

```bash
cmake --preset debug-nosan
```

```bash
cmake --preset release
```

```bash
cmake --preset relwithdebinfo
```

### 2. Build

```bash
cmake --build --preset debug
```

```bash
cmake --build --preset debug-nosan
```

```bash
cmake --build --preset release
```

```bash
cmake --build --preset relwithdebinfo
```

### 3. Test

```bash
ctest --preset debug
```

```bash
ctest --preset debug-nosan
```

```bash
ctest --preset release
```

```bash
ctest --preset relwithdebinfo
```

### 4. Clean (remove compiled files, keep configuration)

```bash
cmake --build --preset debug --target clean
```

```bash
cmake --build --preset debug-nosan --target clean
```

```bash
cmake --build --preset release --target clean
```

```bash
cmake --build --preset relwithdebinfo --target clean
```

### 5. Clean All (remove entire build directory)

```bash
cmake --build --preset debug --target clean-all
```

```bash
cmake --build --preset debug-nosan --target clean-all
```

```bash
cmake --build --preset release --target clean-all
```

```bash
cmake --build --preset relwithdebinfo --target clean-all
```

---

## Build Directories

Each preset creates its own build directory:

| Preset | Directory |
|--------|-----------|
| `debug` | `build-debug/` |
| `debug-nosan` | `build-debug-nosan/` |
| `release` | `build-release/` |
| `relwithdebinfo` | `build-relwithdebinfo/` |

---

## One-liner Examples

### Build and test (debug with sanitizers)

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

### Build and test (release)

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
```

### Build only migcom

```bash
cmake --preset debug-nosan
cmake --build --preset debug-nosan --target migcom
```

---

## Debugging

### With GDB (use debug-nosan preset)

```bash
cmake --preset debug-nosan && cmake --build --preset debug-nosan
gdb ./build-debug-nosan/src/mach_services/lib/migcom/migcom
```

### With Valgrind (use debug-nosan preset)

```bash
cmake --preset debug-nosan && cmake --build --preset debug-nosan
valgrind --track-origins=yes ./build-debug-nosan/src/mach_services/lib/migcom/migcom \
    -V -user /tmp/test.c -server /dev/null -sheader /dev/null -dheader /dev/null \
    < src/mach_services/lib/migcom/tests/minimal.mig
```

### With AddressSanitizer (use debug preset)

The `debug` preset automatically enables ASAN and UBSAN. Just run:

```bash
cmake --preset debug && cmake --build --preset debug
./build-debug/src/mach_services/lib/migcom/migcom -V ...
```

Any memory errors will be reported automatically.

---

## VS Code Integration

The CMake Tools extension automatically detects `CMakePresets.json`.

1. Open VS Code in the `osfmk` directory
2. Press `Ctrl+Shift+P` → "CMake: Select Configure Preset"
3. Choose your desired preset
4. Press `F7` to build or `Ctrl+Shift+P` → "CMake: Build"

---

## Cleaning Build Directories

### Clean compiled files only (keeps CMake configuration)

```bash
cmake --build --preset debug --target clean
```

### Clean entire build directory

```bash
cmake --build --preset debug --target clean-all
```

**Note:** `clean-all` removes the entire `build-*` directory. You will need to run `cmake --preset <name>` again to reconfigure.

---

## Customizing Presets

You can create a `CMakeUserPresets.json` file (git-ignored) for personal overrides:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "my-debug",
      "inherits": "debug",
      "cacheVariables": {
        "CMAKE_C_FLAGS": "-g -O0 -Wall -Wextra"
      }
    }
  ]
}
```

---

## Troubleshooting

### CMake version too old

```
CMake Error: CMake 3.21 or higher is required.
```

Update CMake:
- Arch: `sudo pacman -Syu cmake`
- Ubuntu: Use the official CMake PPA or install from source

### Ninja not found

```
CMake Error: Could not find Ninja
```

Install Ninja or change the generator in the preset to "Unix Makefiles".

### ASAN reports leaks

The test preset for `debug` automatically sets `LSAN_OPTIONS=detect_leaks=0` to suppress benign leaks in this legacy codebase. If you want to see leaks, run manually:

```bash
./build-debug/src/mach_services/lib/migcom/migcom ...
```

---

## Project Structure

```
osfmk/
├── CMakeLists.txt          # Main CMake configuration
├── CMakePresets.json       # Build presets (this file)
├── BUILD.md                # This document
├── build-debug/            # Debug build (ASAN/UBSAN)
├── build-debug-nosan/      # Debug build (no sanitizers)
├── build-release/          # Release build
├── src/
│   └── mach_services/
│       └── lib/
│           └── migcom/     # MIG compiler source
└── export/
    └── powermac/
        └── include/        # Mach headers
```
