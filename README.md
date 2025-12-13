# obs-moq

**obs-moq** is an [OBS Studio](https://obsproject.com/) plugin that enables streaming via the **Media over QUIC (MoQ)** protocol. This plugin allows you to publish live audio and video streams to a MoQ relay or server directly from OBS.

## Features

*   **Protocol:** Media over QUIC (MoQ)
*   **Video Codecs:** H.264, HEVC (H.265), AV1
*   **Audio Codecs:** AAC, Opus
*   **Low Latency:** Leverages QUIC for efficient and low-latency media transport.

## Installation

### From Source

Prerequisites:
*   CMake 3.20+
*   C++ Compiler (Clang/GCC/MSVC)
*   OBS Studio development libraries (libobs)
*   MoQ implementation from [kixelated/moq](https://github.com/kixelated/moq) (required for building the `moq` library)
*   Fork of OBS-Studio from [brianmed/obs-studio](https://github.com/brianmed/obs-studio) to enable the MoQ service in the OBS Studio UI.

1.  Clone the repository:
    ```bash
    git clone https://github.com/qualabs/obs-moq.git
    cd obs-moq
    ```

2.  (temporary) Clone the moq repository if you haven't already:
    ```bash
    git clone https://github.com/kixelated/moq.git ../moq
    ```

3.  Configure the project with CMake:
    ```bash
    just setup ../moq

    # Alternatively:
    cmake --preset macos -DMOQ_LOCAL=../moq
    ```

4.  Build the plugin:
    ```bash
    just build

    # Alternatively:
    cmake --build --preset macos
    ```

5.  Install the plugin to your OBS Studio plugins directory.
```bash
# macOS
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

5a. Or if you're building against a local obs-studio checkout:
```bash
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/PlugIns/

# Run OBS Studio with some extra logging for debugging.
RUST_LOG=debug RUST_BACKTRACE=1 OBS_LOG_LEVEL=debug ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/MacOS/OBS
```

## Usage

1.  Open OBS Studio.
2.  Go to **Settings** > **Stream**.
3.  In the **Service** dropdown, select **MoQ (Debug)**.
4.  Enter your MoQ Server details:
    *   **Server:** The URL of your MoQ relay/server (e.g., `http://localhost:4433/anon`).
    *   **Key:** The stream path or key (e.g., `dev`).
5.  Configure your Output settings (Codecs, Bitrate) as desired.
6.  Start Streaming!

## Supported Build Environments

| Platform  | Tool   |
|-----------|--------|
| Windows   | Visual Studio 17 2022 |
| macOS     | Xcode 16.0 |
| Windows, macOS  | CMake 3.30.5 |
| Ubuntu 24.04 | CMake 3.28.3 |
| Ubuntu 24.04 | `ninja-build` |
| Ubuntu 24.04 | `pkg-config`
| Ubuntu 24.04 | `build-essential` |

## License

See the [LICENSE](LICENSE) file for details.
