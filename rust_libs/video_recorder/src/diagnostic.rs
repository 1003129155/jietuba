use std::io::Write;

/// 少量原生诊断不参与会话控制，也不依赖 Python 或通信协议。
pub fn diagnostic(code: &str, message: &str) {
    let _ = writeln!(std::io::stderr().lock(), "{code}: {message}");
}
