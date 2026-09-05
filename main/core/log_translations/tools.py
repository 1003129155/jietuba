"""main/tools/ 目录下 log_* 调用的中→英翻译表。"""

TRANSLATIONS: dict[str, str] = {
    # action.py
    "已完成复制到剪贴板，已提交异步保存任务": "Copied to clipboard; async save task submitted",
    "已完成复制到剪贴板": "Copied to clipboard",
    "已保存到: {file_path}": "Saved to: {file_path}",
    "已创建钉图窗口": "Pin window created",
    "位置: ({pos_x}, {pos_y})": "Position: ({pos_x}, {pos_y})",
    "底图: {width}x{height}": "Base image: {width}x{height}",
    "继承绘制项目: {count} 个": "Inherited {count} drawing item(s)",
    "启动截图翻译模式": "Starting screenshot translation mode",
    "已复制底图用于OCR: {width}x{height}": "Copied base image for OCR: {width}x{height}",
    "关闭截图窗口，释放内存": "Closing screenshot window to free memory",
    "从 {tool_id} 切换到 cursor": "Switching from {tool_id} to cursor",

    # controller.py
    "初始化": "Initialized",
    "工具不存在: {tool_id}": "Tool does not exist: {tool_id}",
    "工具切换回调错误: {e}": "Tool switch callback error: {e}",

    # arrow.py
    "开始绘制: {pos}, 样式: {arrow_style}": "Started drawing: {pos}, style: {arrow_style}",
    "绘制取消：长度过短 ({length:.1f} < {min_length})": "Drawing cancelled: length too short ({length:.1f} < {min_length})",
    "完成绘制": "Drawing finished",

    # text.py
    "创建文字: {pos}": "Created text: {pos}",

    # rect.py / ellipse.py
    "开始绘制: {pos}": "Started drawing: {pos}",
    "绘制取消：尺寸过小 ({width:.1f}x{height:.1f} < {min_size})": "Drawing cancelled: size too small ({width:.1f}x{height:.1f} < {min_size})",

    # eraser.py
    "删除了 {count} 个图元": "Deleted {count} item(s)",
    "已激活": "Activated",
    "已停用": "Deactivated",

    # number.py
    "scene.items() 失败：{exc}": "scene.items() failed: {exc}",
    "统计序号时异常：{exc}": "Exception while counting numbers: {exc}",
    "统计最大序号时异常：{exc}": "Exception while computing max number: {exc}",
    "刷新序号计数器": "Refreshing number counter",
    "同步序号创建顺序失败：{exc}": "Failed to sync number creation order: {exc}",
    "创建前场景中序号数量: {prev_count}, 将创建序号: {number}": "Number count in scene before creation: {prev_count}, will create number: {number}",
    "统计创建后序号失败：{exc}": "Failed to count numbers after creation: {exc}",
    "创建后场景中序号数量: {count_after}": "Number count in scene after creation: {count_after}",
    "scene 已失效，跳过光标更新": "Scene is no longer valid, skipping cursor update",
    "更新光标时下一个序号: {next_num}": "Next number when updating cursor: {next_num}",
    "设置光标失败：{exc}": "Failed to set cursor: {exc}",

    # cursor_manager.py
    "同步更新覆盖光标": "Syncing override cursor",
}
