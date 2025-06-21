import sys
import json
import math
import subprocess
from PyQt5.QtWidgets import QWidget, QHBoxLayout, QVBoxLayout, QLabel, QPushButton, QSizePolicy, QSlider
from PyQt5.QtWidgets import QWidget, QApplication, QVBoxLayout, QGraphicsView, QGraphicsScene, QGraphicsEllipseItem, QGraphicsItem, QMainWindow
from PyQt5.QtGui import QBrush, QPen, QPainter
from PyQt5.QtCore import Qt, QPointF, QTimer



class DraggableSource(QGraphicsEllipseItem):
    def __init__(self, channel_index, radius=6, send_info_callback=None):
        super().__init__(-radius, -radius, 2 * radius, 2 * radius)
        self.channel_index = channel_index
        self.setBrush(QBrush(Qt.red))
        self.setFlags(QGraphicsItem.ItemIsMovable | QGraphicsItem.ItemSendsScenePositionChanges)
        self.setZValue(1)
        self.max_radius = 250
        self.send_info_callback = send_info_callback
        self.last_sent_azimuth = None
        self.last_sent_radius = None

        # Throttle sending updates
        self.pending_azimuth = None
        self.pending_radius = None
        self.timer = QTimer()
        self.timer.setInterval(10)  # Limit to every 50 ms
        self.timer.timeout.connect(self._maybe_send)
        self.timer.start()

    def itemChange(self, change, value):
        if change == QGraphicsItem.ItemPositionChange:
            new_pos = value
            vector = new_pos
            dist = math.hypot(vector.x(), vector.y())

            azimuth = math.degrees(math.atan2(-new_pos.x(), -new_pos.y()))
            if azimuth <= 0:
                azimuth += 360
            self.pending_azimuth = azimuth

            # Constrain movement within the circle
            if dist > self.max_radius:
                angle = math.atan2(vector.y(), vector.x())
                new_pos = QPointF(
                    self.max_radius * math.cos(angle),
                    self.max_radius * math.sin(angle)
                )

            dist = math.hypot(new_pos.x(), new_pos.y())

            min_m = 0.1 
            max_m = 100
            max_radius = self.max_radius

            # Clamp dist to [0, max_radius]
            dist = min(max(dist, 0), max_radius)
            # Map dist [0, max_radius] -> meters [min_m, max_m] logarithmically
            if max_radius > 0:
                log_min = math.log10(min_m)
                log_max = math.log10(max_m)
                log_m = log_min + (log_max - log_min) * (dist / max_radius)
                meters = 10 ** log_m
            else:
                meters = min_m
            self.pending_radius = meters
            return new_pos

        return super().itemChange(change, value)

    def _maybe_send(self):
        az_changed = (
            self.pending_azimuth is not None and
            self.pending_azimuth != self.last_sent_azimuth
        )

        rad_changed = (
            self.pending_radius is not None and
            0 <= self.pending_radius <= 100 and
            self.pending_radius != self.last_sent_radius
        )

        if az_changed or rad_changed:
            azimuth = self.pending_azimuth if az_changed else self.last_sent_azimuth
            radius = self.pending_radius if rad_changed else self.last_sent_radius

            if self.send_info_callback and radius is not None:
                self.send_info_callback(self.channel_index, azimuth, radius)
                print(f"[GUI] Sending to channel {self.channel_index}: azimuth = {azimuth}°, radius = {radius}")

            if az_changed:
                self.last_sent_azimuth = azimuth
                self.pending_azimuth = None

            if rad_changed:
                self.last_sent_radius = radius
                self.pending_radius = None


        
