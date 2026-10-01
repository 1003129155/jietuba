from PySide6.QtCore import Signal, Qt
from PySide6.QtWidgets import QDialog, QVBoxLayout, QLabel, QProgressBar, QPushButton, QHBoxLayout
from core.resource_manager import ResourceManager
from ui.fluent_lite import TextEdit


class UpdateDialog(QDialog):
    update_requested = Signal()
    cancel_requested = Signal()

    def __init__(self, release):
        super().__init__()
        self.setWindowTitle(self.tr("Update Available"))
        # Use the application icon from the shared rasterized resource cache.
        self.setWindowIcon(ResourceManager.get_icon_by_name("托盘.svg"))
        self.resize(620, 460)
        layout = QVBoxLayout(self)
        self.notes = TextEdit(self)
        self.notes.setReadOnly(True)
        self.notes.setPlainText(release.tag_name + "\n\n" + (release.notes or self.tr("No release notes provided.")))
        layout.addWidget(self.notes)
        self.status = QLabel(self.tr("The application will restart after the download finishes."), self)
        self.status.setWordWrap(True)
        layout.addWidget(self.status)
        self.progress = QProgressBar(self)
        self.progress.setRange(0, 100)
        layout.addWidget(self.progress)
        row = QHBoxLayout()
        self.action = QPushButton(self.tr("Update and Restart"), self)
        self.action.clicked.connect(self.update_requested)
        self.cancel_button = QPushButton(self.tr("Cancel"), self)
        self.cancel_button.clicked.connect(self.close)
        row.addStretch(1)
        row.addWidget(self.action)
        row.addWidget(self.cancel_button)
        layout.addLayout(row)
        self.working = False
        self.committed = False
        self._modal_committed = False
        self._shutdown = False

    def finish_handoff(self):
        """Allow Qt to close the dialog once the worker has accepted the handoff."""
        self._shutdown = True
        self.close()

    def set_working(self, working):
        self.working = working
        self.action.setEnabled(not working)
        self.cancel_button.setEnabled(not self.committed)
        if self._modal_committed != self.committed:
            # Block new tasks after handoff; settings have already been saved or discarded.
            visible = self.isVisible()
            if visible:
                self.hide()
            self.setWindowModality(Qt.WindowModality.ApplicationModal if self.committed else Qt.WindowModality.NonModal)
            self._modal_committed = self.committed
            if visible:
                self.show()

    def closeEvent(self, event):
        if self._shutdown:
            event.accept()
            return
        if self.committed:
            event.ignore()
            return
        self.cancel_requested.emit()
        event.accept()

    def reject(self):
        self.close()
