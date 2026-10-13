# -*- coding: utf-8 -*-
"""剪贴板监听刚启动就停止：只挂监听、不写剪贴板。"""
import threading

import pytest


def test_stopping_right_after_starting_returns(tmp_path):
    pyclipboard = pytest.importorskip("pyclipboard")
    manager = pyclipboard.ClipboardManager(db_path=str(tmp_path / "clipboard.db"))

    def start_and_stop():
        for _ in range(30):
            manager.start_monitor(callback=None)
            manager.stop_monitor()

    worker = threading.Thread(target=start_and_stop, daemon=True)
    worker.start()
    worker.join(timeout=20)

    assert not worker.is_alive(), "stop_monitor 等不到监听线程结束"
    assert not manager.is_monitoring()