class MapView(QGraphicsView):
    def __init__(self, send_info_callback, channel_labels):
        super().__init__()

        self.setScene(QGraphicsScene(-250, -250, 500, 500))
        self.setRenderHint(QPainter.Antialiasing)

        self.send_info_callback = send_info_callback
        self.sources = []

        self._draw_background_grid()
        self.draw_sources(channel_labels)

    def _draw_background_grid(self):
        # Center point
        center = QGraphicsEllipseItem(-4, -4, 16, 16)
        center.setBrush(QBrush(Qt.black))
        self.scene().addItem(center)

        max_radius = 250
        min_m = 0.1
        max_m = 100
        for meter in [1, 10, 100]:
            r = (math.log10(meter) - math.log10(min_m)) / (math.log10(max_m) - math.log10(min_m)) * max_radius
            circle = QGraphicsEllipseItem(-r, -r, 2 * r, 2 * r)
            circle.setPen(QPen(Qt.gray, 1, Qt.DotLine))
            self.scene().addItem(circle)

            label = self.scene().addText(f"{meter} m")
            font = label.font()
            font.setPointSize(7)
            label.setFont(font)
            label.setDefaultTextColor(Qt.gray)
            label.setZValue(2)
            label_rect = label.boundingRect()
            label.setPos(-label_rect.width() / 2, -r - label_rect.height())

    def draw_sources(self, channel_labels):
        # Remove old sources
        for source in self.sources:
            self.scene().removeItem(source)
        self.sources.clear()

        n_channels = len(channel_labels)

        for i in range(n_channels):
            angle_deg = (360 / n_channels) * i
            angle_rad = math.radians(angle_deg)
            x = 120 * math.cos(angle_rad)
            y = 129 * math.sin(angle_rad)

            source = DraggableSource(channel_index=i + 1, send_info_callback=self.send_info_callback)
            source.setPos(x, y)
            self.scene().addItem(source)
            self.sources.append(source)

            # Label formatting
            label_text = channel_labels.get(i + 1, "Unknown")
            if ' ' in label_text:
                parts = label_text.rsplit(' ', 1)
                label_text_formatted = f"{parts[0]}\n{parts[1]}"
            elif ':' in label_text:
                parts = label_text.rsplit(':', 1)
                label_text_formatted = f"{parts[0]}:\n{parts[1]}"
            else:
                label_text_formatted = label_text

            label = self.scene().addText(f"SPK{i + 1}:\n{label_text_formatted}")
            font = label.font()
            font.setPointSize(7)
            label.setFont(font)
            label.setParentItem(source)
            label.setDefaultTextColor(Qt.white)
            label.setZValue(2)
            label_rect = label.boundingRect()
            label.setPos(-label_rect.width() / 2, -label_rect.height() - 5)


