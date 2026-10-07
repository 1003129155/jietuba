"""在 PyInstaller 的 Qt 运行时钩子之前启动轻量录制子进程。"""

import sys


if len(sys.argv) > 1 and sys.argv[1] == "--video-recorder-worker":
    from video_worker import main
    raise SystemExit(main(sys.argv[2:]))
