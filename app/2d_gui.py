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

        self.sources = []
        n_channels = len(channel_labels)

        # Draw center
        center = QGraphicsEllipseItem(-4, -4, 16, 16)
        center.setBrush(QBrush(Qt.black))
        self.scene().addItem(center)

        max_radius = 250  # must match DraggableSource

        min_m = 0.1 
        max_m = 100
        for meter in [1, 10, 100]:
            r = (math.log10(meter) - math.log10(min_m)) / (math.log10(max_m) - math.log10(min_m)) * max_radius
            circle = QGraphicsEllipseItem(-r, -r, 2 * r, 2 * r)
            circle.setPen(QPen(Qt.gray, 1, Qt.DotLine))
            self.scene().addItem(circle)
            # Add label for radius
            label = self.scene().addText(f"{meter} m")
            font = label.font()
            font.setPointSize(7)
            label.setFont(font)
            label.setDefaultTextColor(Qt.gray)
            label.setZValue(2)
            label_rect = label.boundingRect()
            label.setPos(-label_rect.width() / 2, -r - label_rect.height())

        # Add n draggable sources
        for i in range(n_channels):
            angle_deg = (360 / n_channels) * i
            angle_rad = math.radians(angle_deg)
            x = 120 * math.cos(angle_rad)
            y = 129 * math.sin(angle_rad)

            source = DraggableSource(channel_index=i+1, send_info_callback=send_info_callback)
            source.setPos(x, y)
            self.scene().addItem(source)
            self.sources.append(source)

            # text labelling
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
            font.setPointSize(7)  # Slightly smaller font
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

        self.setWindowTitle("2D Azimuth Controller")
        self.setGeometry(100, 100, 700, 600)
        
        # Main layout
        main_widget = QWidget()
        main_layout = QHBoxLayout(main_widget)
        main_layout.setContentsMargins(0, 0, 0, 0)

        # Control column (left)
        control_panel = QWidget()
        control_panel.setFixedWidth(150)
        control_layout = QVBoxLayout(control_panel)
        control_layout.setContentsMargins(10, 10, 10, 10)

        # Falloff compensation slider
        self.falloff_label = QLabel(f"Falloff: {self.falloff_compensation:.2f}")
        control_layout.addWidget(self.falloff_label)
        self.falloff_slider = QSlider(Qt.Horizontal)
        self.falloff_slider.setMinimum(10)   # Represents 1.0
        self.falloff_slider.setMaximum(100)  # Represents 10.0
        self.falloff_slider.setValue(int(self.falloff_compensation * 10))
        self.falloff_slider.setTickInterval(10)
        self.falloff_slider.setTickPosition(QSlider.TicksBelow)
        self.falloff_slider.valueChanged.connect(self.update_falloff)
        control_layout.addWidget(self.falloff_slider)

        control_layout.addStretch()
        main_layout.addWidget(control_panel)
        self.map_view = MapView(self.send_position_to_pipewire, channel_labels)
        main_layout.addWidget(self.map_view)
        self.setCentralWidget(main_widget)

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

def get_node_info(target_name):
    output = subprocess.check_output(["pw-dump"], text=True)
    nodes = json.loads(output)

    target_node_id = None
    n_channels = None
    port_ids = []

    # First, find the target node and its ID
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Node":
            info = node.get("info", {})
            props = info.get("props", {})
            if props.get("node.name") == target_name:
                target_node_id = node["id"]
                n_channels = info.get("n-input-ports", 0)
                break

    if target_node_id is None:
        return None

    # Second, map input-port-id to output-port-id (from links)
    input_to_output_port = {}
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Link":
            info = node.get("info", {})
            if info.get("input-node-id") == target_node_id:
                input_port = info.get("input-port-id")
                output_port = info.get("output-port-id")
                input_to_output_port[input_port] = output_port

    # Third, map output-port-id to port.alias or port.name (from ports)
    output_port_to_alias = {}
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Port":
            info = node.get("info", {})
            port_id = node.get("id")
            props = info.get("props", {})
            alias = props.get("port.alias") or props.get("port.name")
            if alias:
                output_port_to_alias[port_id] = alias

    # Final: map input-port-id to alias via output-port-id
    input_port_to_alias = {}
    for in_port, out_port in input_to_output_port.items():
        alias = output_port_to_alias.get(out_port, "Unknown")
        input_port_to_alias[in_port] = alias

    # Sort input ports and build final numbered map
    sorted_inputs = sorted(input_port_to_alias.items())
    channel_map = {in_port: idx + 1 for idx, (in_port, _) in enumerate(sorted_inputs)}
    labeled_channels = {channel_map[in_port]: alias for in_port, alias in sorted_inputs}

    return {
        "id": target_node_id,
        "n_channels": n_channels,
        "input_port_id_to_channel_map": channel_map,
        "channel_labels": labeled_channels
    }




if __name__ == "__main__":
    # the target name is set in the config file as media.name
    # go through which of these informations you actually need to fetch
    
    info = get_node_info("effect_input.multi_spatial")
    node_id = info["id"]
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