class PipeWireControl(QMainWindow):
    def __init__(self, node_id, channel_labels):
        super().__init__()
        self.node_id = node_id
        self.n_channels = len(channel_labels)
        self.falloff_compensation = 3.0  # Default value
        self.channel_elevations = {i+1: 0.0 for i in range(self.n_channels)}

        self.setWindowTitle("2D Azimuth Controller")
        self.setGeometry(100, 100, 700, 600)

        # Main layout
        main_widget = QWidget()
        main_layout = QHBoxLayout(main_widget)
        main_layout.setContentsMargins(0, 0, 0, 0)

        # Control column (left)
        control_panel = QWidget()
        control_panel.setFixedWidth(185)
        self.control_layout = QVBoxLayout(control_panel)
        self.control_layout.setContentsMargins(10, 10, 10, 10)

        
        #Reload config button
        reload_button = QPushButton("Reload Positions")
        reload_button.clicked.connect(self.reload_positions)
        self.control_layout.addWidget(reload_button)



        # Falloff compensation slider
        self.falloff_label = QLabel(f"Falloff: {self.falloff_compensation:.2f}")
        self.control_layout.addWidget(self.falloff_label)
        self.falloff_slider = QSlider(Qt.Horizontal)
        self.falloff_slider.setMinimum(10)   # Represents 1.0
        self.falloff_slider.setMaximum(100)  # Represents 10.0
        self.falloff_slider.setValue(int(self.falloff_compensation * 10))
        self.falloff_slider.setTickInterval(10)
        self.falloff_slider.setTickPosition(QSlider.TicksBelow)
        self.falloff_slider.valueChanged.connect(self.update_falloff)
        self.control_layout.addWidget(self.falloff_slider)

        self.control_layout.addStretch()
        main_layout.addWidget(control_panel)
        self.map_view = MapView(self.send_position_to_pipewire, channel_labels)
        main_layout.addWidget(self.map_view)
        self.setCentralWidget(main_widget)

        # Elevation sliders per source
        self.elevation_sliders = {}
        self.create_elevation_sliders(self.control_layout)


    def create_elevation_sliders(self, control_layout):
        for slider in getattr(self, 'elevation_sliders', {}).values():
            slider.deleteLater()
        self.elevation_sliders = {}

        for ch in range(1, self.n_channels + 1):
            label = QLabel(f"SPK{ch} Elevation: 0.0°")
            control_layout.addWidget(label)

            slider = QSlider(Qt.Horizontal)
            slider.setMinimum(-180)
            slider.setMaximum(180)
            slider.setValue(0)
            slider.setSingleStep(1)
            slider.setTickInterval(30)
            slider.setTickPosition(QSlider.TicksBelow)

            # Attach metadata to slider
            slider.channel_id = ch
            slider.value_label = label
            slider.last_sent_value = 0.0
            slider.pending_value = 0.0

            slider.update_timer = QTimer()
            slider.update_timer.setInterval(20)
            slider.update_timer.setSingleShot(True)
            slider.update_timer.timeout.connect(lambda s=slider: self._send_throttled_elevation(s))

            slider.valueChanged.connect(lambda val, s=slider: self._on_slider_value_changed(s, val))

            control_layout.addWidget(slider)
            self.elevation_sliders[ch] = slider

    def reload_positions(self):
        print("[GUI] Reloading positions...")

        # Step 1: Re-fetch node data
        node_info = get_node_info(self.node_id)
        if node_info is None:
            print(f"[ERROR] Node '{self.node_id}' not found.")
            return
        print(node_info)
        # Step 2: Update internal state
        new_labels = node_info["channel_labels"]
        self.n_channels = node_info["n_channels"]
        self.channel_elevations = {i + 1: 0.0 for i in range(self.n_channels)}

        # Step 3: Update MapView
        self.map_view.channel_labels = new_labels
        self.clear_sources()
        self.map_view.draw_sources(self.map_view.channel_labels)

        # Step 4: Clear and recreate elevation sliders
        self.clear_elevation_sliders()
        self.create_elevation_sliders(self.control_layout)


    def update_elevation(self, channel_id, value, label_widget):
        elevation = round(value / 10.0, 1)  # Convert to degrees
        self.channel_elevations[channel_id] = elevation
        label_widget.setText(f"SPK{channel_id} Elevation: {elevation}°")
        self.send_elevation_to_pipewire(channel_id, elevation)

    def _on_slider_value_changed(self, slider, value):
        elevation = round(value * 0.5, 1)
        slider.value_label.setText(f"SPK{slider.channel_id} Elevation: {elevation}°")
        
        # Save value, restart timer
        slider.pending_value = elevation
        slider.update_timer.start()

    def _send_throttled_elevation(self, slider):
        elevation = slider.pending_value
        if elevation != slider.last_sent_value:
            self.channel_elevations[slider.channel_id] = elevation
            self.send_elevation_to_pipewire(slider.channel_id, elevation)
            slider.last_sent_value = elevation

    def clear_sources(self):
        # Remove existing source items from the scene
        for source in self.map_view.sources:
            self.map_view.scene().removeItem(source)
        self.map_view.sources.clear()

    def clear_elevation_sliders(self):
        for slider in self.elevation_sliders.values():
            slider.value_label.deleteLater()
            slider.deleteLater()               
        self.elevation_sliders.clear()



    def update_falloff(self, value):
        self.falloff_compensation = value / 10.0
        self.falloff_label.setText(f"Falloff: {self.falloff_compensation:.2f}")


    def radius_to_gain(self, radius):
        radius_clamped = max(0.1, min(radius, 100))
        gain = 1 / ((radius_clamped ** 2)) * self.falloff_compensation  # Use instance variable
        gain = min(gain, 15)
        return round(gain, 3)

    def send_position_to_pipewire(self, channel_id, azimuth, radius):
        gain = self.radius_to_gain(radius)

        # Send azimuth
        az_cmd = f'pw-cli s {self.node_id} Props \'{{ params = [ "spk{channel_id}:Azimuth" {azimuth} ] }}\''
        print(f"[PIPEWIRE] Setting spk{channel_id}:Azimuth to {azimuth}°")
        try:
            subprocess.run(az_cmd, shell=True, check=True)
        except subprocess.CalledProcessError as e:
            print(f"[PIPEWIRE ERROR - Azimuth] {e}")

        # Send gain
        gain_cmd = f'pw-cli s {self.node_id} Props \'{{ params = [ "mixL:Gain {channel_id}" {gain} "mixR:Gain {channel_id}" {gain} ] }}\''
        print(f"[PIPEWIRE] Setting Gain {channel_id} to {gain}")
        try:
            subprocess.run(gain_cmd, shell=True, check=True)
        except subprocess.CalledProcessError as e:
            print(f"[PIPEWIRE ERROR - Gain] {e}")

    def send_elevation_to_pipewire(self, channel_id, elevation):
        cmd = f'pw-cli s {self.node_id} Props \'{{ params = [ "spk{channel_id}:Elevation" {elevation} ] }}\''
        print(f"[PIPEWIRE] Setting spk{channel_id}:Elevation to {elevation}°")
        try:
            subprocess.run(cmd, shell=True, check=True)
        except subprocess.CalledProcessError as e:
            print(f"[PIPEWIRE ERROR - Elevation] {e}")

