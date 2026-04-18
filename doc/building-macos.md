# Building RecoilEngine on macOS (Apple Silicon)

This document covers building and running Recoil natively on macOS — specifically
Apple Silicon (M-series). Apple's OpenGL.framework tops out at OpenGL 4.1 Core
and does not provide a compatibility profile, neither of which is enough for
Recoil. Instead, the engine is wired through **Mesa + Zink + KosmicKrisp**,
which exposes a full **OpenGL 4.6 Compatibility Profile** to the engine by
translating GL → Vulkan → Metal on the GPU. The same modern code path Linux
CI builds run.

It also covers running **BYAR-Chobby** (the Beyond All Reason lobby) on top
of the native macOS build in a live-editable source layout.

Tested on macOS Tahoe (26.x) with an Apple M4 Max. Should work on any arm64
macOS that supports Vulkan/Metal on Apple GPUs.


---

## Pipeline at a glance

```
Recoil (OpenGL 4.6 compat)
  │
  ▼
Mesa libGL wrapper / libEGL (patched ShadyNawara/mesa-BAR master, EGL surfaceless + Metal vtbl)
  │
  ▼
Zink (Gallium OpenGL-over-Vulkan driver)
  │
  ▼
KosmicKrisp (Mesa's native Vulkan-on-Metal driver for Apple GPUs)
  │
  ▼
Metal → M-series GPU
```

A small shim (`libgl_interpose.dylib`, built as fat arm64+arm64e) is injected
via `DYLD_INSERT_LIBRARIES` so that `dlsym("gl*")` resolves through
`eglGetProcAddress`, and so `MESA_EGL_LIBRARY` / `MESA_VULKAN_LIBRARY` are
discovered relative to the installed Mesa prefix.

---

## Prerequisites

Install Xcode Command Line Tools (provides `/usr/bin/clang`, the macOS SDK,
and `xcrun`):

```bash
xcode-select --install
```

Install Homebrew if you don't have it:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

## Step 1 — Homebrew packages

```bash
# Engine build tools and dependencies
brew install cmake ninja pkgconf \
             sdl2 devil glew openal-soft expat p7zip sevenzip \
             freetype fontconfig libpng jpeg-turbo libogg libvorbis

# Mesa build tools
brew install meson bison llvm glslang spirv-tools spirv-llvm-translator libclc \
             vulkan-loader molten-vk
```

Notes:
- **`devil`** — OpenIL, image loading library required by Recoil.
- **`p7zip`** — provides the `7z` binary that Recoil's `FindSevenZip.cmake` looks for. Homebrew's `sevenzip` only ships `7zz` and won't be detected.
- **Do not** `brew install libunwind` — that formula is Linux-only. macOS provides libunwind via libSystem.
- **`vulkan-loader`** provides the Khronos Vulkan loader (`libvulkan.1.dylib`) that Zink opens. KosmicKrisp is a Vulkan ICD *driver*, not a loader.
- **`molten-vk`** is still pulled in because Mesa's build requires its headers for some fallback paths, even when kosmickrisp is the primary driver.

## Step 2 — Python modules for the Mesa build

Pick whichever Python 3 Homebrew's Mesa build resolves to (3.10+). For the
default Python 3.14 on a current Homebrew:

```bash
pip3.14 install --break-system-packages --user mako pyyaml packaging
```

Without these, Meson's configure step errors with `Python >= 3.10 not found`
even though a suitable interpreter is present.

## Step 3 — Build the patched Mesa fork

The fork at <https://github.com/ShadyNawara/mesa-BAR> (branched from
<https://github.com/lucamignatti/mesa>, which introduced the Metal vtbl for
`egl/drivers/dri2/platform_surfaceless.c` and the `src/glwrapper/` shim —
background in the upstream write-up at
<https://gist.github.com/lucamignatti/5312f5e937de2ba44256ecba6de54cc2>)
wires `libgl_wrapper.c` + `libgl_interpose.c` into the meson build so
`libGL.dylib` and `libgl_interpose.dylib` are produced automatically, and
layers on the KosmicKrisp-specific swapchain/format fixes Recoil needs.

