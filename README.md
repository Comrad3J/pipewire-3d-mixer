# Disclaimer

This is actively being developed as part of my thesis project.

# PipeWire 3D Audio Mixer

A "speculative" interface which enables spatial listening of multiple app audio sources in your desktop enviroment. 

The idea is that in case of overlapping audio streams this type of interface allows users better cognative organization and seperation of those streams.

## Features

- **8 Independent SOFA Filters**: Place up to 4 stereo sources with width control
- **3D Controls**: Azimuth (0-360°), elevation (-90° to 90°), and distance
- **Visual Interface**: Top-down 360° view with distance markers
- **Direct PipeWire Integration**: Native C API communication
- **GTK4 GUI**: Native interface

## Prerequisites

### System Requirements
- Linux with PipeWire >= 0.3.0
- GTK4 >= 4.0
- Meson build system
- PipeWire with SOFA filter support (libmysofa)

### Installing Dependencies

**Arch Linux / Manjaro:**
```bash
sudo pacman -S pipewire gtk4 meson ninja libmysofa pipewire-tools
```

**Ubuntu / Debian:**
```bash
sudo apt install libpipewire-0.3-dev libgtk-4-dev meson ninja-build libmysofa-dev pipewire-bin
```

**Fedora:**
```bash
sudo dnf install pipewire-devel gtk4-devel meson libmysofa-devel pipewire-utils
```

## Quick Start

### 1. Create Project Directory

```bash
mkdir -p ~/pipewire-3d-mixer-c
cd ~/pipewire-3d-mixer-c
```

### 2. Add Source Files

Create these files in your project directory:
- `main.c` - Main application code
- `meson.build` - Build configuration
- `pw-3d-mixer.desktop` - Desktop entry (optional)

### 3. Build the Application

```bash
meson setup build
meson compile -C build
```

### 4. Configure PipeWire (Using Your Config)

Place your configuration in `/home/user/.config/pipewire/pipewire.conf.d/sp_2.conf`:

```conf
context.modules = [
  {
    name = libpipewire-module-filter-chain
    args = {
      node.description = "Multi-Source Spatializer"
      media.name = "multi_spatial"
      filter.graph = {
        nodes = [
          {
            type = sofa
            label = spatializer
            name = spk1
            config = {
              filename = "/usr/share/sofa-hrtf/your_sofa_file.sofa"
            }
            control = {
              "Azimuth" = 0.0
              "Elevation" = 0.0
              "Radius" = 1.0
            }
          }
          # ... spk2, spk3, spk4 ...
        ]
        # ... mixer nodes and links ...
      }
      capture.props = {
        node.name = "effect_input.multi_spatial"
        node.passive = true
        media.class = Audio/Sink
        audio.channels = 4
        audio.position = [ Mono Mono Mono Mono ]
      }
      playback.props = {
        node.name = "effect_output.multi_spatial"
        audio.channels = 2
        audio.position = [ FL FR ]
      }
    }
  }
]
```

**Important**: Make sure your SOFA file exists at `/usr/share/sofa-hrtf/your_sofa_file.sofa`

### 5. Restart PipeWire

```bash
systemctl --user restart pipewire pipewire-pulse
```

Or:
```bash
killall pipewire pipewire-pulse && pipewire & pipewire-pulse &
```

### 6. Run the Mixer

```bash
./build/pw-3d-mixer
```

## Usage

### Interface Overview

**Left Side - Spatial View:**
- **White Center**: Your listening position (head)
- **Colored Markers**: Stereo sources with an active audio link
  - Cyan (Source 1 - spk1/spk2)
  - Orange (Source 2 - spk3/spk4)
  - Green (Source 3 - spk5/spk6)
  - Magenta (Source 4 - spk7/spk8)
- **N/S/E/W**: Cardinal directions

**Right Side - Controls:**
- **Source Label**: Shows "Source X (spkY/spkZ)" for each stereo pair
- **Elevation Slider**: Vertical positioning (-90° to 90°), enabled when that source has audio connected
- **Width Slider**: Spreads left/right channels around the center azimuth while keeping the same distance

