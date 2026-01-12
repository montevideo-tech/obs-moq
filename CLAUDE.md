# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**obs-moq** is an OBS Studio plugin that enables streaming via the Media over QUIC (MoQ) protocol. The plugin consists of three main components:
- **MoQ Service** (`moq-service.cpp/h`): Configures the connection to a MoQ relay (server URL, path, supported codecs)
- **MoQ Output** (`moq-output.cpp/h`): Handles the streaming output, manages the session with the MoQ server, and processes audio/video data packets
- **MoQ Source** (`moq-source.cpp/h`): Experimental feature for receiving MoQ broadcasts

The plugin wraps `libmoq`, a Rust library (located at `../moq/rs/libmoq`) that handles the MoQ protocol implementation via FFI bindings.

## Build Commands

### Initial Setup
```bash
# Configure the project (auto-detects platform: macos, ubuntu-x86_64, windows-x64)
just setup

# For local moq development, specify path to moq repo
just setup ../moq
```

### Building
```bash
# Build the plugin (auto-detects platform preset)
just build

# Build with specific preset
just build macos
just build ubuntu-x86_64
just build windows-x64
```

### Code Quality
```bash
# Check formatting (C++ with clang-format, CMake with gersemi)
just check

# Automatically fix formatting issues
just fix
```

### Installing the Plugin

After building, copy the plugin to OBS:

**macOS:**
```bash
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/PlugIns/
```

**Linux:**
```bash
cp build_x86_64/obs-moq.so ~/.config/obs-studio/plugins/obs-moq/bin/64bit/obs-moq.so
```

### Running OBS with Debug Logging
```bash
# macOS
RUST_LOG=debug RUST_BACKTRACE=1 OBS_LOG_LEVEL=debug ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/MacOS/OBS
```

## Architecture

### Plugin Registration Flow
1. `obs_module_load()` in `obs-moq.cpp` initializes the plugin
2. Calls `moq_log_level()` to configure Rust library logging
3. Registers three components: service, output, and source

### Streaming Architecture
1. **Service Layer**: `MoQService` stores server URL and path, validates configuration, and provides codec lists
2. **Output Layer**: `MoQOutput` creates an origin and broadcast, establishes a session with the MoQ server, initializes video/audio tracks, and sends encoder packets
3. **MoQ Library**: The Rust `libmoq` handles QUIC transport, MoQ protocol framing, and network I/O

### Key Data Flow
- OBS encoder packets → `MoQOutput::Data()` → `VideoData()`/`AudioData()` → `moq_track_send_*()` FFI calls
- The plugin uses callbacks for session state (connect/close) notifications

## Dependencies

- **OBS Studio**: libobs (version 31.1.1 sources in `.deps/`)
- **libmoq**: Rust library via CMake FetchContent (downloads pre-built binaries from GitHub releases)
  - Version specified in `CMakePresets.json` (`MOQ_VERSION: 0.2.0`)
  - Platform-specific targets: `aarch64-apple-darwin`, `x86_64-unknown-linux-gnu`, `x86_64-pc-windows-msvc`
  - For local development: use `just setup ../moq` to point to local moq repository
- **FFmpeg**: libavcodec, libavutil, libswscale, libswresample (via pkg-config)

## Code Style

- Use `./build-aux/run-clang-format` for C++ formatting
- Use `./build-aux/run-gersemi` for CMake formatting
- Both tools support `--check` (CI) and `--fix` (auto-format) modes

## Logging

Use the macros defined in `logger.h`:
- `LOG_DEBUG(format, ...)` - Verbose debugging info
- `LOG_INFO(format, ...)` - General information
- `LOG_WARNING(format, ...)` - Warnings
- `LOG_ERROR(format, ...)` - Errors

All logs are prefixed with `[obs-moq]` to distinguish from other OBS output.

## Testing

Connect to development server:
- URL: `http://localhost:4443/anon`
- Path: Any unique string (e.g., `obs`, `test123`)
- Watch at: `https://moq.dev/watch/?name=<your-path>`

Connect to production test server:
- URL: `https://cdn.moq.dev/anon`

## Platform-Specific Notes

- **macOS**: Builds universal binaries (arm64), requires Xcode generator
- **Linux**: Uses Ninja generator, requires `ninja-build`, `pkg-config`, `build-essential`
- **Windows**: Uses Visual Studio 17 2022 generator

## CMake Configuration

- Presets are defined in `CMakePresets.json`
- Platform detection is automatic via `just preset` command
- Key options:
  - `MOQ_LOCAL`: Path to local moq repository for development
  - `ENABLE_FRONTEND_API`: Enable OBS frontend API (currently OFF)
  - `ENABLE_QT`: Enable Qt functionality (currently OFF)
