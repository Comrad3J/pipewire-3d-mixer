## ⚠️ Disclaimer

This software is under active development and intended for testing, experimentation, and feedback. Features may change without notice, and bugs are likely.

# PipeWire 3d Mixer

For now only a 2d gui version. It supports azimuth and distance control for spatial positioning of up to 8 multiple virtual audio sources in real time. It works by constructing a filter-chain of PipeWire built-in sofa spatial filters.

# Installation and configuration

1. Clone this repo
2. Update your PipeWire config
   Copy the provided `4/8_channel_spatializer.conf` file and either:

   - Replace your existing config:  
     ```bash
     cp 4_channel_spatializer.conf ~/.config/pipewire/pipewire.conf
     ```

   - **OR** manually append the `context.modules` section to your existing `~/.config/pipewire/pipewire.conf`.

2. Edit your HRTF (SOFA) file path
   In the config, locate and update the path to your SOFA file:

    - hrtf-path = /path/to/file.sofa

3. Setup Python environment
```bash
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```
4. Run the Gui
```bash
python 2d_gui.py
```

License : This project is licensed under the GNU General Public License v3.0.