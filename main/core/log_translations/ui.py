"""main/ui/ 目录下 log_* 调用的中→英翻译表。"""

TRANSLATIONS: dict[str, str] = {
    # -- ui/magnifier.py --
    "调整倍数: {zoom_factor:.2f}x": "Zoom adjusted: {zoom_factor:.2f}x",

    # -- ui/screenshot_window.py --
    "虚拟桌面: {width}x{height} at ({x}, {y})": "Virtual desktop: {width}x{height} at ({x}, {y})",
    "图像尺寸: {width}x{height}": "Image size: {width}x{height}",
    "[计时] 初始化完成 | {parts}": "[Timing] Initialization complete | {parts}",
    "[计时] 复用窗口会话准备完成 | 耗时={elapsed:.1f}ms": "[Timing] Reused window session ready | elapsed={elapsed:.1f}ms",
    "开始释放截图会话资源（保留 UI 壳）": "Releasing screenshot session resources (keeping UI shell)",
    "恢复 pin 窗口": "Restoring pinned windows",
    "截图会话资源释放完成": "Screenshot session resources released",
    "SetWindowDisplayAffinity({mode}) 成功": "SetWindowDisplayAffinity({mode}) succeeded",
    "SetWindowDisplayAffinity 失败, GetLastError={error_code}": "SetWindowDisplayAffinity failed, GetLastError={error_code}",
    "shiboken6 有效性检查": "shiboken6 validity check",
    "智能选区初始化: 鼠标位置({x}, {y}) -> 选区{rect}": "Smart selection initialized: cursor position ({x}, {y}) -> selection {rect}",
    "放大镜初始化: 位置({x}, {y})": "Magnifier initialized: position ({x}, {y})",
    "箭头样式已更新: {style}": "Arrow style updated: {style}",
    "线条样式已更新: {style}": "Line style updated: {style}",
    "启动GIF录制模式": "Starting GIF recording mode",
    "GIF录制区域: x={x}, y={y}, w={w}, h={h}": "GIF recording area: x={x}, y={y}, w={w}, h={h}",
    "关闭旧 GIF 窗口": "Closing old GIF window",
    "GifRecordWindow已创建, overlay visible={visible}": "GifRecordWindow created, overlay visible={visible}",
    "GIF录制窗口已启动": "GIF recording window started",
    "启动长截图模式": "Starting long screenshot mode",
    "selection_rect（场景坐标）: x={x}, y={y}, w={w}, h={h}": "selection_rect (scene coordinates): x={x}, y={y}, w={w}, h={h}",
    "virtual偏移: x={x}, y={y}": "virtual offset: x={x}, y={y}",
    "选中区域（屏幕坐标）: x={x}, y={y}, w={w}, h={h}": "Selected area (screen coordinates): x={x}, y={y}, w={w}, h={h}",
    "关闭旧滚动截图窗口": "Closing old scroll screenshot window",
    "长截图窗口创建完成，准备显示": "Long screenshot window created, preparing to show",
    "滚动截图窗口已显示并激活": "Scroll screenshot window shown and activated",
    "释放截图窗口内存": "Releasing screenshot window memory",

    # -- ui/text_settings_panel.py --
    "加载文字设置": "Loading text settings",
    "保存字体设置": "Saving font settings",
    "保存文字背景设置": "Saving text background settings",

    # -- ui/toolbar.py --
    "初始化线条样式失败: {exc}": "Failed to initialize line style: {exc}",
    "同步形状线条样式失败: {exc}": "Failed to sync shape line style: {exc}",
    "同步荧光笔模式失败: {exc}": "Failed to sync highlighter mode: {exc}",
    "保存荧光笔模式失败: {exc}": "Failed to save highlighter mode: {exc}",
    "设置高亮笔光标": "Setting highlighter cursor",

    # -- ui/welcome/wizard.py --
    "WelcomeWizard 语言初始化": "WelcomeWizard language initialization",
    "欢迎向导品牌图标": "Welcome wizard brand icon",
    "向导页面刷新翻译": "Wizard page translation refresh",

    # -- ui/welcome/page1_welcome.py --
    "加载托盘图标": "Loading tray icon",
    "加载语言": "Loading language",
    "切换界面主题": "Switching UI theme",

    # -- ui/welcome/page5_translation.py --
    "获取当前语言": "Getting current language",

    # -- ui/welcome/page6_finish.py --
    "已写入开机自启注册表项: {exe_path}": "Autostart registry entry written: {exe_path}",
    "已删除开机自启注册表项": "Autostart registry entry removed",
    "设置开机自启": "Setting autostart",
    "已创建桌面快捷方式: {desktop_lnk}": "Desktop shortcut created: {desktop_lnk}",
    "创建桌面快捷方式": "Creating desktop shortcut",

    # -- ui/selection_info/rounded_corners.py --
    "圆角截图: {state}  r={radius}": "Rounded corners: {state}  r={radius}",
    "圆角半径: {value}": "Corner radius: {value}",
    "圆角裁剪完成: r={r}": "Rounded corner crop complete: r={r}",

    # -- ui/selection_info/panel.py --
    "加载按钮图标": "Loading button icon",

    # -- ui/selection_info/lock_ratio.py --
    "锁定纵横比: ON  ratio={ratio:.4f}": "Lock aspect ratio: ON  ratio={ratio:.4f}",
    "锁定纵横比: OFF": "Lock aspect ratio: OFF",

    # -- ui/selection_info/controller.py --
    "刷新背景失败: {e}": "Failed to refresh background: {e}",

    # -- ui/selection_info/border_shadow.py --
    "描边/阴影: {state}  mode={mode} size={size}": "Border/shadow: {state}  mode={mode} size={size}",
    "模式切换: {mode}": "Mode switched: {mode}",
    "描边导出完成: size={size} corner_r={corner_radius}": "Border export complete: size={size} corner_r={corner_radius}",
    "阴影导出完成: size={size} corner_r={corner_radius}": "Shadow export complete: size={size} corner_r={corner_radius}",

    # -- ui/settings_ui/dialog.py --
    "设置窗口图标": "Setting window icon",
    "加载 Logo 图标": "Loading logo icon",
    "重载 Pin 快捷键绑定": "Reloading pin shortcut bindings",
    "重置标题栏按钮状态": "Resetting title bar button state",
    "设置任务栏图标": "Setting taskbar icon",

    # -- ui/settings_ui/page_clipboard.py --
    "获取剪贴板数据库路径": "Getting clipboard database path",
    "计算图片目录大小": "Calculating image directory size",
    "计算剪贴板存储大小": "Calculating clipboard storage size",
}
