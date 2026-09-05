"""main/gif/ 目录下 log_* 调用的中→英翻译表。"""

TRANSLATIONS: dict[str, str] = {
    # record_window.py
    "GifRecordWindow 初始化, 区域={capture_rect}": "GifRecordWindow initialized, region={capture_rect}",
    "GifRecordWindow 初始化完成": "GifRecordWindow initialization complete",
    "FPS 切换: {fps}": "FPS changed: {fps}",
    "开始录制, fps={fps}, 区域={rect}": "Recording started, fps={fps}, region={rect}",
    "停止录制, 共 {frame_count} 帧, store=有": "Recording stopped, {frame_count} frames total, store=present",
    "停止录制, 共 {frame_count} 帧, store=无": "Recording stopped, {frame_count} frames total, store=absent",
    "FrameStore 为空": "FrameStore is empty",
    "录制帧为空，不进入回放": "Recorded frames are empty, skipping playback",
    "录制暂停": "Recording paused",
    "录制恢复": "Recording resumed",
    "绘制工具: {tool_id}": "Drawing tool: {tool_id}",
    "退出绘制模式": "Exiting drawing mode",
    "重新录制": "Re-recording",
    "关闭 GIF 窗口": "Closing GIF window",
    "清除 app._gif_window": "Cleared app._gif_window",

    # record_toolbar.py
    "加载 GIF 帧率设置": "Loading GIF frame rate settings",
    "加载绘制工具设置": "Loading drawing tool settings",
    "保存箭头样式": "Saving arrow style",
    "保存线条样式": "Saving line style",
    "保存 GIF 帧率": "Saving GIF frame rate",

    # playback_engine.py
    "PlaybackEngine cleanup 完成": "PlaybackEngine cleanup complete",
    "get_frame_image 失败 idx={index}: {e}": "get_frame_image failed idx={index}: {e}",

    # playback_controller.py
    "切换到回放模式, {frame_count} 帧": "Switching to playback mode, {frame_count} frames",
    "预览层创建, geometry={geometry}": "Preview layer created, geometry={geometry}",
    "回放暂停": "Playback paused",
    "回放开始": "Playback started",
    "回放速度: {speed}": "Playback speed: {speed}",
    "跳转帧: {index}": "Seeking to frame: {index}",
    "重新录制（回放控制器）": "Re-recording (playback controller)",
    "帧渲染失败 index={index}: {e}": "Frame render failed index={index}: {e}",
    "回放结束": "Playback finished",
    "预览层销毁": "Preview layer destroyed",
    "保存取消": "Save cancelled",
    "保存 GIF: {path}": "Saving GIF: {path}",
    "复制 GIF → {out}": "Copying GIF → {out}",
    "开始导出 GIF: frames={frame_count}, copy={copy_to_clipboard}, store=有":
        "Starting GIF export: frames={frame_count}, copy={copy_to_clipboard}, store=present",
    "开始导出 GIF: frames={frame_count}, copy={copy_to_clipboard}, store=无":
        "Starting GIF export: frames={frame_count}, copy={copy_to_clipboard}, store=absent",
    "帧列表为空，中止导出": "Frame list is empty, aborting export",
    "导出裁剪范围: [{trim_start}, {trim_end}]，共 {frame_count} 帧":
        "Export trim range: [{trim_start}, {trim_end}], {frame_count} frames total",
    "鼠标导出: {n_active}/{frame_count} 帧有光标": "Cursor export: {n_active}/{frame_count} frames have a cursor",
    "GIF 导出取消或失败": "GIF export cancelled or failed",
    "GIF 导出完成: {result}": "GIF export complete: {result}",
    "已复制到剪贴板 [{mime_type}], 大小={size} bytes": "Copied to clipboard [{mime_type}], size={size} bytes",
    "复制到剪贴板失败: {e}": "Failed to copy to clipboard: {e}",
    "获取截图保存路径": "Getting screenshot save path",

    # frame_recorder.py
    "gifrecorder 不可用，无法录制": "gifrecorder unavailable, cannot record",
    "FrameStore 创建失败: {e}": "FrameStore creation failed: {e}",
    "RecordSession 启动失败: {e}": "RecordSession failed to start: {e}",
    "录制开始: {w}x{h} @ {fps}fps (Rust Win32 BitBlt)": "Recording started: {w}x{h} @ {fps}fps (Rust Win32 BitBlt)",
    "RecordSession.stop 异常: {e}": "RecordSession.stop exception: {e}",
    "StopWorker 异常: {e}": "StopWorker exception: {e}",
    "停止 Rust 截屏会话": "Stopping Rust screen capture session",
    "pynput 滚轮监听启动失败: {e}": "pynput scroll listener failed to start: {e}",
    "停止滚轮监听": "Stopping scroll listener",
    "采集鼠标快照": "Capturing cursor snapshot",

    # composer.py
    "GIF 合成失败: {e}": "GIF composition failed: {e}",
    "GIF 取消后删除临时文件": "Deleting temp file after GIF export cancelled",
    "GIF 读取后删除临时文件": "Deleting temp file after reading GIF",
    "使用 gifrecorder export_gif: {gif_width}x{gif_height}": "Using gifrecorder export_gif: {gif_width}x{gif_height}",
    "export_gif 失败: {e}": "export_gif failed: {e}",
    "GIF 导出完成: {out_path} ({size_kb:.1f} KB)": "GIF export complete: {out_path} ({size_kb:.1f} KB)",
    "居中进度对话框": "Centering progress dialog",

    # drawing_view.py
    "GIF 绘制层设置透明": "Setting GIF drawing layer transparency",
    "绘制层穿透=开": "Drawing layer click-through=on",
    "绘制层穿透=关": "Drawing layer click-through=off",

    # drawing_toolbar.py
    "加载绘制工具样式": "Loading drawing tool style",

    # cursor_overlay.py
    "设置光标覆盖层透明": "Setting cursor overlay transparency",
}
