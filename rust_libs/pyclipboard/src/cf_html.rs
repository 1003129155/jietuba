//! CF_HTML：Windows 剪贴板的 HTML 格式，正文前是一段记着文档、片段字节偏移量的头部。

/// 剪贴板里注册的格式名
pub const FORMAT_NAME: &str = "HTML Format";

const START_MARKER: &str = "<!--StartFragment-->";
const END_MARKER: &str = "<!--EndFragment-->";
const HEADER_TEMPLATE: &str =
    "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n";

/// 把 HTML 包成 CF_HTML，末尾带 0 结束符，可直接写进剪贴板。
/// 不是完整文档的包进 <body>；没有片段标记的以 <body> 内容为片段，连 <body> 都没有时整份都是片段。
pub fn encode(html: &str) -> Vec<u8> {
    let document = if !html.contains("<html") {
        format!(
            "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n</head>\n<body>\n{START_MARKER}{html}{END_MARKER}\n</body>\n</html>"
        )
    } else if !html.contains(START_MARKER) {
        html.replace("<body>", &format!("<body>\n{START_MARKER}"))
            .replace("</body>", &format!("{END_MARKER}\n</body>"))
    } else {
        html.to_string()
    };
    let start_html = HEADER_TEMPLATE.len();
    let fragment_start = document.find(START_MARKER).map_or(0, |at| at + START_MARKER.len());
    let fragment_end = document.find(END_MARKER).unwrap_or(document.len());
    let mut out = format!(
        "Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\nEndFragment:{:010}\r\n{}",
        start_html,
        start_html + document.len(),
        start_html + fragment_start,
        start_html + fragment_end,
        document
    )
    .into_bytes();
    out.push(0);
    out
}

/// 解出来的 CF_HTML
#[derive(Debug, PartialEq)]
pub struct Decoded {
    /// HTML 文档，不含头部
    pub document: String,
    /// 片段在 document 里的字节范围；头部没给或给得不对时为 None
    pub fragment: Option<(usize, usize)>,
}

/// 解析 CF_HTML。
///
/// 头部偏移量只当提示：写剪贴板的程序常把它算错，越界、指进头部、落在字符中间或首尾颠倒的
/// 都不用，文档改按 <html> 标签定位、片段交给调用方按注释标记找；不能因为偏移量不对而 panic。
pub fn decode(raw: &[u8]) -> Option<Decoded> {
    // lossy：个别非法字节不该让整块内容作废
    let data = String::from_utf8_lossy(raw);
    let data = data.trim_end_matches('\0');
    let header_end: usize = data.split_inclusive('\n')
        .take_while(|line| !line.starts_with('<'))
        .map(str::len)
        .sum();
    let offset = |key: &str| header_offset(data, key);
    let usable = |start: usize, end: usize| {
        start <= end && end <= data.len() && data.is_char_boundary(start) && data.is_char_boundary(end)
    };

    let (doc_start, doc_end) = match (offset("StartHTML"), offset("EndHTML")) {
        (Some(start), Some(end)) if start >= header_end && start < end && usable(start, end) => (start, end),
        _ => html_tag_range(data)?,
    };
    let fragment = match (offset("StartFragment"), offset("EndFragment")) {
        (Some(start), Some(end)) if start >= doc_start && end <= doc_end && usable(start, end) => {
            Some((start - doc_start, end - doc_start))
        }
        _ => None,
    };
    Some(Decoded { document: data[doc_start..doc_end].to_string(), fragment })
}

/// 只要 HTML 文档本身
pub fn document(raw: &[u8]) -> Option<String> {
    decode(raw).map(|decoded| decoded.document)
}

/// 头部里某一项的偏移量；头部到第一行以 < 开头的正文为止
fn header_offset(data: &str, key: &str) -> Option<usize> {
    data.lines()
        .take_while(|line| !line.starts_with('<'))
        .filter_map(|line| line.split_once(':'))
        .find(|(name, _)| name.trim() == key)
        .and_then(|(_, value)| value.trim().parse().ok())
}