### Positioning Sources

1. **Connect Audio First**: Markers only appear for sources that are linked to the spatializer.
2. **Drag a Marker**: Click and drag the colored dot to move the stereo pair; you can also click the marker to drop it somewhere new.
   - Click near center = close sound
   - Click near edge = distant sound
   - Angle = direction (N=front, E=right, S=back, W=left)
3. **Set Stereo Width**: Use the width slider to push L/R apart symmetrically around the marker
4. **Set Vertical Position**: Adjust the elevation slider for that source
   - 0° = same height as you
   - 90° = directly above
   - -90° = directly below

### Connecting Audio

Use your favorite PipeWire patchbay to route audio to the spatializer inputs.

**Option 1: pw-link (Command Line)**

List available audio outputs:
```bash
pw-link -o
```

Connect an application to spk1 (first input channel):
```bash
pw-link "Firefox:output_FL" "effect_input.multi_spatial:input_FL"
pw-link "Firefox:output_FR" "effect_input.multi_spatial:input_FL"
```

Connect to spk2 (second input channel):
```bash
pw-link "Spotify:output_FL" "effect_input.multi_spatial:input_FR"
pw-link "Spotify:output_FR" "effect_input.multi_spatial:input_FR"
```

**Recommended: Graphical Tools for Linking**

Install a PipeWire patchbay:
```bash
# Helvum
flatpak install flathub org.pipewire.Helvum

# qpwgraph (pacman/apt/dnf)
sudo pacman -S qpwgraph

```

Then visually connect:
- Application outputs → `effect_input.multi_spatial` (channels 0-3)
- `effect_output.multi_spatial` → Your headphones/speakers

### Input Channel Mapping

Your filter has 4 mono input channels:
- **Channel 0** (FL position) = spk1 input
- **Channel 1** (FR position) = spk2 input
- **Channel 2** (RL position) = spk3 input
- **Channel 3** (RR position) = spk4 input

## How It Works

### Architecture

1. **Single Filter-Chain**: All 4 SOFA filters are in one PipeWire filter-chain module
2. **Mixer Stage**: Internal mixer combines all 4 stereo outputs into a final stereo output
3. **Control Naming**: Parameters are named like "spk1:Azimuth", "spk2:Elevation", etc.
4. **CLI Fallback**: Currently uses `pw-cli` for parameter setting (future versions will use native API)

### Parameter Format

When the app changes a source position, it executes:
```bash
pw-cli set-param <node-id> Props '{ "spk1:Azimuth": 45.0, "spk1:Elevation": 0.0, "spk1:Radius": 2.5 }'
```

## Troubleshooting

### "effect_input.multi_spatial" Not Found

1. **Check PipeWire config:**
```bash
cat /home/comrade/.config/pipewire/pipewire.conf.d/sp_2.conf | grep multi_spatial
```

2. **Verify SOFA file exists:**
```bash
ls -lh /usr/share/sofa-hrtf/your_sofa_file.sofa
```

3. **Test config syntax:**
```bash
pipewire -c /home/comrade/.config/pipewire/pipewire.conf.d/sp_2.conf --dry-run
```

4. **Check PipeWire logs:**
```bash
journalctl --user -u pipewire -f
```

Look for errors mentioning SOFA or filter-chain.

### No Audio Output

1. **Verify connections exist:**
```bash
pw-link -l | grep multi_spatial
```

2. **Check the filter-chain output is connected to speakers:**
```bash
pw-link -o | grep effect_output.multi_spatial
```

If not connected:
```bash
# Find your default audio sink
pw-cli info @DEFAULT_AUDIO_SINK@

# Connect manually
pw-link "effect_output.multi_spatial:output_FL" "Your-Sink:playback_FL"
pw-link "effect_output.multi_spatial:output_FR" "Your-Sink:playback_FR"
```

### Parameters Not Changing

1. **Check if pw-cli works manually:**
```bash
pw-cli set-param $(pw-cli ls Node | grep -A1 multi_spatial | grep "id" | awk '{print $2}' | head -1) Props '{ "spk1:Azimuth": 90.0 }'
```

