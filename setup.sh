#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${PW_MIXER_BUILD_DIR:-$SCRIPT_DIR/build}"

: "${XDG_CONFIG_HOME:=${HOME}/.config}"
CONFIG_DIR="${PW_MIXER_CONFIG_DIR:-$XDG_CONFIG_HOME/pipewire/pipewire.conf.d}"
CONFIG_FILE="${PW_MIXER_CONFIG_FILE:-$CONFIG_DIR/sp_2.conf}"
SOFA_FILE="${PW_MIXER_SOFA_FILE:-}"

detect_sofa_file() {
    local candidate found

    for candidate in \
        "/usr/share/sofa-hrtf/hrtf-b_nh724.sofa" \
        "/usr/local/share/sofa-hrtf/hrtf-b_nh724.sofa" \
        "$HOME/.config/hrtf-sofa/FABIAN_HRIR_modeled_HATO_0.sofa"
    do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    for candidate in \
        "$HOME/.config/hrtf-sofa" \
        "${XDG_DATA_HOME:-$HOME/.local/share}/sofa" \
        "/usr/share/sofa-hrtf" \
        "/usr/local/share/sofa-hrtf"
    do
        if [ -d "$candidate" ]; then
            found="$(find "$candidate" -maxdepth 2 -type f -name '*.sofa' 2>/dev/null | head -n 1 || true)"
            if [ -n "$found" ]; then
                printf '%s\n' "$found"
                return 0
            fi
        fi
    done

    return 1
}

render_config() {
    local escaped_sofa

    mkdir -p "$CONFIG_DIR"
    escaped_sofa="$(printf '%s\n' "$SOFA_FILE" | sed 's/[&|]/\\&/g')"
    sed "s|@SOFA_FILE@|$escaped_sofa|g" "$SCRIPT_DIR/sp_2.conf" > "$CONFIG_FILE"
}

echo "== PipeWire 3D Mixer setup =="
echo "Project root: $SCRIPT_DIR"
echo

echo "Checking dependencies..."
missing_deps=()

command -v pkg-config >/dev/null 2>&1 || missing_deps+=("pkg-config")
command -v meson >/dev/null 2>&1 || missing_deps+=("meson")
command -v ninja >/dev/null 2>&1 || missing_deps+=("ninja")
command -v pw-cli >/dev/null 2>&1 || missing_deps+=("pw-cli")

pkg-config --exists gtk4 2>/dev/null || missing_deps+=("gtk4")
pkg-config --exists libpipewire-0.3 2>/dev/null || missing_deps+=("libpipewire-0.3")
pkg-config --exists libspa-0.2 2>/dev/null || missing_deps+=("libspa-0.2")

if [ "${#missing_deps[@]}" -ne 0 ]; then
    echo "ERROR: missing dependencies: ${missing_deps[*]}"
    echo
    echo "Install them with your distro package manager, then rerun this script."
    exit 1
fi

echo "Dependencies look good."
echo

if [ -z "$SOFA_FILE" ]; then
    SOFA_FILE="$(detect_sofa_file || true)"
fi

if [ -z "$SOFA_FILE" ]; then
    echo "ERROR: no SOFA file was found automatically."
    echo "Set PW_MIXER_SOFA_FILE=/path/to/your/file.sofa and rerun."
    exit 1
fi

if [ ! -f "$SOFA_FILE" ]; then
    echo "ERROR: SOFA file does not exist: $SOFA_FILE"
    exit 1
fi

echo "Using SOFA file: $SOFA_FILE"
render_config
echo "Installed PipeWire config: $CONFIG_FILE"
echo

if [ ! -f "$SCRIPT_DIR/meson.build" ]; then
    echo "ERROR: meson.build not found in $SCRIPT_DIR"
    exit 1
fi

echo "Configuring build directory..."
if [ -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" "$SCRIPT_DIR" --reconfigure
else
    meson setup "$BUILD_DIR" "$SCRIPT_DIR"
fi

echo "Building..."
meson compile -C "$BUILD_DIR"

echo
echo "Build complete."
echo "Executable: $BUILD_DIR/pw-3d-mixer"
echo
echo "Next steps:"
echo "  1. Restart PipeWire so it loads: $CONFIG_FILE"
echo "     Example for systemd user sessions:"
echo "       systemctl --user restart pipewire pipewire-pulse"
echo "  2. Verify the filter-chain is present:"
echo "       pw-cli ls Node | grep multi_spatial"
echo "  3. Run the mixer:"
echo "       $BUILD_DIR/pw-3d-mixer"
