# 验证状态

整体无损迁移尚未完成，当前 EXE 为原型。下列测试只证明各自覆盖的范围。

- 原版选区、上次区域、缩放和工具栏归一化参考测试：64 passed。
- 原版颜色格式和保存参考测试：58 passed。
- C++ 原 Python 行为对照测试：15 passed / 0 failed；包括初始化与清理。预期值来自冻结 Python 实现。
- 原型 Windows 工作流测试：9 passed / 0 failed；包括实际桌面捕获与连续会话取消，不能视为完整原版截图窗口的对等验收。
- CTest 两个测试目标通过。
- 最终打包 EXE 在仅含 Windows/System32 的 PATH 下通过启动自检，无 Python 或 SDK bin 路径依赖。
- 原版参考文件 SHA-256 校验通过。

记录分别见 `python-reference-tests.txt`、`python-save-color-tests.txt`、`cpp-parity-tests.txt` 和 `native-tests.txt`。

`launcher.png` 是实际主窗口渲染。`startup.json` 是一次 main() 到首次事件循环的计时，不包含操作系统载入 EXE/DLL 的时间，不用于宣称相对原版的性能收益。

参考 Qt 为 6.11.0；上述 C++ 记录生成时为 Qt 6.10.3，现已改用 6.11.0。多屏混合 DPI、HDR、完整窗口交互、编辑手柄、Rust 后端、数据兼容和整应用等价性仍待验收。全部进度和剩余符号见 `migration/parity_report.json` 与 `docs/MIGRATION.md`。