def get_node_id(target_name):
    output = subprocess.check_output(["pw-dump"], text=True)
    nodes = json.loads(output)
    target_node_id = None
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Node":
            info = node.get("info", {})
            props = info.get("props", {})
            if props.get("node.name") == target_name:
                target_node_id = node["id"]
                n_channels = info.get("n-input-ports", 0)
                break

    if target_node_id is not None:
        return target_node_id
    else:
        return None

def get_node_info(target_id):
    output = subprocess.check_output(["pw-dump"], text=True)
    nodes = json.loads(output)

    if target_id is None:
        return None

    n_channels = None
    port_ids = []

    # Find node info by ID and extract n-input-ports
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Node" and node.get("id") == target_id:
            info = node.get("info", {})
            n_channels = info.get("n-input-ports", 0)
            break

    # Map input-port-id to output-port-id (from links)
    input_to_output_port = {}
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Link":
            info = node.get("info", {})
            if info.get("input-node-id") == target_id:
                input_port = info.get("input-port-id")
                output_port = info.get("output-port-id")
                input_to_output_port[input_port] = output_port

    # Map output-port-id to port.alias or port.name (from ports)
    output_port_to_alias = {}
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Port":
            info = node.get("info", {})
            port_id = node.get("id")
            props = info.get("props", {})
            alias = props.get("port.alias") or props.get("port.name")
            if alias:
                output_port_to_alias[port_id] = alias

    # Map input-port-id to alias via output-port-id
    input_port_to_alias = {}
    for in_port, out_port in input_to_output_port.items():
        alias = output_port_to_alias.get(out_port, "Unknown")
        input_port_to_alias[in_port] = alias

    # Sort and label channels
    sorted_inputs = sorted(input_port_to_alias.items())
    channel_map = {in_port: idx + 1 for idx, (in_port, _) in enumerate(sorted_inputs)}
    labeled_channels = {channel_map[in_port]: alias for in_port, alias in sorted_inputs}

    return {
        "n_channels": n_channels,
        "input_port_id_to_channel_map": channel_map,
        "channel_labels": labeled_channels
    }





if __name__ == "__main__":
    # the target name is set in the config file as media.name
    
    node_id = get_node_id("effect_input.multi_spatial")
    info = get_node_info(node_id)

    n_channels = info["n_channels"]
    input_port_id_to_channel_map = info["input_port_id_to_channel_map"]
    channel_labels = info["channel_labels"]

    print(f"[GUI] Found node ID: {node_id} with {n_channels} channels")
    if node_id is None:
        print("Error: Could not find multi_spatial node")
        sys.exit(1)

    app = QApplication(sys.argv)
    window = PipeWireControl(node_id, channel_labels)
    window.show()
    sys.exit(app.exec_())