```bash
git clone --depth 1 https://github.com/ShadyNawara/mesa-BAR.git mesa-bar
cd mesa-bar

export PATH="/opt/homebrew/opt/bison/bin:$PATH"
export SDKROOT="$(xcrun --show-sdk-path)"
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
export CC=/usr/bin/clang
export CXX=/usr/bin/clang++

meson setup build --native-file native.ini \
  -Dprefix=$HOME/mesa-native \
  -Dbuildtype=release \
  -Dplatforms=macos \
  -Degl-native-platform=surfaceless \
  -Degl=enabled \
  -Dgallium-drivers=zink \
  -Dvulkan-drivers=kosmickrisp \
  -Dglx=disabled \
  -Dgbm=disabled \
  -Dllvm=enabled \
  -Dshared-llvm=enabled \
  -Dmoltenvk-dir=/opt/homebrew/opt/molten-vk

ninja -C build
meson install -C build
```

Gotchas:
- **Use Apple's clang, not Homebrew's LLVM clang.** Homebrew clang points at a SDK path (`/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk`) that may not exist, causing configure to fail with `library 'System' not found`.
- **Homebrew bison** must be first on `PATH`; Apple's system bison 2.3 is too old for Mesa.

Build time is roughly 10–20 minutes on an M-series chip.

## Step 4 — Copy the real Vulkan loader into the Mesa install

The Homebrew `vulkan-loader` exposes `libvulkan.1.dylib` as a symlink, but the
libgl interposer runs `dladdr` to find its own directory and tries to open a
co-located `libvulkan.1.dylib` by name. Copy the real dylib (not the symlink)
into the Mesa install and re-create the symlinks locally:

```bash
cp /opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.*.dylib $HOME/mesa-native/lib/
cd $HOME/mesa-native/lib
REAL_VULKAN=$(ls libvulkan.1.*.dylib | head -1)
ln -sf "$REAL_VULKAN" libvulkan.1.dylib
ln -sf libvulkan.1.dylib libvulkan.dylib
```

## Step 5 — Rebuild the interposer as fat arm64+arm64e

Mesa's build produces `libgl_interpose.dylib` as arm64-only. When the engine
runs under `DYLD_INSERT_LIBRARIES`, macOS calls such as `wordexp(3)` spawn
`/bin/sh`, which is an arm64e system binary. dyld refuses to inject an arm64
dylib into an arm64e process and silently fails the expansion, which breaks
`${XDG_CONFIG_HOME-...}` substitution inside Recoil's DataDirLocater. Fix
this by building a fat dylib:

```bash
/usr/bin/clang -arch arm64 -arch arm64e -dynamiclib \
  -install_name @rpath/libgl_interpose.dylib \
  -I$HOME/mesa-native/include \
  "src/glwrapper/libgl_interpose.c" \
  -L$HOME/mesa-native/lib -lEGL \
  -o $HOME/mesa-native/lib/libgl_interpose.dylib
```

(The linker's `-lEGL` warning about the missing arm64e EGL slice is benign;
only the arm64 slice of the interposer is ever loaded into Recoil.)

## Step 6 — Configure and build Recoil

From the repository root:

```bash
git clone --recursive https://github.com/ShadyNawara/RecoilEngine-MacOS RecoilEngine
cd RecoilEngine

mkdir -p build && cd build

export CMAKE_PREFIX_PATH="/opt/homebrew/opt/openal-soft:/opt/homebrew/opt/expat:/opt/homebrew/opt/glew:/opt/homebrew/opt/devil:/opt/homebrew/opt/sdl2:/opt/homebrew/opt/freetype:/opt/homebrew/opt/fontconfig:/opt/homebrew/opt/libpng:/opt/homebrew/opt/libogg:/opt/homebrew/opt/libvorbis"

cmake -S .. -B . -G Ninja \
  -DCMAKE_BUILD_TYPE=RELWITHDEBINFO \
  -DAI_TYPES=NATIVE \
  -DAI_EXCLUDE_REGEX='^CppTestAI$' \
  -DOPENAL_INCLUDE_DIR=/opt/homebrew/opt/openal-soft/include/AL \
  -DOPENAL_LIBRARY=/opt/homebrew/opt/openal-soft/lib/libopenal.dylib \
  -DMESA_NATIVE_DIR=$HOME/mesa-native

ninja engine-legacy
```

