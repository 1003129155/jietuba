import threading

_lock = threading.Lock()
_active = 0
_paused = False


def busy():
    with _lock:
        return _active > 0


def pause_if_idle():
    global _paused
    with _lock:
        if _active:
            return False
        _paused = True
        return True


def resume():
    global _paused
    with _lock:
        _paused = False


def start_thread(target, *, name=None):
    global _active
    with _lock:
        if _paused:
            raise RuntimeError("Application is preparing to update")
        _active += 1

    def run():
        global _active
        try:
            target()
        finally:
            with _lock:
                _active -= 1

    thread = threading.Thread(target=run, daemon=True, name=name)
    try:
        thread.start()
    except Exception:
        with _lock:
            _active -= 1
        raise
    return thread
