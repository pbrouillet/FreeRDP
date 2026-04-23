# Copilot Instructions for FreeRDP

FreeRDP is a free implementation of the Remote Desktop Protocol (RDP), licensed under Apache 2.0.
API documentation: https://pub.freerdp.com/api/
MS Open Specifications: https://www.microsoft.com/openspecifications/

## Build

Out-of-source builds only. Ninja is recommended.

```sh
# Configure (debug + tests)
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING_INTERNAL=ON -B build -S .

# Build
cmake --build build --parallel

# Run all tests
ctest --test-dir build --output-on-failure

# Run a single test (regex match)
ctest --test-dir build --output-on-failure -R '^TestSettings$'
```

### CI QA preload

The `ci/cmake-preloads/config-qa.cmake` preload switches the compiler to clang,
enables ASan, clang-tidy, internal tests, and most optional components:

```sh
cmake -GNinja -C ci/cmake-preloads/config-qa.cmake -B build -S .
```

### Formatting and linting

- **clang-format**: configured in `.clang-format`. Run via CMake:
  ```sh
  cmake -GNinja -DWITH_CLANG_FORMAT=ON -B build -S .
  cmake --build build --target clangformat
  ```
- **clang-tidy**: configured in `.clang-tidy`. Enabled automatically by `config-qa.cmake`
  or manually with `-DBUILD_WITH_CLANG_TIDY=ON`.
- `compile_commands.json` is exported by default (useful for editor integration).

## Architecture

### Core libraries

- **libfreerdp/** — Core RDP protocol library. Built from submodules: core, codec, gdi,
  cache, crypto, primitives, locale, utils, common, emu.
- **winpr/** — Windows Portable Runtime. Platform abstraction layer providing Win32-like
  APIs (threads, synch, crypto, file, pipe, registry, etc.) on all platforms. Used by
  every other component.
- **include/freerdp/** — Public FreeRDP API headers.
- **winpr/include/winpr/** — Public WinPR API headers.

### Channels

- **channels/** — RDP virtual channel plugins. Each channel has its own directory containing:
  - `ChannelOptions.cmake` — defines channel name, type (static/dynamic), and MS spec reference
  - `client/` and/or `server/` — endpoint-specific implementations
  - `common/` (optional) — shared code between client and server
  - Client channels may have backend **subsystem** directories (e.g., `client/alsa/`,
    `client/pulse/`) for platform-specific implementations.
- Dynamic virtual channels depend on the **drdynvc** static channel.

### Frontends and servers

- **client/** — Platform-specific clients: SDL (primary, C++17), X11, Wayland, macOS
  (Objective-C), iOS, Android, Windows, and a sample client.
- **server/** — Server components: proxy, shadow server, sample server, platform server.
- **rdtk/** — Remote Desktop Toolkit (optional, used by shadow server).
- **uwac/** — Using Wayland As Client helper library (optional, Wayland-only).

## Code Conventions

### Language

The core project (`libfreerdp`, `winpr`, `channels`) is **C** (`LANGUAGES C` in CMake).
Some subprojects use **C++17** (SDL client, proxy modules) or **Objective-C** (macOS/iOS
clients). Default to C unless the file or module you're editing is already C++.

### Style

Enforced by `.clang-format`:
- **Allman** brace style (braces on their own line)
- **Tabs** for indentation, 4-space tab width
- **100-column** line limit
- Pointer alignment: `int* ptr` (left-aligned)

### Naming and visibility

- Functions are prefixed by module: `freerdp_*`, `winpr_*`, channel-specific prefixes.
- Public API symbols use visibility macros: `FREERDP_API` / `FREERDP_LOCAL` for libfreerdp,
  `WINPR_API` for WinPR.
- `FREERDP_ENTRY_POINT(fkt)` macro silences missing-prototype warnings for library entry points.

### Error handling and return values

- Functions return `BOOL` (`TRUE`/`FALSE`) for success/failure.
- `WINPR_ATTR_NODISCARD` marks functions whose return values must be checked.
- Null-safe callback dispatch via `IFCALL(cb, ...)`, `IFCALLRET(cb, ret, ...)`,
  and `IFCALLRESULT(default, cb, ...)`.

### ABI stability

Public structs use `ALIGN64` on fields and include reserved padding fields.
Preserve these when modifying ABI-stable structures in `include/`.

### Logging

Uses **WLog**, a hierarchical logging system. Loggers use `WLog_*` macros
(e.g., `WLog_ERR`, `WLog_DBG`). Configurable at runtime via environment variables
(`WLOG_LEVEL`, `WLOG_FILTER`, `WLOG_APPENDER`).

### Testing

Tests are organized per module in `*/test/` directories using CMake's
`create_test_sourcelist`. Each test module produces a single executable that
dispatches to individual test functions by name. Enable with
`-DBUILD_TESTING_INTERNAL=ON` (CI) or `-DBUILD_TESTING=ON` (packaging-compatible).
