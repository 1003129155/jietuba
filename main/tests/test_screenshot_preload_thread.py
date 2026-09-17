# -*- coding: utf-8 -*-
"""截图模块预加载：真的在后台线程里 import 整条 ui.screenshot_window 依赖链。

以前这一步只打日志、不 import，首次截图的模块加载其实是被 main_app 顶层
同步 import 过了，预加载线程白跑。现在改成真的在 QThread 里
`from ui.screenshot_window import ScreenshotWindow`，这条链会拖出 canvas、
ui.toolbar、ui.magnifier、ui.mask_overlay、ui.selection_info 和 tools 包下
全部工具类——这些模块的定义阶段（类体、模块顶层）不该构造任何要求绑在主
线程上的原生 Qt 资源（QPixmap/QIcon 这类），否则会在后台线程上触发未定义
行为，而且多半只在特定平台/时机下偶发，日常开发很难撞上。

这里不是重新抄一份 import 列表去猜"应该没问题"，而是直接调用生产代码用的
那个 PreloadManager._preload_screenshot_modules，在真的 QThread 里跑一遍，
用 Qt 的消息处理器兜底：以后谁往这条链的某个模块顶层/类体里加一行
`ICON = QIcon(...)`，这个测试会先炸，不用等它在生产环境上表现成一次说不清
哪次启动会撞上的偶发崩溃。
"""
import pytest
from PySide6.QtCore import QObject, qInstallMessageHandler
from PySide6.QtWidgets import QApplication


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance()
    if app is None:
        app = QApplication([])
    yield app


class _FakeApp(QObject):
    """PreloadManager 只需要 app 能挂一个线程引用、有 config_manager 属性。

    真正的 MainApp 要连一整套窗口/托盘/剪贴板，这里不需要——被测的这一步
    (`_preload_screenshot_modules`) 只碰得到 `self.app`，不会碰 `self.config`。
    继承 QObject 是因为 ScreenshotPreloadThread(self.app) 把它当 QThread 的
    parent 用，QThread 的构造函数只认 QObject。
    """
    config_manager = None


def test_the_screenshot_module_chain_imports_cleanly_off_the_main_thread(qapp):
    """预加载线程真正 import 这条链时，不能在后台线程上炸出 Qt 警告/错误。

    只测 _preload_screenshot_modules 这一步本身：把它的 finished 回调换成
    空操作，不让它接着触发 OCR/工具栏/设置窗口那一整条后续链——那些不是这
    个测试要验证的东西，牵进来只会让测试变脆、变慢。
    """
    from core.bootstrap import PreloadManager

    manager = PreloadManager(_FakeApp())
    manager._run_next = lambda: None

    messages = []
    previous_handler = qInstallMessageHandler(lambda mode, ctx, msg: messages.append(msg))
    try:
        is_async = manager._preload_screenshot_modules()
        assert is_async is True, "这一步应该是异步的（返回 True），否则下面等的就是个空线程"

        thread = manager.app._screenshot_preload_thread
        assert thread.wait(15000), "预加载线程 15 秒内没跑完，import 链本身卡住了"
    finally:
        qInstallMessageHandler(previous_handler)

    assert messages == [], \
        f"截图模块 import 链在后台线程里跑出了 Qt 警告/错误: {messages}"

    # run() 内部把导入包在 try/except 里、失败只走 log_warning，不会让上面的
    # wait() 或消息处理器直接看出"其实没导入成功"——这里补一道独立的检查：
    # 真的完整跑到了 ScreenshotWindow 那一步，而不是半路吞掉异常提前退出。
    import sys
    assert "ui.screenshot_window" in sys.modules, \
        "后台线程没能把 ui.screenshot_window 真正 import 进来（异常被内部吞掉了？）"


