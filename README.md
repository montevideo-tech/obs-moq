# obs-moq

**obs-moq** is an [OBS Studio](https://obsproject.com/) plugin that enables streaming via the **Media over QUIC (MoQ)** protocol. This plugin allows you to publish live audio and video streams to a MoQ relay or server directly from OBS.

## Features

*   **Protocol:** Media over QUIC (MoQ)
*   **Video Codecs:** H.264, HEVC (H.265), AV1
*   **Audio Codecs:** AAC, Opus
*   **Low Latency:** Leverages QUIC for efficient and low-latency media transport.

## Installation

### Setup

Prerequisites:
*   CMake 3.20+
*   C++ Compiler (Clang/GCC/MSVC)
*   OBS Studio development libraries (libobs)
*   [Fork of OBS-Studio](https://github.com/brianmed/obs-studio) just to show MoQ in the UI.

1.  Clone the repos:
    ```bash
    git clone https://github.com/moq-dev/obs.git moq-obs
    git clone https://github.com/brianmed/obs-studio.git obs-studio

    # optional: for local moq development
    git clone https://github.com/moq-dev/moq.git moq
    ```

2. Build the OBS fork:
    ```bash
    cd obs-studio

    # Replace with your platform
    cmake --preset macos
    cmake --build --preset macos
    ```

3.  Configure the plugin:
    ```bash
    cd moq-obs

    just setup

    # optional: for local moq development
    just setup ../moq
    ```

### Build

1.  Build the plugin:
    ```bash
    just build
    ```

2. Copy the plugin to the OBS Studio plugins directory:
```bash
# macOS
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/PlugIns/

# eventually, without the fork:
cp -a build_macos/RelWithDebInfo/obs-moq.plugin ~/Library/Application\ Support/obs-studio/plugins/
```

3. Run OBS Studio with some extra logging for debugging.
```bash
# macOS
RUST_LOG=debug RUST_BACKTRACE=1 OBS_LOG_LEVEL=debug ../obs-studio/build_macos/frontend/RelWithDebInfo/OBS.app/Contents/MacOS/OBS
```

## Usage

1.  Open OBS Studio.
2.  Go to **Settings** > **Stream**.
3.  In the **Service** dropdown, select **MoQ**.
4.  Enter your MoQ Server details:
    * For development (`just dev`): `http://localhost:4433/anon`.
    * For testing: `https://cdn.moq.dev/anon`.
5.  Enter the broadcast name/path:
    * For testing: `obs` or some unique string.
    * Watch it here: https://moq.dev/watch/?name=obs
5.  Configure your Output settings (Codecs, Bitrate) as desired.
    * Currently, only: `h264` and `aac` are supported.
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