2. **Monitor parameter changes:**
```bash
pw-cli monitor | grep -i azimuth
```

3. **Verify node ID:**
```bash
pw-cli ls Node | grep -B1 -A5 multi_spatial
```

### Application Won't Build

1. **Check dependencies:**
```bash
pkg-config --modversion gtk4 libpipewire-0.3
meson --version
```

2. **Clean and rebuild:**
```bash
rm -rf build
meson setup build
meson compile -C build
```

3. **Check for missing libraries:**
```bash
ldd build/pw-3d-mixer
```

### Markers Don't Appear

- Ensure the audio output is linked to `effect_input.multi_spatial`; markers only show for connected sources
- The filter-chain must be detected (check terminal output when starting)
- Reconnect the audio stream if the app was started before the link existed

## Advanced Usage

### Manual Testing

Test individual SOFA filters:
```bash
# Get node ID
NODE_ID=$(pw-cli ls Node | grep -A1 multi_spatial | grep "id" | awk '{print $2}' | head -1)

# Set spk1 to 45° right
pw-cli set-param $NODE_ID Props '{ "spk1:Azimuth": 45.0 }'

# Set spk2 behind and above
pw-cli set-param $NODE_ID Props '{ "spk2:Azimuth": 180.0, "spk2:Elevation": 30.0 }'

# Set spk3 close and left
pw-cli set-param $NODE_ID Props '{ "spk3:Azimuth": 270.0, "spk3:Radius": 0.5 }'

# Set spk4 far and in front
pw-cli set-param $NODE_ID Props '{ "spk4:Azimuth": 0.0, "spk4:Radius": 10.0 }'
```

### Creating Surround Sound Setup

Connect four different audio sources:
```bash
# Front channels -> spk1 (front center)
pw-link "App1:output_FL" "effect_input.multi_spatial:input_FL"

# Rear channels -> spk2 (rear)
pw-link "App2:output_FL" "effect_input.multi_spatial:input_FR"

# Side channels -> spk3 and spk4
pw-link "App3:output_FL" "effect_input.multi_spatial:input_RL"
pw-link "App4:output_FL" "effect_input.multi_spatial:input_RR"
```

Then position each in the GUI to create a surround effect.

### Performance Tuning

Edit your config to adjust the blocksize:
```conf
config = {
  filename = "/usr/share/sofa-hrtf/your_sofa_file.sofa"
  blocksize = 128  # Lower = less latency, higher CPU
}
```

Values:
- **64**: Ultra-low latency (~1ms), high CPU
- **128**: Low latency (~2ms), moderate CPU
- **256**: Balanced (~5ms), recommended
- **512**: Lower CPU (~10ms), higher latency

## File Structure

```
pipewire-3d-mixer-c/
├── main.c                    # Main application
├── meson.build              # Build configuration
├── pw-3d-mixer.desktop     # Desktop entry
├── build/                   # Build directory
│   └── pw-3d-mixer         # Compiled executable
└── README.md                # This file
```

## Differences from Original Python Version

 **Improvements:**
- Native C code (no Python subprocess overhead)
- Direct PipeWire API integration
- Modern GTK4 interface
- Real-time parameter updates
- Lower CPU usage

 **Current Limitations:**
- Uses `pw-cli` as fallback for parameter setting (will be replaced with native SPA pod API)
- Requires manual audio routing (no auto-connect yet)
- No preset save/load (planned for future)

## Future Enhancements

- [ ] Preset system for saving/loading positions
- [ ] MIDI control support

## Contributing

Tell me how stupid this project is!

## License

GPL-3.0 (matching original project)

## Resources

- [PipeWire Documentation](https://docs.pipewire.org/)
- [SOFA Databases](https://www.sofacoustics.org/data/)
- [GTK4 Documentation](https://docs.gtk.org/gtk4/)

## Credits

Based on the Python implementation by Comrad3J.
Adapted for native C integration with multi-source filter-chain configuration.
