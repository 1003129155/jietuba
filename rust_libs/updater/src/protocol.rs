use crate::{fail, Result};
use serde::{Deserialize, Serialize};
use std::io::{BufRead, Write};
pub const MAX_LINE: usize = 65536;
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Control {
    pub protocol: u32,
    pub transaction: String,
    pub command: String,
}
pub fn read_control(reader: &mut impl BufRead, transaction: &str) -> Result<Control> {
    let mut bytes = Vec::new();
    loop {
        let available = reader.fill_buf()?;
        if available.is_empty() {
            return fail("cancelled", "控制通道已关闭");
        }
        let n = available
            .iter()
            .position(|c| *c == b'\n')
            .map(|i| i + 1)
            .unwrap_or(available.len());
        if bytes.len() + n > MAX_LINE {
            return fail("protocol", "控制消息长度超限");
        }
        let end = available[n - 1] == b'\n';
        bytes.extend_from_slice(&available[..n]);
        reader.consume(n);
        if end {
            break;
        }
    }
    let control: Control = serde_json::from_slice(&bytes)?;
    if control.protocol != 1
        || control.transaction != transaction
        || !["go", "cancel"].contains(&control.command.as_str())
    {
        return fail("protocol", "无效控制消息");
    }
    Ok(control)
}
#[derive(Serialize)]
pub struct Event<'a> {
    pub protocol: u32,
    pub event: &'a str,
    pub transaction: &'a str,
    pub data: serde_json::Value,
}
pub fn emit(event: &str, transaction: &str, data: serde_json::Value) -> Result<()> {
    let bytes = serde_json::to_vec(&Event {
        protocol: 1,
        event,
        transaction,
        data,
    })?;
    if bytes.len() > MAX_LINE {
        return fail("protocol", "事件消息长度超限");
    }
    let mut stdout = std::io::stdout().lock();
    stdout.write_all(&bytes)?;
    stdout.write_all(b"\n")?;
    stdout.flush()?;
    Ok(())
}
