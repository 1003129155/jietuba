//! 拼接过程的错误类型。
//!
//! 存在的理由是区分「无重叠」和「真故障」：前者是算法给出的合法结论——两张图
//! 确实接不上，调用方该换一对图继续；后者是解码失败、编码失败这类故障，调用方
//! 需要知道原因。原先两者都退化成 `String`，绑定层只能一律返回 None，调用方
//! 拿不到任何可据以分支的信息。

use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum StitchError {
    /// 两图之间找不到满足条件的重叠区。
    ///
    /// 这是合法结论而非故障，Python 绑定层把它映射成 `None`，其余变体映射成异常。
    NoOverlap,
    /// 输入字节无法解码为图片。
    Decode(String),
    /// 拼接结果无法编码为 PNG。
    Encode(String),
    /// 请求了未实现的哈希算法。
    UnknownHashMethod(String),
}

impl StitchError {
    pub fn decode(context: impl fmt::Display) -> Self {
        Self::Decode(context.to_string())
    }

    pub fn encode(context: impl fmt::Display) -> Self {
        Self::Encode(context.to_string())
    }
}

impl fmt::Display for StitchError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NoOverlap => write!(f, "no overlap found between the two images"),
            Self::Decode(c) => write!(f, "failed to decode image: {c}"),
            Self::Encode(c) => write!(f, "failed to encode result: {c}"),
            Self::UnknownHashMethod(m) => write!(f, "unknown hash method: {m}"),
        }
    }
}

impl std::error::Error for StitchError {}