/// 从 <html 到最后一个 </html>，不分大小写；没有结束标签时到末尾
fn html_tag_range(data: &str) -> Option<(usize, usize)> {
    // 只改 ASCII 字母的大小写，字节位置不变
    let lower = data.to_ascii_lowercase();
    let start = lower.find("<html")?;
    let end = lower.rfind("</html>").map_or(data.len(), |at| at + "</html>".len());
    (start < end).then_some((start, end))
}

#[cfg(test)]
mod tests {
    use super::*;

    const BODY: &str = "<html><body>中文内容测试</body></html>";

    fn header(start_html: usize, end_html: usize) -> String {
        format!("Version:0.9\r\nStartHTML:{start_html:010}\r\nEndHTML:{end_html:010}\r\n")
    }

    fn header_len() -> usize {
        header(0, 0).len()
    }

    #[test]
    fn encoded_html_decodes_back_with_its_fragment() {
        let decoded = decode(&encode("<b>粗体</b>")).unwrap();
        let (start, end) = decoded.fragment.unwrap();
        assert_eq!(&decoded.document[start..end], "<b>粗体</b>");
        assert!(decoded.document.starts_with("<!DOCTYPE html>"));
    }

    #[test]
    fn documents_without_markers_get_them_around_the_body() {
        let decoded = decode(&encode("<html><body><p>x</p></body></html>")).unwrap();
        let (start, end) = decoded.fragment.unwrap();
        assert_eq!(decoded.document[start..end].trim(), "<p>x</p>");
    }

    #[test]
    fn documents_without_a_body_are_one_whole_fragment() {
        let decoded = decode(&encode("<html><p>x</p></html>")).unwrap();
        assert_eq!(decoded.fragment, Some((0, decoded.document.len())));
    }

    #[test]
    fn offsets_pointing_at_the_body_are_used_as_is() {
        let raw = format!("{}{BODY}", header(header_len(), header_len() + BODY.len()));
        assert_eq!(document(raw.as_bytes()).as_deref(), Some(BODY));
    }

    #[test]
    fn offset_inside_a_multibyte_char_falls_back_to_the_tags() {
        // "<html><body>" 之后第一个字是「中」，+13 正好落在它的字节中间
        let raw = format!("{}{BODY}", header(header_len() + 13, header_len() + BODY.len()));
        assert_eq!(document(raw.as_bytes()).as_deref(), Some(BODY));
    }

    #[test]
    fn out_of_range_offsets_fall_back_to_the_tags() {
        let raw = format!("{}{BODY}", header(header_len(), 999_999));
        assert_eq!(document(raw.as_bytes()).as_deref(), Some(BODY));
    }

    #[test]
    fn offsets_pointing_into_the_header_are_ignored() {
        let raw = format!("{}{BODY}", header(0, 999_999));
        assert_eq!(document(raw.as_bytes()).as_deref(), Some(BODY));
    }

    #[test]
    fn reversed_offsets_fall_back_to_tag_search() {
        let raw = format!("{}{BODY}", header(200, 10));
        assert_eq!(document(raw.as_bytes()).as_deref(), Some(BODY));
    }

    #[test]
    fn tags_are_found_in_any_case_and_trailing_nuls_are_dropped() {
        let body = "<HTML><Body>中文</Body></Html>";
        let raw = format!("{}{body}\0\0", header(200, 10));
        assert_eq!(document(raw.as_bytes()).as_deref(), Some(body));
    }

    #[test]
    fn a_fragment_starting_before_the_document_is_dropped() {
        let doc = "<html><body>x</body></html>";
        let start = header_len() + "StartFragment:0000000000\r\nEndFragment:0000000000\r\n".len();
        let raw = format!(
            "Version:0.9\r\nStartHTML:{start:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\nEndFragment:{:010}\r\n{doc}",
            start + doc.len(), 5, start + 13,
        );
        let decoded = decode(raw.as_bytes()).unwrap();
        assert_eq!(decoded.document, doc);
        assert_eq!(decoded.fragment, None);
    }

    #[test]
    fn garbage_without_any_html_yields_none() {
        assert_eq!(decode(b"Version:0.9\r\nStartHTML:0000000200\r\n"), None);
        assert_eq!(decode(b"no html here"), None);
    }
}