def test_no_gui_thread_only_resource_is_built_at_import_time(qapp):
    """静态兜底：QPixmap/QIcon/QBitmap 这几种必须绑在主线程上的原生资源，
    不能在这条 import 链任何一个模块的顶层/类体里构造。

    上面那个测试是"真的跑一遍、看 Qt 报不报警告"——这里补一道跟平台无关的
    静态检查，理由是实测过：offscreen 平台插件下，即便真在后台线程构造一个
    QPixmap，Qt 也不报警告、不崩溃，运行时那道检查在这台开发机上是哑的。
    生产用的 Windows 原生平台插件（GDI 后端）会不会真的出事，这台机器测不
    出来，不能只赌"没报错就是没事"。这里换个不依赖平台后端的角度：不管 Qt
    答不答应，只要源码里真写出了"类体/模块顶层构造一个 QPixmap"这种一定会
    在后台线程上执行到的代码，就先拦下来，不等它在某台用户机器上偶发验证。

    QColor/QFont 不在查禁之列——它们是纯值类型，Qt 文档没有把它们算进
    "必须在 GUI 线程构造"那一类；QImage 同理（纯像素缓冲区，没有原生窗口
    系统句柄）。真正被 Qt 明确要求绑主线程的是持有原生 pixmap 句柄的这几个：
    QPixmap、QBitmap（QPixmap 的子类）、QIcon（内部常常包着 QPixmap）。
    """
    import ast
    import sys
    from pathlib import Path

    # 保证这条链已经被 import 过——静态扫描看的是源码本身，跟"这次是不是刚
    # 被这个测试导入的"无关，不必像上一个测试那样特意在后台线程里跑一遍。
    import ui.screenshot_window  # noqa: F401

    main_dir = Path(__file__).resolve().parents[1]
    # 与 bootstrap.py 里 _preload_screenshot_modules 文档字符串描述的这条
    # import 链保持一致：canvas、ui.toolbar、ui.magnifier、ui.mask_overlay、
    # ui.selection_info、tools 包、ui.screenshot_window 本身、capture 服务。
    chain_prefixes = (
        "canvas", "ui.toolbar", "ui.magnifier", "ui.mask_overlay",
        "ui.selection_info", "tools", "ui.screenshot_window",
        "capture.capture_service",
    )
    risky_calls = {"QPixmap", "QBitmap", "QIcon"}

    def call_name(node):
        if not isinstance(node, ast.Call):
            return None
        func = node.func
        if isinstance(func, ast.Name):
            return func.id
        if isinstance(func, ast.Attribute):
            return func.attr
        return None

    def scan(body, where, module_name, offenders):
        for stmt in body:
            if isinstance(stmt, (ast.Assign, ast.AnnAssign)) and stmt.value is not None:
                fn = call_name(stmt.value)
                if fn in risky_calls:
                    offenders.append(f"{module_name}:{stmt.lineno} ({where}) 构造了 {fn}(...)")
            if isinstance(stmt, ast.ClassDef):
                # 只往下钻 class 的直接语句体（类体本身在 import 时执行）；
                # def 方法体不钻——那些要等真正调用时才跑，不在这次风险范围内。
                scan(stmt.body, f"class {stmt.name}", module_name, offenders)

    offenders = []
    scan_failures = []
    scanned_count = 0
    for name, module in sorted(sys.modules.items()):
        if not any(name == prefix or name.startswith(prefix + ".") for prefix in chain_prefixes):
            continue
        path = getattr(module, "__file__", None)
        if not path:
            continue
        resolved = Path(path).resolve()
        try:
            resolved.relative_to(main_dir)
        except ValueError:
            continue  # 不是项目自己的模块（标准库/第三方不归我们管）

        try:
            # utf-8-sig，不是 utf-8：这个代码库的源文件基本都带 BOM
            # （﻿），ast.parse 吃到一个当普通字符解出来的 BOM 会直接
            # SyntaxError——第一版就是栽在这上面，看着通过、其实每个文件
            # 都在这里被吞掉，从没真的扫描过一行。
            tree = ast.parse(resolved.read_text(encoding="utf-8-sig"), filename=str(resolved))
        except (SyntaxError, OSError) as e:
            scan_failures.append(f"{name} ({resolved}): {e}")
            continue

        scan(tree.body, "module level", name, offenders)
        scanned_count += 1

    # 解析失败不能像上面那样悄悄 continue 掉——那正是第一版的坑：BOM 导致
    # 每个文件都解析失败，然后被这个 continue 吃掉，最后 offenders 是空的、
    # 测试通过，但其实一行都没扫描过。这里把它变成一个响亮的失败。
    assert scan_failures == [], "这些文件解析失败，扫描没有真的覆盖到它们:\n" + "\n".join(scan_failures)
    # 同样是防"看着测了、其实没测"：chain_prefixes 要是哪天和真实模块名对
    # 不上了（比如谁重命名了包），上面的循环会静静地一个文件都不匹配，
    # offenders 永远是空列表，测试却还是绿的。用一个下限兜住这种情况。
    assert scanned_count >= 20, f"只扫到 {scanned_count} 个文件，chain_prefixes 可能和实际模块名对不上了"

    assert offenders == [], (
        "这些地方在模块顶层/类体里构造了必须绑主线程的原生资源，"
        "预加载线程把它们导入进来是危险的:\n" + "\n".join(offenders)
    )
