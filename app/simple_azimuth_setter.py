import sys
import json
import subprocess
from PyQt5.QtWidgets import (QApplication, QMainWindow, QWidget, 
                            QVBoxLayout, QLabel, QSpinBox)
from PyQt5.QtCore import Qt

class AzimuthControl(QMainWindow):
    def __init__(self, node_id):
        super().__init__()
        self.node_id = node_id
        self.setWindowTitle("Speaker 1 Azimuth Control")
        self.setGeometry(100, 100, 200, 100)
        
        self.init_ui()
        
    def init_ui(self):
        main_widget = QWidget()
        layout = QVBoxLayout()
        
        # Title
        title = QLabel("Speaker 1 Azimuth (degrees):")
        title.setAlignment(Qt.AlignCenter)
        layout.addWidget(title)
        
        # SpinBox
        self.azimuth_spin = QSpinBox()
        self.azimuth_spin.setRange(0, 360)  # 0-360 degrees
        self.azimuth_spin.setValue(90)      # Default to 90°
        self.azimuth_spin.setSingleStep(1)  # 1° steps
        self.azimuth_spin.valueChanged.connect(self.update_azimuth)
        
        layout.addWidget(self.azimuth_spin)
        main_widget.setLayout(layout)
        self.setCentralWidget(main_widget)
    
    def update_azimuth(self, value):
        """Send azimuth update to PipeWire"""
        command = f'pw-cli s {self.node_id} Props \'{{ params = [ "spk1:Azimuth" {value} ] }}\''
        print(f"Setting azimuth to {value}°")
        try:
            subprocess.run(command, shell=True, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Error executing command: {e}")

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

if __name__ == "__main__":
    node_id = get_node_id_by_name("effect_input.multi_spatial")
    if node_id is None:
        print("Error: Could not find multi_spatial node")
        sys.exit(1)
    
    app = QApplication(sys.argv)
    window = AzimuthControl(node_id)
    window.show()
    sys.exit(app.exec_())