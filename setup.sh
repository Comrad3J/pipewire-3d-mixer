#!/bin/bash
# Setup script for PipeWire 3D Audio Mixer (4-Channel)

set -e

PROJECT_DIR="pipewire-3d-mixer-c"

echo "=== PipeWire 3D Audio Mixer Setup (4-Channel) ==="
echo

# Check dependencies
echo "Checking dependencies..."
MISSING_DEPS=()

command -v pkg-config >/dev/null 2>&1 || MISSING_DEPS+=("pkg-config")
command -v meson >/dev/null 2>&1 || MISSING_DEPS+=("meson")
command -v ninja >/dev/null 2>&1 || MISSING_DEPS+=("ninja")
command -v pw-cli >/dev/null 2>&1 || MISSING_DEPS+=("pw-cli")

pkg-config --exists gtk4 2>/dev/null || MISSING_DEPS+=("gtk4")
pkg-config --exists libpipewire-0.3 2>/dev/null || MISSING_DEPS+=("libpipewire-0.3")

if [ ${#MISSING_DEPS[@]} -ne 0 ]; then
    echo "ERROR: Missing dependencies: ${MISSING_DEPS[*]}"
    echo
    echo "Install them with:"
    
    if command -v apt >/dev/null 2>&1; then
        echo "  sudo apt install libpipewire-0.3-dev libgtk-4-dev meson ninja-build libmysofa-dev pipewire-bin pkg-config"
    elif command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -S pipewire gtk4 meson ninja libmysofa pipewire-tools"
    elif command -v dnf >/dev/null 2>&1; then
        echo "  sudo dnf install pipewire-devel gtk4-devel meson libmysofa-devel pipewire-utils"
    fi
    
    exit 1
fi

echo " All dependencies found"
echo

# Create project directory if it doesn't exist
if [ ! -d "$PROJECT_DIR" ]; then
    echo "Creating project directory: $PROJECT_DIR"
    mkdir -p "$PROJECT_DIR"
fi

cd "$PROJECT_DIR"

# Check if files exist
if [ -f "main.c" ]; then
    echo " main.c exists"
else
    echo "! Please create main.c with the provided code"
fi

if [ -f "meson.build" ]; then
    echo " meson.build exists"
else
    echo "! Please create meson.build with the provided configuration"
fi

echo

# Check SOFA file
SOFA_FILE="/usr/share/sofa-hrtf/hrtf-b_nh724.sofa"
if [ -f "$SOFA_FILE" ]; then
    echo "✓ SOFA file found: $SOFA_FILE"
    echo "  Size: $(du -h "$SOFA_FILE" | cut -f1)"
else
    echo "⚠ SOFA file not found: $SOFA_FILE"
    echo "  This is required for your PipeWire configuration"
    echo "  Download a SOFA file and place it at this location"
    echo "  Or update your PipeWire config with the correct path"
fi

echo

# Check PipeWire configuration
CONFIG_FILE="/home/comrade/.config/pipewire/pipewire.conf.d/sp_2.conf"

if [ -f "$CONFIG_FILE" ]; then
    echo " PipeWire config exists: $CONFIG_FILE"
    
    if grep -q "effect_input.multi_spatial" "$CONFIG_FILE"; then
        echo " Multi-spatial configuration found in config"
    else
        echo " 'effect_input.multi_spatial' not found in config"
        echo "  Please add your multi-source spatializer configuration"
    fi
else
    echo " No PipeWire config found at: $CONFIG_FILE"
    echo "  Please create this file with your multi-spatial configuration"
fi

echo

# Build project
if [ -f "main.c" ] && [ -f "meson.build" ]; then
    echo "Building project..."
    
    if [ -d "build" ]; then
        echo "Cleaning old build directory..."
        rm -rf build
    fi
    
    echo "Running meson setup..."
    meson setup build
    
    echo "Compiling..."
    meson compile -C build
    
    if [ -f "build/pw-3d-mixer" ]; then
        echo
        echo " Build successful!"
        echo
        echo "Executable: $(pwd)/build/pw-3d-mixer"
        echo "Size: $(du -h build/pw-3d-mixer | cut -f1)"
    else
        echo " Build failed"
        exit 1
    fi
else
    echo "! Cannot build - missing source files"
    echo "  Please create main.c and meson.build first"
fi

echo

# Check if PipeWire is running
if systemctl --user is-active --quiet pipewire; then
    echo " PipeWire is running"
    
    # Check for multi_spatial node
    if pw-cli ls Node 2>/dev/null | grep -q "multi_spatial"; then
        echo " Multi-spatial node detected!"
        
        NODE_ID=$(pw-cli ls Node 2>/dev/null | grep -B1 "effect_input.multi_spatial" | grep "id" | awk '{print $2}' | head -1 | tr -d ',')
        if [ -n "$NODE_ID" ]; then
            echo "  Node ID: $NODE_ID"
        fi
    else
        echo "⚠ Multi-spatial node NOT found"
        echo "  Your PipeWire config may not be loaded"
        echo "  Try restarting PipeWire:"
        echo "    systemctl --user restart pipewire pipewire-pulse"
    fi
else
    echo "⚠ PipeWire is not running"
    echo "  Start it with: systemctl --user start pipewire"
fi

echo

# Post-setup instructions
cat << 'EOF'
=== Setup Complete! ===

Next Steps:

1. Verify PipeWire Configuration:
    Config file: /home/comrade/.config/pipewire/pipewire.conf.d/sp_2.conf
    Contains multi_spatial filter-chain
    SOFA file path is correct

2. Restart PipeWire:
   systemctl --user restart pipewire pipewire-pulse

3. Verify the filter-chain:
   pw-cli ls Node | grep multi_spatial

4. Run the mixer:
   ./build/pw-3d-mixer

5. Connect audio sources:
   - Use Helvum, qpwgraph, or pw-link
   - Connect apps to effect_input.multi_spatial (4 channels)
   - Connect effect_output.multi_spatial to your speakers

=== Quick Test ===

Test that parameters work:
   NODE_ID=$(pw-cli ls Node | grep -A1 multi_spatial | grep "id" | awk '{print $2}' | head -1 | tr -d ',')
   pw-cli set-param $NODE_ID Props '{ "spk1:Azimuth": 90.0 }'

If this works, the GUI controls should work too!

=== Troubleshooting ===

If the node isn't detected:
   1. Check logs: journalctl --user -u pipewire -f
   2. Verify SOFA file exists at the path in your config
   3. Test config: pipewire -c /home/comrade/.config/pipewire/pipewire.conf.d/sp_2.conf --dry-run

For more help, see README.md

EOF