This produces `build/spring` linked against `$HOME/mesa-native/lib/libGL.dylib`
and `libEGL.dylib` instead of Apple's `OpenGL.framework`.

`MESA_NATIVE_DIR` defaults to `$HOME/mesa-native`; pass a different path if you
installed Mesa elsewhere. CMake will fail early with a clear error if the path
doesn't contain `lib/libGL.dylib`.

OpenAL overrides are required because CMake on macOS otherwise picks up
Apple's built-in `OpenAL.framework`, which does not support EFX extensions
and breaks `rts/System/Sound/OpenAL/EFXPresets.h`.

## Step 7 — Run

```bash
cd /path/to/RecoilEngine/build

XDG_CONFIG_HOME="$HOME/.config" \
DYLD_INSERT_LIBRARIES=$HOME/mesa-native/lib/libgl_interpose.dylib \
DYLD_LIBRARY_PATH=$HOME/mesa-native/lib \
VK_DRIVER_FILES=$HOME/mesa-native/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
MESA_LOADER_DRIVER_OVERRIDE=zink \
MESA_GL_VERSION_OVERRIDE=4.6 \
EGL_PLATFORM=surfaceless \
SPRING_DATADIR="$(pwd):$(pwd)/../cont" \
./spring
```

On a successful launch, `~/.config/spring/infolog.txt` should show:

```
[MacEGL] EGL 1.5  vendor=Mesa Project  version=1.5
GL vendor    : Mesa
GL renderer  : zink Vulkan 1.3(Apple M… (MESA_KOSMICKRISP))
GL version   : 4.6 (Compatibility Profile) Mesa 26.1.0-devel
GLSL version : 4.60
Initialized OpenGL Context: 4.6 (Compat)
GLSL shader support       : 1
GL4 support               : 1
FBO extension support     : 1
```

and zero `Fatal` / `Error:` / `Shader … Error` / `Link error` lines.


---

## Troubleshooting

### `dyld[…]: tried: '…/libgl_interpose.dylib' (mach-o file, but is an incompatible architecture (have 'arm64', need 'arm64e'))`
The interposer wasn't rebuilt as a fat binary. Repeat step 5.

### `${XDG_CONFIG_HOME-"~/.config"}/spring/` appears literally in logs, engine aborts with `a datadir may not be specified with a relative path`
Also the fat-interposer issue: `wordexp()` calls `/bin/sh` (arm64e), injection fails silently, env expansion returns the original string. Rebuild the interposer.

### `MESA: error: ZINK: failed to load libvulkan.1.dylib`
The Vulkan loader isn't alongside `libEGL.dylib`. Repeat step 4.

### `[GR::CreateGLContext] error creating main GL3.0 compatibility-context`
Mesa EGL isn't loading — the interposer's `MESA_EGL_LIBRARY` setting hasn't reached the process. Check `DYLD_INSERT_LIBRARIES` is set and the path is correct. `otool -L build/spring | grep libGL` should show `$HOME/mesa-native/lib/libGL.dylib`, not `/System/Library/Frameworks/OpenGL.framework/…`.

### `MESA: error: CreateSwapchainKHR failed with VK_ERROR_FORMAT_NOT_SUPPORTED`
This is a warning-level message that Mesa prints on its first try; it retries with a compatible format and succeeds. If the engine reports frames rendering afterwards, it's working as intended.

### `WARNING: Some incorrect rendering might occur because the selected Vulkan device (Apple M…) doesn't support base Zink requirements: have_EXT_custom_border_color have_EXT_line_rasterization`
Expected on Apple hardware — KosmicKrisp doesn't expose these optional Vulkan extensions yet. Zink uses fallbacks; visible glitches are possible but gameplay works.


---

## Running BYAR-Chobby (the BAR lobby)

