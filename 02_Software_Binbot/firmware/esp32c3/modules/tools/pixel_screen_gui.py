#!/usr/bin/env python3
"""PyQt6 front end for the Binbot pixel-screen asset converter."""

import sys
from pathlib import Path

from PIL import Image
from PyQt6.QtCore import QTimer, Qt
from PyQt6.QtGui import QColor, QImage, QPixmap
from PyQt6.QtWidgets import (
    QApplication,
    QCheckBox,
    QColorDialog,
    QComboBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

import pixel_screen


class PixelScreenWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Binbot Pixel Screen Studio")
        self.resize(1080, 760)
        self.source_path = None
        self.frames = []
        self.durations = []
        self.preview_index = 0
        self.preview_timer = QTimer(self)
        self.preview_timer.timeout.connect(self.show_next_frame)
        self.refresh_timer = QTimer(self)
        self.refresh_timer.setSingleShot(True)
        self.refresh_timer.timeout.connect(self.refresh_preview)
        self.build_ui()
        self.apply_style()

    def build_ui(self):
        root = QWidget()
        self.setCentralWidget(root)
        layout = QVBoxLayout(root)

        toolbar = QHBoxLayout()
        self.open_button = QPushButton("Open photo / GIF")
        self.open_button.clicked.connect(self.open_source)
        toolbar.addWidget(self.open_button)
        self.generate_button = QPushButton("Generate assets")
        self.generate_button.clicked.connect(self.generate_assets)
        toolbar.addWidget(self.generate_button)
        self.output_edit = QLineEdit()
        self.output_edit.setPlaceholderText("Output GIF path")
        toolbar.addWidget(self.output_edit, 1)
        layout.addLayout(toolbar)

        preview_row = QHBoxLayout()
        self.original_label = self.make_preview_label("Original")
        self.pixel_label = self.make_preview_label("Pixel preview")
        preview_row.addWidget(self.original_label, 1)
        preview_row.addWidget(self.pixel_label, 1)
        layout.addLayout(preview_row, 1)

        settings = QFormLayout()
        self.pixel_size = QSpinBox()
        self.pixel_size.setRange(1, 16)
        self.pixel_size.setValue(4)
        self.pixel_size.setSuffix(" px")
        settings.addRow("Pixel block", self.pixel_size)

        self.colors = QComboBox()
        self.colors.addItems(["16", "32", "64", "128", "256"])
        self.colors.setCurrentText("32")
        settings.addRow("Colors", self.colors)

        self.fit = QComboBox()
        self.fit.addItems(["cover", "contain"])
        settings.addRow("Fit", self.fit)

        self.background_edit = QLineEdit("#000000")
        self.background_edit.setMaxLength(9)
        color_button = QPushButton("Pick")
        color_button.clicked.connect(self.pick_background)
        background_row = QHBoxLayout()
        background_row.addWidget(self.background_edit)
        background_row.addWidget(color_button)
        settings.addRow("Background", background_row)

        self.circle_check = QCheckBox("Round mask")
        self.circle_check.setChecked(True)
        settings.addRow("Shape", self.circle_check)
        self.dither_check = QCheckBox("Floyd-Steinberg")
        settings.addRow("Dither", self.dither_check)

        self.max_frames = QSpinBox()
        self.max_frames.setRange(1, 200)
        self.max_frames.setValue(60)
        settings.addRow("Max GIF frames", self.max_frames)
        layout.addLayout(settings)

        bottom = QHBoxLayout()
        self.rgb565_check = QCheckBox("Also write RGB565")
        bottom.addWidget(self.rgb565_check)
        self.status_label = QLabel("Open an image to begin.")
        bottom.addWidget(self.status_label, 1)
        layout.addLayout(bottom)

        for control in (self.pixel_size, self.colors, self.fit,
                        self.background_edit, self.circle_check,
                        self.dither_check, self.max_frames):
            if hasattr(control, "valueChanged"):
                control.valueChanged.connect(self.schedule_preview)
            elif hasattr(control, "currentTextChanged"):
                control.currentTextChanged.connect(self.schedule_preview)
            elif hasattr(control, "textChanged"):
                control.textChanged.connect(self.schedule_preview)
            else:
                control.stateChanged.connect(self.schedule_preview)

    @staticmethod
    def make_preview_label(title):
        label = QLabel(title)
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        label.setMinimumSize(360, 360)
        label.setStyleSheet("background: #111820; border: 1px solid #344454;")
        return label

    def apply_style(self):
        self.setStyleSheet(
            "QMainWindow { background: #202830; color: #e8eef2; }"
            "QLabel, QCheckBox { color: #e8eef2; }"
            "QPushButton { padding: 8px 14px; background: #2e8b9d;"
            " color: white; border: 0; border-radius: 3px; }"
            "QPushButton:hover { background: #3ba9bd; }"
            "QLineEdit, QComboBox, QSpinBox { padding: 6px;"
            " background: #10161b; color: #e8eef2; border: 1px solid #4b6070; }"
        )

    def pick_background(self):
        color = QColorDialog.getColor(QColor(self.background_edit.text()), self)
        if color.isValid():
            self.background_edit.setText(color.name())

    def open_source(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Open image", "",
            "Images (*.jpg *.jpeg *.png *.bmp *.gif);;All files (*)")
        if not path:
            return
        self.source_path = Path(path)
        assets_dir = Path(__file__).resolve().parent.parent / "assets"
        default_output = assets_dir / (self.source_path.stem + "_pixel.gif")
        self.output_edit.setText(str(default_output))
        try:
            with Image.open(self.source_path) as image:
                self.show_image(self.original_label, image.convert("RGB"))
            self.refresh_preview()
        except (OSError, ValueError) as error:
            QMessageBox.critical(self, "Open failed", str(error))

    def schedule_preview(self):
        if self.source_path is not None:
            self.refresh_timer.start(220)

    def current_settings(self):
        background = pixel_screen.parse_color(self.background_edit.text())
        return {
            "size": 240,
            "pixel_size": self.pixel_size.value(),
            "colors": int(self.colors.currentText()),
            "dither": self.dither_check.isChecked(),
            "background": background,
            "circle": self.circle_check.isChecked(),
            "fit": self.fit.currentText(),
            "max_frames": self.max_frames.value(),
        }

    def refresh_preview(self):
        if self.source_path is None:
            return
        try:
            settings = self.current_settings()
            with Image.open(self.source_path) as source:
                frames, _ = pixel_screen.load_frames(
                    source, size=settings["size"], max_frames=1,
                    **{key: settings[key] for key in (
                        "pixel_size", "colors", "dither", "background",
                        "circle", "fit")})
            self.show_image(self.pixel_label, frames[0])
            self.preview_timer.stop()
            self.frames = []
            self.status_label.setText("Preview ready. Click Generate assets.")
        except (OSError, ValueError) as error:
            self.status_label.setText("Preview error: " + str(error))

    def show_image(self, label, image):
        image = image.convert("RGBA")
        data = image.tobytes("raw", "RGBA")
        qimage = QImage(data, image.width, image.height, image.width * 4,
                        QImage.Format.Format_RGBA8888).copy()
        pixmap = QPixmap.fromImage(qimage)
        label.setPixmap(pixmap.scaled(
            label.size(), Qt.AspectRatioMode.KeepAspectRatio,
            Qt.TransformationMode.FastTransformation))

    def generate_assets(self):
        if self.source_path is None:
            QMessageBox.warning(self, "No input", "Open a photo or GIF first.")
            return
        output_text = self.output_edit.text().strip()
        if not output_text:
            QMessageBox.warning(self, "No output", "Choose an output GIF path.")
            return
        try:
            settings = self.current_settings()
            output = Path(output_text)
            output.parent.mkdir(parents=True, exist_ok=True)
            preview = output.with_name(output.stem + "_preview.png")
            with Image.open(self.source_path) as source:
                self.frames, self.durations = pixel_screen.load_frames(
                    source, **settings)
            pixel_screen.save_gif(self.frames, self.durations, output,
                                  settings["colors"])
            self.frames[0].save(preview, format="PNG")
            raw_path = None
            if self.rgb565_check.isChecked():
                raw_path = output.with_suffix(".rgb565")
                raw_path.write_bytes(pixel_screen.rgb565_bytes(self.frames[0]))
            self.preview_index = 0
            self.show_image(self.pixel_label, self.frames[0])
            self.preview_timer.start(max(20, self.durations[0]))
            message = "Generated {} frame(s): {}".format(len(self.frames), output)
            if raw_path:
                message += " | " + str(raw_path)
            self.status_label.setText(message)
        except (OSError, ValueError) as error:
            QMessageBox.critical(self, "Generate failed", str(error))

    def show_next_frame(self):
        if not self.frames:
            return
        self.preview_index = (self.preview_index + 1) % len(self.frames)
        self.show_image(self.pixel_label, self.frames[self.preview_index])
        self.preview_timer.start(max(20, self.durations[self.preview_index]))


def main():
    app = QApplication(sys.argv)
    window = PixelScreenWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
