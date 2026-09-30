//! Windows 全局输入：一个原生线程持有低层鼠标、键盘钩子和前台窗口事件。
//!
//! 钩子回调只运行纯状态机（engine），在回调里当场决定吞不吞事件，从不进入 Python；
//! 要交给界面的少量事件放进队列，由 Python 侧的消费线程取走。

pub mod engine;
pub mod hooks;

#[cfg(feature = "python")]
mod python;
