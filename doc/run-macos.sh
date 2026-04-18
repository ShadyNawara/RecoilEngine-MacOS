#!/usr/bin/env bash
# Launch the native-macOS RecoilEngine build through the BYAR-Chobby lobby.
# Prereqs: doc/building-macos.md steps 1–6 completed, so $HOME/mesa-native
# exists and build/spring is up to date.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
MESA_NATIVE_DIR="${MESA_NATIVE_DIR:-$HOME/mesa-native}"

export XDG_CONFIG_HOME="$HOME/.config"
export DYLD_INSERT_LIBRARIES="$MESA_NATIVE_DIR/lib/libgl_interpose.dylib"
export DYLD_LIBRARY_PATH="$MESA_NATIVE_DIR/lib"
export VK_DRIVER_FILES="$MESA_NATIVE_DIR/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json"
export MESA_LOADER_DRIVER_OVERRIDE=zink
export MESA_GL_VERSION_OVERRIDE=4.6
export EGL_PLATFORM=surfaceless
export SPRING_DATADIR="$BUILD_DIR:$REPO_ROOT/cont:$HOME/.config/spring"

cd "$BUILD_DIR"
exec ./spring --menu 'BYAR Chobby $VERSION'
