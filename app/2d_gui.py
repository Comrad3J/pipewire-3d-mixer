import sys
import json
import math
import subprocess
from PyQt5.QtWidgets import QApplication, QGraphicsView, QGraphicsScene, QGraphicsEllipseItem, QGraphicsItem, QMainWindow
from PyQt5.QtGui import QBrush, QPen, QPainter
from PyQt5.QtCore import Qt, QPointF, QTimer



class DraggableSource(QGraphicsEllipseItem):
    def __init__(self, channel_index, radius=6, send_info_callback=None):
        super().__init__(-radius, -radius, 2 * radius, 2 * radius)
        self.channel_index = channel_index
        self.setBrush(QBrush(Qt.red))
        self.setFlags(QGraphicsItem.ItemIsMovable | QGraphicsItem.ItemSendsScenePositionChanges)
        self.setZValue(1)
        self.max_radius = 100
        self.send_info_callback = send_info_callback
        self.last_sent_azimuth = None
        self.last_sent_radius = None

        # Throttle sending updates
        self.pending_azimuth = None
        self.pending_radius = None
        self.timer = QTimer()
        self.timer.setInterval(50)  # Limit to every 50 ms
        self.timer.timeout.connect(self._maybe_send)
        self.timer.start()

    def itemChange(self, change, value):
        if change == QGraphicsItem.ItemPositionChange:
            new_pos = value
            vector = new_pos
            dist = math.hypot(vector.x(), vector.y())

            # Constrain movement within the circle
            if dist > self.max_radius:
                angle = math.atan2(vector.y(), vector.x())
                new_pos = QPointF(
                    self.max_radius * math.cos(angle),
                    self.max_radius * math.sin(angle)
                )

            # Calculate azimuth (0° = top, clockwise)
            azimuth = (math.degrees(math.atan2(-new_pos.x(), -new_pos.y())) % 360)
            self.pending_azimuth = round(azimuth)

            ## Calcilate the distane from the center
            dist = math.hypot(new_pos.x(), new_pos.y())  # Distance from center
            radius_norm = (dist / self.max_radius) * 10  # Normalize to 0-10
            self.pending_radius = radius_norm

            return new_pos

        return super().itemChange(change, value)

    def _maybe_send(self):
        az_changed = (
            self.pending_azimuth is not None and
            self.pending_azimuth != self.last_sent_azimuth
        )

        rad_changed = (
            self.pending_radius is not None and
            0 <= self.pending_radius <= 10 and
            self.pending_radius != self.last_sent_radius
        )

        if az_changed or rad_changed:
            azimuth = self.pending_azimuth if az_changed else self.last_sent_azimuth
            radius = self.pending_radius if rad_changed else self.last_sent_radius

            if self.send_info_callback:
                self.send_info_callback(self.channel_index, azimuth, radius)
                print(f"[GUI] Sending to channel {self.channel_index}: azimuth = {azimuth}°, radius = {radius}")

            if az_changed:
                self.last_sent_azimuth = azimuth
                self.pending_azimuth = None

            if rad_changed:
                self.last_sent_radius = radius
                self.pending_radius = None


        


class MapView(QGraphicsView):
    def __init__(self, send_info_callback, n_channels):
        super().__init__()
        self.setScene(QGraphicsScene(-150, -150, 300, 300))
        self.setRenderHint(QPainter.Antialiasing)

        self.sources = [] 

        # Draw center
        center = QGraphicsEllipseItem(-3, -3, 6, 6)
        center.setBrush(QBrush(Qt.black))
        self.scene().addItem(center)

        # Draw boundary circle
        boundary = QGraphicsEllipseItem(-100, -100, 200, 200)
        boundary.setPen(QPen(Qt.darkGray, 2, Qt.DashLine))
        self.scene().addItem(boundary)

        # Add n draggable sources
        for i in range(n_channels):
            angle_deg = (360 / n_channels) * i
            angle_rad = math.radians(angle_deg)
            x = 60 * math.cos(angle_rad)
            y = 60 * math.sin(angle_rad)

            source = DraggableSource(channel_index=i+1, send_info_callback=send_info_callback)
            source.setPos(x, y)
            self.scene().addItem(source)
            self.sources.append(source)



class PipeWireControl(QMainWindow):
    def __init__(self, node_id, n_channels):
        super().__init__()
        self.node_id = node_id
        self.n_channels = n_channels
        self.setWindowTitle("2D Azimuth Controller")
        self.setGeometry(100, 100, 400, 400)
        
        # Initialize and set central widget
        self.map_view = MapView(self.send_position_to_pipewire, n_channels)
        self.setCentralWidget(self.map_view)

    def radius_to_gain(self, radius):
        ## Avoid log(0) issue and make it rad max 10 
        radius_clamped = max(0.1, min(radius, 10))
        falloff_compensation = 3.0 # Higher value means more falloff compensation
        gain = 1 / ((radius_clamped ** 2)) * falloff_compensation  # inverse-square law
        gain = min(gain, 10)
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

# Replce with an universal method that also return n_channels and also the name of connected source
def get_node_id_by_name(target_name):
    output = subprocess.check_output(["pw-dump"], text=True)
    nodes = json.loads(output)
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Node":
            info = node.get("info", {})
            props = info.get("props", {})
            if props.get("node.name") == target_name:
                return node["id"]
    return None

def get_node_info(target_name):
    output = subprocess.check_output(["pw-dump"], text=True)
    nodes = json.loads(output)
    for node in nodes:
        if node.get("type") == "PipeWire:Interface:Node":
            info = node.get("info", {})
            props = info.get("props", {})
            if props.get("node.name") == target_name:
                return {
                    "id": node["id"],
                    "n_channels": node["info"]["n-input-ports"]
                }
    return None



if __name__ == "__main__":
    node_id = get_node_info("effect_input.multi_spatial")["id"]
    n_channels = get_node_info("effect_input.multi_spatial")["n_channels"]
    print(f"[GUI] Found node ID: {node_id} with {n_channels} channels")
    if node_id is None:
        print("Error: Could not find multi_spatial node")
        sys.exit(1)

    app = QApplication(sys.argv)
    window = PipeWireControl(node_id, n_channels)
    window.show()
    sys.exit(app.exec_())
