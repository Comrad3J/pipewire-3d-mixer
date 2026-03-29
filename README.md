# PipeWire 3D Mixer

Native C/GTK4 controller for a PipeWire filter-chain that spatializes up to 4 stereo sources through 8 SOFA channels.

## Features

- GTK4 desktop UI for moving sources in azimuth, elevation, distance, and stereo width
- Native PipeWire registry tracking for source and link detection
- `spatial-notification` CLI for sending a sound to a specific spatial channel
- Meson-based build with no machine-specific paths in the repository

## Dependencies

- `gtk4`
- `libpipewire-0.3`
- `libspa-0.2`
- `meson`
- `ninja`
- `pw-cli`
- a `.sofa` HRTF file if you do not want to use the bundled one

Examples:

```bash
# Debian / Ubuntu
sudo apt install libgtk-4-dev libpipewire-0.3-dev libspa-0.2-dev meson ninja-build pipewire-bin

# Arch
sudo pacman -S gtk4 pipewire meson ninja pipewire-tools

# Fedora
sudo dnf install gtk4-devel pipewire-devel meson ninja-build pipewire-utils
```

## Build

```bash
meson setup build
meson compile -C build
```

## PipeWire Configuration

The app expects a PipeWire filter-chain with these node names:

- `effect_input.multi_spatial`
- `effect_output.multi_spatial`

The repository ships a portable config template in `sp_2.conf`. It uses `@SOFA_FILE@` as a placeholder so you can install it on any Linux system.

## Bundled SOFA File

The repository now includes the SOFA file this project is currently using:

```bash
assets/sofa/FABIAN_HRIR_modeled_HATO_0.sofa
```

`setup.sh` will prefer that bundled file automatically. You can still override it with:

```bash
PW_MIXER_SOFA_FILE=/path/to/your/file.sofa ./setup.sh
```

Attribution from the file metadata:

- Database: `FABIAN head-related transfer function database`
- Organization: Audio Communication Group, TU Berlin
- License: CC-BY 4.0
- Original dataset DOI: `10.14279/depositonce-5718.4`

### Recommended setup

Run the helper script. It will use the bundled SOFA file by default, and you can still override it with `PW_MIXER_SOFA_FILE` when needed:

```bash
./setup.sh
```

By default the script installs the rendered config to:

```bash
${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/sp_2.conf
```

### Manual setup

```bash
mkdir -p "${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d"
sed "s|@SOFA_FILE@|$(pwd)/assets/sofa/FABIAN_HRIR_modeled_HATO_0.sofa|g" sp_2.conf \
  > "${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d/sp_2.conf"
```

Then restart PipeWire. For systemd user sessions:

```bash
systemctl --user restart pipewire pipewire-pulse
```

## Run

```bash
./build/pw-3d-mixer
```

Verify the filter-chain is visible:

```bash
pw-cli ls Node | grep multi_spatial
```

## Spatial Notification CLI

```bash
./build/spatial-notification \
  --file /path/to/notify.mp3 \
  --channel 3 \
  --azimuth 45 \
  --elevation 10
```

Options:

- `--channel 1..8` maps to `spk1..spk8`
- `--azimuth` uses degrees in the 0..360 range
- `--elevation` uses degrees in the -90..90 range
- `ffmpeg` must be available to decode compressed audio files

## Installing

```bash
meson install -C build
```

This installs:

- `pw-3d-mixer`
- the `pw-3d-mixer.desktop` launcher

## Repository Notes

- Generated Meson output, editor settings, and LaTeX build artifacts are ignored
- The tracked PipeWire config is now a template, not a single-user machine dump
- The existing uncommitted edit in `pipewire.c` was left untouched