BYAR-Chobby is a Lua-only "game" archive that the engine runs as its menu.
It depends on the base Chobby lobby code plus a few libs (`chili`, `chilifx`,
`chotify`, `i18n`, `liblobby`) — the source repo bundles them under `libs/`.

### Step A — Clone the Chobby and BAR source trees

Pull both repositories directly and expose them to the engine as live-editable
SDD ("spring-data directory") archives.

Pick any directory with enough free space (~4 GB total) and export it as
`BAR_SRC` so the rest of this section is copy-pasteable:

```bash
export BAR_SRC="$HOME/src"          # or any other path you prefer
mkdir -p "$BAR_SRC"
cd "$BAR_SRC"

# ~320 MB; the lobby UI and its embedded Lua libs
git clone --depth 1 https://github.com/beyond-all-reason/BYAR-Chobby.git

# ~3.2 GB; the full BAR game — required by Chobby's
# Configuration:GetDefaultGameName() path even for menu-only startups
git clone --depth 1 https://github.com/beyond-all-reason/Beyond-All-Reason.git
```

Use `--depth 1` unless you intend to edit and push changes.

### Step B — Publish both as SDDs the engine can resolve

Symlink each clone into `~/.config/spring/games/` under the exact name the
engine reads from `modinfo.lua`. For Chobby that's `BYAR-Chobby.sdd`; for the
game archive that's `BAR.sdd`:

```bash
mkdir -p "$HOME/.config/spring/games"

ln -sfn "$BAR_SRC/BYAR-Chobby" \
        "$HOME/.config/spring/games/BYAR-Chobby.sdd"

ln -sfn "$BAR_SRC/Beyond-All-Reason" \
        "$HOME/.config/spring/games/BAR.sdd"
```

Both `modinfo.lua` files use `version = '$VERSION'` literally, so the engine
sees the archives as `BYAR Chobby $VERSION` and `Beyond All Reason $VERSION`.
Pass those exact strings (single-quoted so the shell doesn't expand `$VERSION`)
when referencing them from command lines or skirmish scripts.

### Step C — Deploy `chobby_config.json`

The spring-launcher writes this file at startup in production builds; on a
native macOS build it has to be created by hand:

```bash
cat > "$HOME/.config/spring/chobby_config.json" <<'EOF'
{
    "server": {
        "address": "server4.beyondallreason.info",
        "port": 8200,
        "protocol": "spring",
        "serverName": "BAR"
    },
    "game": "byar"
}
EOF
```

Without this file Chobby aborts with `Missing chobby_config.json file`.

### Step D — Launch

The repo ships [`doc/run-macos.sh`](run-macos.sh), a thin wrapper that sets
the full `DYLD_INSERT_LIBRARIES` / `VK_DRIVER_FILES` / `MESA_*` / `EGL_PLATFORM`
/ `SPRING_DATADIR` envelope and invokes the engine with the Chobby lobby as
its menu:

```bash
cd /path/to/RecoilEngine
doc/run-macos.sh
```

Equivalent bare command, for reference:

```bash
cd /path/to/RecoilEngine/build

XDG_CONFIG_HOME="$HOME/.config" \
DYLD_INSERT_LIBRARIES=$HOME/mesa-native/lib/libgl_interpose.dylib \
DYLD_LIBRARY_PATH=$HOME/mesa-native/lib \
VK_DRIVER_FILES=$HOME/mesa-native/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json \
MESA_LOADER_DRIVER_OVERRIDE=zink \
MESA_GL_VERSION_OVERRIDE=4.6 \
EGL_PLATFORM=surfaceless \
SPRING_DATADIR="$(pwd):$(pwd)/../cont:$HOME/.config/spring" \
./spring --menu 'BYAR Chobby $VERSION'
```

From the lobby you can log in, pick a game mode, and start a skirmish/multiplayer
match — the engine loads `BAR.sdd` from the same `games/` directory.

### Updating Chobby or BAR later

Since both are plain checkouts, `git pull` in either directory refreshes the
content the engine sees next launch — no rapid-repo re-fetch, no container
rebuild:

```bash
( cd "$BAR_SRC/BYAR-Chobby"        && git pull )
( cd "$BAR_SRC/Beyond-All-Reason"  && git pull )
```

