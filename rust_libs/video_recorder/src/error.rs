use std::fmt;

pub type Result<T> = std::result::Result<T, Failure>;

#[derive(Debug)]
pub struct Failure {
    pub code: &'static str,
    pub message: String,
}

impl Failure {
    pub fn new(code: &'static str, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }
}

impl fmt::Display for Failure {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message)
    }
}

impl std::error::Error for Failure {}

impl From<windows::core::Error> for Failure {
    fn from(error: windows::core::Error) -> Self {
        Self::new("recording", format!("{} ({:#010x})", error.message(), error.code().0 as u32))
    }
}
