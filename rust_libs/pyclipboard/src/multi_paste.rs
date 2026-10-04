//! 多选粘贴：几条记录合成一份剪贴板内容。
//!
//! 文本按分隔符拼接；HTML、RTF 各合成一份完整文档，本来没有这种格式的条目按纯文本
//! 转进去，贴到只认富文本的地方也一条不少；文件取并集；全是图片时拼成一张。

use std::collections::{HashMap, HashSet};
use std::io::Cursor;

use image::{Rgba, RgbaImage};

// ── 文本、文件 ────────────────────────────────────────────

/// 剪贴板文本按 Windows 惯例用 CRLF
pub fn crlf(text: &str) -> String {
    text.replace("\r\n", "\n").replace('\n', "\r\n")
}

pub fn join_text(pieces: &[String], separator: &str) -> String {
    pieces.join(&crlf(separator))
}

/// 同一个路径只留一次；Windows 路径不分大小写
pub fn union_files(lists: &[Vec<String>]) -> Vec<String> {
    let mut seen = HashSet::new();
    lists.iter()
        .flatten()
        .filter(|path| seen.insert(path.to_lowercase()))
        .cloned()
        .collect()
}

// ── HTML ──────────────────────────────────────────────────

/// 一条记录的 HTML，拆成合并要用的几部分
#[derive(Debug, Default, Clone, PartialEq)]
pub struct HtmlPart {
    /// <html> 上的命名空间声明；Office 片段里的 x:、o: 属性靠它们解释
    namespaces: Vec<String>,
    /// <meta name=ProgId> 的值，Office 据此按自己的格式解析
    prog_id: Option<String>,
    /// <head> 里各 <style> 的内容
    css: String,
    /// 片段外层的开始、结束标签：表格单元格的片段只有行，<table> 在片段外面
    open: String,
    close: String,
    fragment: String,
}

impl HtmlPart {
    pub fn from_text(text: &str) -> Self {
        Self { fragment: text_to_html(text), ..Self::default() }
    }

    /// CF_HTML 原始数据；头部偏移量不可用时按注释标记和标签找
    pub fn from_cf_html(raw: &[u8]) -> Option<Self> {
        let data = String::from_utf8_lossy(raw);
        let data = data.trim_end_matches('\0');
        let offset = |key: &str| header_offset(data, key);
        let range = |start: Option<usize>, end: Option<usize>| match (start, end) {
            (Some(start), Some(end))
                if start <= end && end <= data.len()
                    && data.is_char_boundary(start) && data.is_char_boundary(end) => Some((start, end)),
            _ => None,
        };
        let (doc_start, doc_end) = range(offset("StartHTML"), offset("EndHTML"))
            .or_else(|| find_ci(data, "<html", 0).map(|start| (start, data.len())))?;
        let fragment = range(offset("StartFragment"), offset("EndFragment"))
            .filter(|(start, end)| *start >= doc_start && *end <= doc_end)
            .map(|(start, end)| (start - doc_start, end - doc_start));
        Some(Self::from_document(&data[doc_start..doc_end], fragment))
    }

    /// 完整的 HTML 文档；fragment 为片段在文档里的范围，缺省时按注释标记或 <body> 找
    pub fn from_document(doc: &str, fragment: Option<(usize, usize)>) -> Self {
        let (start, end) = fragment
            .or_else(|| fragment_markers(doc))
            .or_else(|| body_inner(doc))
            .unwrap_or((0, doc.len()));
        let body = find_ci(doc, "<body", 0).filter(|body| *body < start);
        let head = &doc[..body.unwrap_or(start)];
        let (open, close) = match body {
            Some(body) => {
                let inner = tag_end(doc.as_bytes(), body + 1).min(start);
                let body_end = find_ci(doc, "</body", end).unwrap_or(doc.len());
                (tags_only(&doc[inner..start]), tags_only(&doc[end..body_end]))
            }
            None => (String::new(), String::new()),
        };
        Self {
            namespaces: html_namespaces(head),
            prog_id: meta_prog_id(head),
            css: styles(head),
            open,
            close,
            fragment: strip_fragment_markers(&doc[start..end]),
        }
    }
}

/// 合成一份完整的 HTML 文档（不含 CF_HTML 头部），片段用注释标记括起来。
///
/// 各条的样式都带上。同名的 class 在两条里定义不同时（例如两次表格复制各自从
/// xl65 编起），后面那条改名，免得互相覆盖。
pub fn merge_html(parts: &[HtmlPart], separator: &str) -> String {
    let mut known: HashMap<String, String> = HashMap::new();
    let mut css = String::new();
    let mut body = String::new();
    for (index, part) in parts.iter().enumerate() {
        let rules = parse_css(&part.css);
        let mut definitions: Vec<(String, String)> = class_definitions(&rules).into_iter().collect();
        definitions.sort();
        let mut renames = HashMap::new();
        for (class, definition) in definitions {
            match known.get(&class) {
                Some(existing) if *existing != definition => {
                    let mut n = index + 1;
                    let renamed = loop {
                        let candidate = format!("{class}-{n}");
                        if !known.contains_key(&candidate) {
                            break candidate;
                        }
                        n += 1;
                    };
                    known.insert(renamed.clone(), definition);
                    renames.insert(class, renamed);
                }
                Some(_) => {}
                None => {
                    known.insert(class, definition);
                }
            }
        }
        css.push_str(&render_css(&rules, &renames));
        if index > 0 {
            body.push_str(&text_to_html(separator));
        }
        let content = format!("{}{}{}", part.open, part.fragment, part.close);
        body.push_str(&if renames.is_empty() { content } else { rename_classes(&content, &renames) });
    }

    let mut namespaces: Vec<&str> = Vec::new();
    for namespace in parts.iter().flat_map(|part| &part.namespaces) {
        let name = namespace.split('=').next().unwrap_or_default();
        if !namespaces.iter().any(|n| n.split('=').next().unwrap_or_default().eq_ignore_ascii_case(name)) {
            namespaces.push(namespace);
        }
    }
    // 只有来源一致时才保留：混进别的程序的内容后再按 Office 格式解析反而出错
    let mut prog_ids = parts.iter().filter_map(|part| part.prog_id.as_deref());
    let prog_id = prog_ids.next().filter(|first| prog_ids.all(|other| other == *first));

    let mut html = String::from("<html");
    for namespace in namespaces {
        html.push(' ');
        html.push_str(namespace);
    }
    html.push_str(">\r\n<head>\r\n<meta http-equiv=Content-Type content=\"text/html; charset=utf-8\">\r\n");
    if let Some(prog_id) = prog_id {
        html.push_str(&format!("<meta name=ProgId content={prog_id}>\r\n"));
    }
    if !css.is_empty() {
        html.push_str("<style>\r\n<!--\r\n");
        html.push_str(&css);
        html.push_str("-->\r\n</style>\r\n");
    }
    html.push_str("</head>\r\n<body>\r\n<!--StartFragment-->");
    html.push_str(&body);
    html.push_str("<!--EndFragment-->\r\n</body>\r\n</html>");
    html
}

/// 纯文本转成 HTML：转义，换行变 <br>，连续空格和制表符不让浏览器合并掉
pub fn text_to_html(text: &str) -> String {
    let mut html = String::with_capacity(text.len());
    for (index, line) in text.replace("\r\n", "\n").split('\n').enumerate() {
        if index > 0 {
            html.push_str("<br>");
        }
        // 行首的空格也要保留，所以一开始就当作前面是空格
        let mut after_space = true;
        for ch in line.chars() {
            match ch {
                '&' => html.push_str("&amp;"),
                '<' => html.push_str("&lt;"),
                '>' => html.push_str("&gt;"),
                '"' => html.push_str("&quot;"),
                ' ' if after_space => html.push_str("&nbsp;"),
                ' ' => html.push(' '),
                '\t' => html.push_str("&nbsp;&nbsp;&nbsp;&nbsp;"),
                ch => html.push(ch),
            }
            after_space = ch == ' ' && !html.ends_with("&nbsp;");
        }
    }
    html
}

/// CF_HTML 头部的一个偏移量
fn header_offset(data: &str, key: &str) -> Option<usize> {
    data.lines()
        .take_while(|line| !line.starts_with('<'))
        .filter_map(|line| line.split_once(':'))
        .find(|(name, _)| name.trim() == key)
        .and_then(|(_, value)| value.trim().parse().ok())
}

/// 不区分 ASCII 大小写查找；needle 以 ASCII 开头，找到的位置必是字符边界
fn find_ci(haystack: &str, needle: &str, from: usize) -> Option<usize> {
    let (hay, pat) = (haystack.as_bytes(), needle.as_bytes());
    if pat.is_empty() || from + pat.len() > hay.len() {
        return None;
    }
    (from..=hay.len() - pat.len()).find(|&i| hay[i..i + pat.len()].eq_ignore_ascii_case(pat))
}

fn fragment_markers(doc: &str) -> Option<(usize, usize)> {
    let marker = find_ci(doc, "<!--StartFragment", 0)?;
    let start = doc[marker..].find("-->")? + marker + 3;
    let end = find_ci(doc, "<!--EndFragment", start)?;
    Some((start, end))
}

fn body_inner(doc: &str) -> Option<(usize, usize)> {
    let body = find_ci(doc, "<body", 0)?;
    let start = tag_end(doc.as_bytes(), body + 1);
    let end = find_ci(doc, "</body", start).unwrap_or(doc.len());
    Some((start, end))
}

/// 片段里残留的起止标记会让合并后的片段在第一个结束标记处就被截断
fn strip_fragment_markers(fragment: &str) -> String {
    let mut out = String::with_capacity(fragment.len());
    let mut copied = 0;
    let mut pos = 0;
    while let Some((start, end)) = next_tag(fragment, pos) {
        pos = end;
        let tag = &fragment[start..end];
        if find_ci(tag, "<!--StartFragment", 0) == Some(0) || find_ci(tag, "<!--EndFragment", 0) == Some(0) {
            out.push_str(&fragment[copied..start]);
            copied = end;
        }
    }
    out.push_str(&fragment[copied..]);
    out
}

/// 下一个标签或注释的 (起点, 终点)；正文里落单的 '<' 不算
fn next_tag(html: &str, from: usize) -> Option<(usize, usize)> {
    let bytes = html.as_bytes();
    let mut pos = from;
    while let Some(offset) = html.get(pos..)?.find('<') {
        let start = pos + offset;
        if html[start..].starts_with("<!--") {
            let end = html[start + 4..].find("-->").map_or(html.len(), |e| start + 4 + e + 3);
            return Some((start, end));
        }
        match bytes.get(start + 1) {
            Some(c) if c.is_ascii_alphabetic() || matches!(c, b'/' | b'!' | b'?') => {
                return Some((start, tag_end(bytes, start + 1)));
            }
            _ => pos = start + 1,
        }
    }
    None
}

/// 标签结束的位置（'>' 之后）；属性值引号里的 '>' 不算
fn tag_end(bytes: &[u8], mut i: usize) -> usize {
    let mut after_equals = false;
    while i < bytes.len() {
        match bytes[i] {
            b'>' => return i + 1,
            b'=' => after_equals = true,
            quote @ (b'"' | b'\'') if after_equals => {
                i += 1;
                while i < bytes.len() && bytes[i] != quote {
                    i += 1;
                }
                after_equals = false;
            }
            c if c.is_ascii_whitespace() => {}
            _ => after_equals = false,
        }
        i += 1;
    }
    bytes.len()
}

/// 只留下标签，去掉注释和标签之间的文字
fn tags_only(html: &str) -> String {
    let mut out = String::new();
    let mut pos = 0;
    while let Some((start, end)) = next_tag(html, pos) {
        pos = end;
        if !html[start..].starts_with("<!") {
            out.push_str(&html[start..end]);
        }
    }
    out
}

fn tag_name(tag: &str) -> &str {
    let name = tag.trim_start_matches('<');
    let end = name.find(|c: char| c.is_ascii_whitespace() || c == '>' || c == '/').unwrap_or(name.len());
    &name[..end]
}

/// 标签的属性：(名字, 值在标签里的字节范围)
fn attributes(tag: &str) -> Vec<(&str, Option<(usize, usize)>)> {
    let bytes = tag.as_bytes();
    let mut i = 1;
    while i < bytes.len() && !bytes[i].is_ascii_whitespace() && !matches!(bytes[i], b'>' | b'/') {
        i += 1;
    }
    let mut attrs = Vec::new();
    loop {
        while i < bytes.len() && (bytes[i].is_ascii_whitespace() || bytes[i] == b'/') {
            i += 1;
        }
        if i >= bytes.len() || bytes[i] == b'>' {
            return attrs;
        }
        let name_start = i;
        while i < bytes.len() && !bytes[i].is_ascii_whitespace() && !matches!(bytes[i], b'=' | b'>' | b'/') {
            i += 1;
        }
        let name = &tag[name_start..i];
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= bytes.len() || bytes[i] != b'=' {
            attrs.push((name, None));
            continue;
        }
        i += 1;
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if let Some(quote @ (b'"' | b'\'')) = bytes.get(i).copied() {
            let start = i + 1;
            i = start;
            while i < bytes.len() && bytes[i] != quote {
                i += 1;
            }
            attrs.push((name, Some((start, i))));
            i += 1;
        } else {
            let start = i;
            while i < bytes.len() && !bytes[i].is_ascii_whitespace() && bytes[i] != b'>' {
                i += 1;
            }
            attrs.push((name, Some((start, i))));
        }
    }
}

fn attribute<'a>(tag: &'a str, name: &str) -> Option<&'a str> {
    attributes(tag)
        .into_iter()
        .find(|(attr, _)| attr.eq_ignore_ascii_case(name))
        .and_then(|(_, value)| value)
        .map(|(start, end)| &tag[start..end.min(tag.len())])
}

fn html_namespaces(head: &str) -> Vec<String> {
    let Some(start) = find_ci(head, "<html", 0) else {
        return Vec::new();
    };
    let tag = &head[start..tag_end(head.as_bytes(), start + 1)];
    attributes(tag)
        .into_iter()
        .filter(|(name, _)| name.len() >= 5 && name[..5].eq_ignore_ascii_case("xmlns"))
        .filter_map(|(name, value)| {
            let (start, end) = value?;
            let value = &tag[start..end.min(tag.len())];
            Some(if value.contains('"') { format!("{name}='{value}'") } else { format!("{name}=\"{value}\"") })
        })
        .collect()
}

fn meta_prog_id(head: &str) -> Option<String> {
    let mut pos = 0;
    while let Some((start, end)) = next_tag(head, pos) {
        pos = end;
        let tag = &head[start..end];
        if tag_name(tag).eq_ignore_ascii_case("meta")
            && attribute(tag, "name").is_some_and(|name| name.eq_ignore_ascii_case("ProgId"))
        {
            return attribute(tag, "content").map(str::to_string);
        }
    }
    None
}

fn styles(head: &str) -> String {
    let mut css = String::new();
    let mut pos = 0;
    while let Some(open) = find_ci(head, "<style", pos) {
        let start = tag_end(head.as_bytes(), open + 1);
        let end = find_ci(head, "</style", start).unwrap_or(head.len());
        css.push_str(&head[start..end]);
        css.push_str("\r\n");
        pos = end;
    }
    css
}

/// 把标签 class 属性里的类名按 renames 改掉
fn rename_classes(html: &str, renames: &HashMap<String, String>) -> String {
    let mut out = String::with_capacity(html.len());
    let mut copied = 0;
    let mut pos = 0;
    while let Some((start, end)) = next_tag(html, pos) {
        pos = end;
        let tag = &html[start..end];
        if tag.starts_with("<!") || tag.starts_with("</") {
            continue;
        }
        for (name, value) in attributes(tag) {
            let (Some((value_start, value_end)), true) = (value, name.eq_ignore_ascii_case("class")) else {
                continue;
            };
            let value = &tag[value_start..value_end.min(tag.len())];
            if !value.split_ascii_whitespace().any(|class| renames.contains_key(class)) {
                continue;
            }
            let renamed: Vec<&str> = value
                .split_ascii_whitespace()
                .map(|class| renames.get(class).map_or(class, String::as_str))
                .collect();
            out.push_str(&html[copied..start + value_start]);
            out.push_str(&renamed.join(" "));
            copied = start + value_end.min(tag.len());
        }
    }
    out.push_str(&html[copied..]);
    out
}

#[derive(Debug)]
enum CssItem {
    Rule { selector: String, body: String },
    /// @ 规则等原样保留的部分
    Raw(String),
}

fn parse_css(css: &str) -> Vec<CssItem> {
    // <style> 里常见的 <!-- --> 只是给老浏览器看的，对规则没有意义
    let css = css.replace("<!--", " ").replace("-->", " ");
    let bytes = css.as_bytes();
    let mut items = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i].is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if css[i..].starts_with("/*") {
            i = css[i + 2..].find("*/").map_or(bytes.len(), |end| i + 2 + end + 2);
            continue;
        }
        let start = i;
        let mut j = i;
        while j < bytes.len() && !matches!(bytes[j], b'{' | b';') {
            j = if matches!(bytes[j], b'"' | b'\'') { skip_string(bytes, j) } else { j + 1 };
        }
        match bytes.get(j) {
            Some(b';') => {
                items.push(CssItem::Raw(css[start..=j].to_string()));
                i = j + 1;
            }
            Some(_) => {
                let (body_end, end) = block_end(bytes, j);
                let prelude = css[start..j].trim();
                if prelude.starts_with('@') {
                    items.push(CssItem::Raw(css[start..end].to_string()));
                } else {
                    items.push(CssItem::Rule { selector: prelude.to_string(), body: css[j + 1..body_end].to_string() });
                }
                i = end;
            }
            None => {
                items.push(CssItem::Raw(css[start..].to_string()));
                break;
            }
        }
    }
    items
}

/// 引号字符串之后的位置
fn skip_string(bytes: &[u8], start: usize) -> usize {
    let quote = bytes[start];
    let mut i = start + 1;
    while i < bytes.len() && bytes[i] != quote {
        i += if bytes[i] == b'\\' { 2 } else { 1 };
    }
    (i + 1).min(bytes.len())
}

/// open 处 '{' 对应的 '}'：返回 (块内容的结尾, 整块之后的位置)
fn block_end(bytes: &[u8], open: usize) -> (usize, usize) {
    let mut depth = 0;
    let mut i = open;
    while i < bytes.len() {
        match bytes[i] {
            b'{' => depth += 1,
            b'}' => {
                depth -= 1;
                if depth == 0 {
                    return (i, i + 1);
                }
            }
            b'"' | b'\'' => {
                i = skip_string(bytes, i);
                continue;
            }
            _ => {}
        }
        i += 1;
    }
    (bytes.len(), bytes.len())
}

/// 选择器里各个类名的字节范围（不含前面的点）
fn selector_classes(selector: &str) -> Vec<(usize, usize)> {
    let bytes = selector.as_bytes();
    let mut spans = Vec::new();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'"' | b'\'' => i = skip_string(bytes, i),
            b'[' => i = selector[i..].find(']').map_or(bytes.len(), |end| i + end + 1),
            b'.' => {
                let start = i + 1;
                let mut end = start;
                while end < bytes.len() && (bytes[end].is_ascii_alphanumeric() || matches!(bytes[end], b'_' | b'-') || bytes[end] >= 0x80) {
                    end += 1;
                }
                if end > start && !bytes[start].is_ascii_digit() {
                    spans.push((start, end));
                }
                i = end.max(start);
            }
            _ => i += 1,
        }
    }
    spans
}

fn normalize_space(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

/// 每个类名对应的定义：所有提到它的规则连起来，用来判断两条里的同名类是否一样
fn class_definitions(items: &[CssItem]) -> HashMap<String, String> {
    let mut definitions: HashMap<String, String> = HashMap::new();
    for item in items {
        let CssItem::Rule { selector, body } = item else { continue };
        let rule = format!("{}{{{}}}", normalize_space(selector), normalize_space(body));
        let mut classes: Vec<&str> = selector_classes(selector).into_iter().map(|(s, e)| &selector[s..e]).collect();
        classes.dedup();
        for class in classes {
            definitions.entry(class.to_string()).or_default().push_str(&rule);
        }
    }
    definitions
}

fn render_css(items: &[CssItem], renames: &HashMap<String, String>) -> String {
    let mut css = String::new();
    for item in items {
        match item {
            CssItem::Rule { selector, body } => {
                let mut renamed = String::with_capacity(selector.len());
                let mut copied = 0;
                for (start, end) in selector_classes(selector) {
                    if let Some(new_name) = renames.get(&selector[start..end]) {
                        renamed.push_str(&selector[copied..start]);
                        renamed.push_str(new_name);
                        copied = end;
                    }
                }
                renamed.push_str(&selector[copied..]);
                css.push_str(&format!("{renamed}\r\n\t{{{body}}}\r\n"));
            }
            CssItem::Raw(raw) => {
                css.push_str(raw);
                css.push_str("\r\n");
            }
        }
    }
    css
}

// ── RTF ───────────────────────────────────────────────────

#[derive(Clone, Debug, PartialEq)]
enum Rtf {
    Group(Vec<Rtf>),
    Word(String, Option<i32>),
    /// 控制符号，原样保存，如 \'e9、\{、\*
    Symbol(Vec<u8>),
    /// \binN 后面的二进制数据
    Bin(Vec<u8>),
    Text(Vec<u8>),
}

/// 嵌套超过这么多层的内容不当 RTF 解析，免得递归爆栈
const MAX_RTF_DEPTH: usize = 512;

struct RtfParser<'a> {
    data: &'a [u8],
    pos: usize,
}

impl RtfParser<'_> {
    /// pos 指着 '{'，读到对应的 '}'；缺了结尾的 '}' 时按读到的算
    fn group(&mut self, depth: usize) -> Option<Vec<Rtf>> {
        if depth > MAX_RTF_DEPTH {
            return None;
        }
        self.pos += 1;
        let mut nodes = Vec::new();
        while let Some(&byte) = self.data.get(self.pos) {
            match byte {
                b'{' => nodes.push(Rtf::Group(self.group(depth + 1)?)),
                b'}' => {
                    self.pos += 1;
                    return Some(nodes);
                }
                b'\\' => nodes.push(self.control()),
                _ => {
                    let start = self.pos;
                    while self.data.get(self.pos).is_some_and(|b| !matches!(b, b'{' | b'}' | b'\\')) {
                        self.pos += 1;
                    }
                    nodes.push(Rtf::Text(self.data[start..self.pos].to_vec()));
                }
            }
        }
        Some(nodes)
    }

    fn control(&mut self) -> Rtf {
        let data = self.data;
        let start = self.pos;
        self.pos += 1;
        let Some(&first) = data.get(self.pos) else {
            return Rtf::Symbol(b"\\".to_vec());
        };
        if !first.is_ascii_alphabetic() {
            let end = (start + if first == b'\'' { 4 } else { 2 }).min(data.len());
            self.pos = end;
            return Rtf::Symbol(data[start..end].to_vec());
        }
        let name_start = self.pos;
        while data.get(self.pos).is_some_and(u8::is_ascii_alphabetic) {
            self.pos += 1;
        }
        let name = String::from_utf8_lossy(&data[name_start..self.pos]).into_owned();
        let number_start = self.pos;
        if data.get(self.pos) == Some(&b'-') {
            self.pos += 1;
        }
        let digits_start = self.pos;
        while data.get(self.pos).is_some_and(u8::is_ascii_digit) {
            self.pos += 1;
        }
        let param = if self.pos > digits_start {
            std::str::from_utf8(&data[number_start..self.pos]).ok()
                .and_then(|n| n.parse::<i64>().ok())
                .map(|n| n.clamp(i32::MIN as i64, i32::MAX as i64) as i32)
        } else {
            // 减号后面没有数字：减号是正文
            self.pos = number_start;
            None
        };
        if data.get(self.pos) == Some(&b' ') {
            self.pos += 1;
        }
        if name == "bin" {
            let end = (self.pos + param.unwrap_or(0).max(0) as usize).min(data.len());
            let raw = data[self.pos..end].to_vec();
            self.pos = end;
            return Rtf::Bin(raw);
        }
        Rtf::Word(name, param)
    }
}

/// 控制字后一律补一个空格作分隔：它只是分隔符，不算正文，省得判断后面跟的是什么
fn write_rtf(nodes: &[Rtf], out: &mut Vec<u8>) {
    for node in nodes {
        match node {
            Rtf::Group(children) => {
                out.push(b'{');
                write_rtf(children, out);
                out.push(b'}');
            }
            Rtf::Word(name, param) => {
                out.push(b'\\');
                out.extend_from_slice(name.as_bytes());
                if let Some(param) = param {
                    out.extend_from_slice(param.to_string().as_bytes());
                }
                out.push(b' ');
            }
            Rtf::Symbol(raw) | Rtf::Text(raw) => out.extend_from_slice(raw),
            Rtf::Bin(raw) => {
                out.extend_from_slice(format!("\\bin{} ", raw.len()).as_bytes());
                out.extend_from_slice(raw);
            }
        }
    }
}

fn rtf_bytes(nodes: &[Rtf]) -> Vec<u8> {
    let mut out = Vec::new();
    write_rtf(nodes, &mut out);
    out
}

/// 组的目标名：开头的控制字，或 \* 后面的控制字
fn destination(children: &[Rtf]) -> Option<&str> {
    let mut nodes = children.iter().filter(|node| !matches!(node, Rtf::Text(t) if t.iter().all(u8::is_ascii_whitespace)));
    match nodes.next()? {
        Rtf::Word(name, _) => Some(name),
        Rtf::Symbol(symbol) if symbol == b"\\*" => match nodes.next()? {
            Rtf::Word(name, _) => Some(name),
            _ => None,
        },
        _ => None,
    }
}

/// 文档头里除字体表、颜色表外的部分；只保留第一份文档的，其余文档的正文里
/// 对样式、列表的引用随之去掉
const HEADER_DESTINATIONS: &[&str] = &[
    "stylesheet", "listtable", "listoverridetable", "rsidtbl", "generator", "info", "xmlnstbl",
    "mmathPr", "themedata", "colorschememapping", "latentstyles", "datastore", "defchp", "defpap",
    "filetbl", "revtbl", "pgdsctbl", "userprops", "wgrffmtfilter", "fchars", "lchars", "pnseclvl",
];
const FONT_REFS: &[&str] = &["f", "af", "pnf"];
const COLOR_REFS: &[&str] = &[
    "cf", "cb", "highlight", "chcbpat", "chcfpat", "cbpat", "cfpat", "clcbpat", "clcfpat",
    "clcbpatraw", "clcfpatraw", "brdrcf", "trcbpat", "trcfpat", "ulc", "pncf",
];
const STYLE_REFS: &[&str] = &["s", "cs", "ds", "ts", "ls", "ilvl"];

struct RtfDoc {
    /// \rtf1 之后、第一个组之前的文档设置
    prolog: Vec<Rtf>,
    fonts: Vec<(i32, Vec<Rtf>)>,
    colors: Vec<Vec<Rtf>>,
    header: Vec<Rtf>,
    body: Vec<Rtf>,
    uc: Option<i32>,
    deff: Option<i32>,
}

fn parse_rtf_doc(data: &[u8]) -> Option<RtfDoc> {
    let start = data.iter().position(|b| *b == b'{')?;
    let nodes = RtfParser { data, pos: start }.group(0)?;
    let mut nodes = nodes.into_iter();
    if !matches!(nodes.next()?, Rtf::Word(name, _) if name == "rtf") {
        return None;
    }
    let mut doc = RtfDoc {
        prolog: Vec::new(),
        fonts: Vec::new(),
        colors: Vec::new(),
        header: Vec::new(),
        body: Vec::new(),
        uc: None,
        deff: None,
    };
    let mut in_prolog = true;
    for node in nodes {
        match node {
            Rtf::Group(children) => {
                in_prolog = false;
                match destination(&children).map(str::to_owned).as_deref() {
                    Some("fonttbl") => doc.fonts = parse_fonttbl(&children),
                    Some("colortbl") => doc.colors = parse_colortbl(&children),
                    Some(name) if HEADER_DESTINATIONS.contains(&name) => doc.header.push(Rtf::Group(children)),
                    _ => doc.body.push(Rtf::Group(children)),
                }
            }
            Rtf::Word(name, param) if in_prolog => {
                match (name.as_str(), param) {
                    ("uc", Some(param)) => doc.uc = Some(param),
                    ("deff", Some(param)) => doc.deff = Some(param),
                    _ => {}
                }
                doc.prolog.push(Rtf::Word(name, param));
            }
            Rtf::Text(text) if in_prolog && text.iter().all(u8::is_ascii_whitespace) => doc.prolog.push(Rtf::Text(text)),
            other => {
                in_prolog = false;
                doc.body.push(other);
            }
        }
    }
    Some(doc)
}

/// 字体表：每项 (编号, 去掉 \fN 后的定义)；每项各成一组和连着写两种都有
fn parse_fonttbl(children: &[Rtf]) -> Vec<(i32, Vec<Rtf>)> {
    let is_font_number = |node: &Rtf| matches!(node, Rtf::Word(name, Some(_)) if name == "f");
    let mut fonts = Vec::new();
    let mut flat: Option<(i32, Vec<Rtf>)> = None;
    for node in children.iter().skip_while(|node| !matches!(node, Rtf::Word(name, _) if name == "fonttbl")).skip(1) {
        match node {
            Rtf::Group(definition) => {
                if let Some(index) = definition.iter().position(is_font_number) {
                    let Rtf::Word(_, Some(number)) = &definition[index] else { continue };
                    let mut rest = definition.clone();
                    rest.remove(index);
                    fonts.push((*number, rest));
                }
            }
            Rtf::Word(_, Some(number)) if is_font_number(node) => {
                fonts.extend(flat.take());
                flat = Some((*number, Vec::new()));
            }
            other => {
                if let Some((_, rest)) = flat.as_mut() {
                    rest.push(other.clone());
                }
            }
        }
    }
    fonts.extend(flat);
    fonts
}

/// 颜色表：按分号分成各项，位置就是编号
fn parse_colortbl(children: &[Rtf]) -> Vec<Vec<Rtf>> {
    let mut entries = Vec::new();
    let mut current = Vec::new();
    for node in children.iter().skip_while(|node| !matches!(node, Rtf::Word(name, _) if name == "colortbl")).skip(1) {
        match node {
            Rtf::Text(text) => {
                for (index, part) in text.split(|b| *b == b';').enumerate() {
                    if index > 0 {
                        entries.push(std::mem::take(&mut current));
                    }
                    if !part.iter().all(u8::is_ascii_whitespace) {
                        current.push(Rtf::Text(part.to_vec()));
                    }
                }
            }
            other => current.push(other.clone()),
        }
    }
    entries
}

/// 正文里的字体、颜色编号换成合并后的；样式、列表引用去掉，列表编号的显示文字留作正文
fn remap_rtf(nodes: &[Rtf], fonts: &HashMap<i32, i32>, colors: &HashMap<i32, i32>) -> Vec<Rtf> {
    let mut out = Vec::with_capacity(nodes.len());
    for node in nodes {
        match node {
            Rtf::Word(name, Some(_)) if STYLE_REFS.contains(&name.as_str()) => {}
            Rtf::Word(name, Some(number)) if FONT_REFS.contains(&name.as_str()) => {
                out.push(Rtf::Word(name.clone(), Some(*fonts.get(number).unwrap_or(number))));
            }
            Rtf::Word(name, Some(number)) if COLOR_REFS.contains(&name.as_str()) => {
                out.push(Rtf::Word(name.clone(), Some(*colors.get(number).unwrap_or(number))));
            }
            Rtf::Group(children) if destination(children) == Some("listtext") => {
                let rest: Vec<Rtf> = children.iter()
                    .filter(|child| !matches!(child, Rtf::Word(name, _) if name == "listtext"))
                    .cloned()
                    .collect();
                out.push(Rtf::Group(remap_rtf(&rest, fonts, colors)));
            }
            Rtf::Group(children) => out.push(Rtf::Group(remap_rtf(children, fonts, colors))),
            other => out.push(other.clone()),
        }
    }
    out
}

/// 纯文本写成 RTF 正文；非 ASCII 用 \uN，后面的 ? 是 \uc1 下的替代字符
fn escape_rtf_text(text: &str, out: &mut Vec<u8>) {
    for ch in text.replace("\r\n", "\n").chars() {
        match ch {
            '\\' => out.extend_from_slice(b"\\\\"),
            '{' => out.extend_from_slice(b"\\{"),
            '}' => out.extend_from_slice(b"\\}"),
            '\n' | '\r' => out.extend_from_slice(b"\\par "),
            '\t' => out.extend_from_slice(b"\\tab "),
            ch if ch.is_ascii() => out.push(ch as u8),
            ch => {
                let mut units = [0u16; 2];
                for unit in ch.encode_utf16(&mut units) {
                    out.extend_from_slice(format!("\\u{}?", *unit as i16).as_bytes());
                }
            }
        }
    }
}

pub enum RtfSource<'a> {
    /// RTF 原始数据，以及解析失败时改用的纯文本
    Rtf(&'a [u8], &'a str),
    Text(&'a str),
}

/// 合成一份 RTF 文档；没有一份能解析的 RTF 时返回 None。
///
/// 文档头以第一份 RTF 为准；其余文档的字体、颜色并进字体表和颜色表、正文里的编号
/// 跟着换。每条正文各包一组，格式不会串到下一条。
pub fn merge_rtf(sources: &[RtfSource], separator: &str) -> Option<Vec<u8>> {
    let docs: Vec<Option<RtfDoc>> = sources.iter()
        .map(|source| match source {
            RtfSource::Rtf(data, _) => parse_rtf_doc(data),
            RtfSource::Text(_) => None,
        })
        .collect();
    let base_index = docs.iter().position(Option::is_some)?;
    let base = docs[base_index].as_ref()?;

    let mut fonts = base.fonts.clone();
    let mut font_keys: HashMap<Vec<u8>, i32> = HashMap::new();
    for (number, definition) in &fonts {
        font_keys.entry(rtf_bytes(definition)).or_insert(*number);
    }
    let mut next_font = fonts.iter().map(|(number, _)| *number).max().unwrap_or(-1).saturating_add(1);
    let mut colors = base.colors.clone();
    let mut color_keys: HashMap<Vec<u8>, i32> = HashMap::new();
    for (index, entry) in colors.iter().enumerate() {
        color_keys.entry(rtf_bytes(entry)).or_insert(index as i32);
    }
    let mut font_maps = vec![HashMap::new(); docs.len()];
    let mut color_maps = vec![HashMap::new(); docs.len()];
    for (index, doc) in docs.iter().enumerate() {
        let Some(doc) = doc.as_ref().filter(|_| index != base_index) else { continue };
        for (number, definition) in &doc.fonts {
            let merged = *font_keys.entry(rtf_bytes(definition)).or_insert_with(|| {
                let assigned = next_font;
                next_font = next_font.saturating_add(1);
                fonts.push((assigned, definition.clone()));
                assigned
            });
            font_maps[index].insert(*number, merged);
        }
        for (position, entry) in doc.colors.iter().enumerate() {
            let merged = *color_keys.entry(rtf_bytes(entry)).or_insert_with(|| {
                colors.push(entry.clone());
                colors.len() as i32 - 1
            });
            color_maps[index].insert(position as i32, merged);
        }
    }

    let mut out = b"{\\rtf1 ".to_vec();
    write_rtf(&base.prolog, &mut out);
    let mut font_table = vec![Rtf::Word("fonttbl".into(), None)];
    font_table.extend(fonts.into_iter().map(|(number, definition)| {
        let mut group = vec![Rtf::Word("f".into(), Some(number))];
        group.extend(definition);
        Rtf::Group(group)
    }));
    let mut color_table = vec![Rtf::Word("colortbl".into(), None)];
    for entry in colors {
        color_table.extend(entry);
        color_table.push(Rtf::Text(b";".to_vec()));
    }
    write_rtf(&[Rtf::Group(font_table), Rtf::Group(color_table)], &mut out);
    write_rtf(&base.header, &mut out);

    for (index, source) in sources.iter().enumerate() {
        if index > 0 && !separator.is_empty() {
            // 分隔符的 Unicode 转义只带一个替代字符，不能继承原文档的 uc。
            out.extend_from_slice(b"{\\plain \\uc1 ");
            escape_rtf_text(separator, &mut out);
            out.push(b'}');
        }
        out.push(b'{');
        match (&docs[index], source) {
            (Some(doc), _) if index == base_index => write_rtf(&doc.body, &mut out),
            (Some(doc), _) => {
                out.extend_from_slice(b"\\plain ");
                // 独立 RTF 省略 uc 时默认是 1；合并后必须显式恢复这个默认值。
                let uc = doc.uc.unwrap_or(1);
                out.extend_from_slice(format!("\\uc{uc} ").as_bytes());
                if let Some(deff) = doc.deff {
                    let font = font_maps[index].get(&deff).copied().unwrap_or(deff);
                    out.extend_from_slice(format!("\\f{font} ").as_bytes());
                }
                write_rtf(&remap_rtf(&doc.body, &font_maps[index], &color_maps[index]), &mut out);
            }
            (None, RtfSource::Rtf(_, text) | RtfSource::Text(text)) => {
                out.extend_from_slice(b"\\plain \\uc1 ");
                escape_rtf_text(text, &mut out);
            }
        }
        out.push(b'}');
    }
    out.extend_from_slice(b"}\0");
    Some(out)
}

// ── 图片 ──────────────────────────────────────────────────

/// 拼接后的画布超过这么多像素就不拼：约七张 4K 截图，剪贴板上的位图将近 250 MB
pub const MAX_STITCHED_PIXELS: u64 = 60_000_000;

/// 拼接留白的颜色：认 alpha 的程序里是透明，不认的显示为白
pub const STITCH_BACKGROUND: Rgba<u8> = Rgba([255, 255, 255, 0]);

pub enum ImageSource {
    Png(Vec<u8>),
    /// CF_DIB / CF_DIBV5
    Dib(Vec<u8>),
}

impl ImageSource {
    /// 只读头部取尺寸，不解码
    pub fn dimensions(&self) -> Option<(u32, u32)> {
        match self {
            Self::Png(data) => image::ImageReader::with_format(Cursor::new(data), image::ImageFormat::Png)
                .into_dimensions()
                .ok(),
            Self::Dib(data) => {
                let header_len = u32::from_le_bytes(data.get(0..4)?.try_into().ok()?);
                let (width, height) = if header_len == 12 {
                    let width = u16::from_le_bytes(data.get(4..6)?.try_into().ok()?) as i64;
                    let height = u16::from_le_bytes(data.get(6..8)?.try_into().ok()?) as i64;
                    (width, height)
                } else {
                    let width = i32::from_le_bytes(data.get(4..8)?.try_into().ok()?) as i64;
                    let height = i32::from_le_bytes(data.get(8..12)?.try_into().ok()?) as i64;
                    (width, height.abs())
                };
                (width > 0 && height > 0).then(|| (width as u32, height as u32))
            }
        }
    }
}

/// 竖拼时宽取最大、高相加，横拼反过来；超过上限返回 None
pub fn stitched_size(sizes: &[(u32, u32)], vertical: bool) -> Option<(u32, u32)> {
    let widths = sizes.iter().map(|(w, _)| *w as u64);
    let heights = sizes.iter().map(|(_, h)| *h as u64);
    let (width, height) = if vertical {
        (widths.max()?, heights.sum::<u64>())
    } else {
        (widths.sum::<u64>(), heights.max()?)
    };
    (width > 0 && height > 0 && width.saturating_mul(height) <= MAX_STITCHED_PIXELS)
        .then(|| (width as u32, height as u32))
}

/// 把 image 贴到 canvas 的 (x, y)，超出画布的部分丢掉
pub fn paste_image(canvas: &mut RgbaImage, image: &RgbaImage, x: u32, y: u32) {
    let (canvas_width, canvas_height) = canvas.dimensions();
    if x >= canvas_width || y >= canvas_height {
        return;
    }
    let row_bytes = image.width().min(canvas_width - x) as usize * 4;
    let rows = image.height().min(canvas_height - y) as usize;
    let canvas_stride = canvas_width as usize * 4;
    let image_stride = image.width() as usize * 4;
    let source = image.as_raw();
    let target: &mut [u8] = canvas;
    for row in 0..rows {
        let to = (y as usize + row) * canvas_stride + x as usize * 4;
        let from = row * image_stride;
        target[to..to + row_bytes].copy_from_slice(&source[from..from + row_bytes]);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cf_html(doc: &str) -> Vec<u8> {
        let header_len = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\n".len();
        let start = doc.find("<!--StartFragment-->").unwrap() + "<!--StartFragment-->".len();
        let end = doc.find("<!--EndFragment-->").unwrap();
        format!(
            "Version:0.9\r\nStartHTML:{:010}\r\nEndHTML:{:010}\r\nStartFragment:{:010}\r\nEndFragment:{:010}\r\n{}",
            header_len,
            header_len + doc.len(),
            header_len + start,
            header_len + end,
            doc
        )
        .into_bytes()
    }

    fn excel(class_color: &str, cell: &str) -> HtmlPart {
        let doc = format!(
            "<html xmlns:o=\"urn:schemas-microsoft-com:office:office\" xmlns:x=\"urn:schemas-microsoft-com:office:excel\">\r\n<head>\r\n<meta name=ProgId content=Excel.Sheet>\r\n<style>\r\n<!--\r\ntd\r\n\t{{mso-number-format:General;}}\r\n.xl65\r\n\t{{color:{class_color};}}\r\n-->\r\n</style>\r\n</head>\r\n<body link=\"#0563C1\">\r\n\r\n<table border=0 width=72>\r\n<!--StartFragment-->\r\n <tr height=19>\r\n  <td class=xl65 x:num>{cell}</td>\r\n </tr>\r\n<!--EndFragment-->\r\n</table>\r\n\r\n</body>\r\n</html>\r\n"
        );
        HtmlPart::from_cf_html(&cf_html(&doc)).unwrap()
    }

    #[test]
    fn text_is_joined_with_crlf_separators() {
        let pieces = vec!["a".to_string(), "b".to_string(), "c".to_string()];
        assert_eq!(join_text(&pieces, "\n"), "a\r\nb\r\nc");
        assert_eq!(join_text(&pieces, "\r\n\n"), "a\r\n\r\nb\r\n\r\nc");
        assert_eq!(join_text(&pieces, ", "), "a, b, c");
    }

    #[test]
    fn file_lists_are_merged_without_duplicates() {
        let lists = vec![
            vec![r"C:\a.txt".to_string(), r"C:\b.txt".to_string()],
            vec![r"c:\A.TXT".to_string(), r"D:\c.png".to_string()],
        ];
        assert_eq!(union_files(&lists), vec![r"C:\a.txt", r"C:\b.txt", r"D:\c.png"]);
    }

    #[test]
    fn table_fragments_keep_their_table_and_namespaces() {
        let part = excel("red", "1");
        assert_eq!(part.open, "<table border=0 width=72>");
        assert_eq!(part.close, "</table>");
        assert!(part.fragment.contains("<td class=xl65 x:num>1</td>"));
        assert_eq!(part.prog_id.as_deref(), Some("Excel.Sheet"));

        let merged = merge_html(&[part.clone(), part], "\n");
        assert!(merged.starts_with("<html xmlns:o=\"urn:schemas-microsoft-com:office:office\" xmlns:x="));
        assert!(merged.contains("<meta name=ProgId content=Excel.Sheet>"));
        assert_eq!(merged.matches("<table border=0 width=72>").count(), 2);
        assert!(merged.contains("</table><br><table"));
        // 两次定义相同，不必改名
        assert!(!merged.contains("xl65-"));
    }

    #[test]
    fn conflicting_classes_in_later_parts_are_renamed() {
        let merged = merge_html(&[excel("red", "1"), excel("blue", "2")], "");
        assert!(merged.contains(".xl65\r\n\t{color:red;}"));
        assert!(merged.contains(".xl65-2\r\n\t{color:blue;}"));
        assert!(merged.contains("<td class=xl65 x:num>1</td>"));
        assert!(merged.contains("<td class=xl65-2 x:num>2</td>"));
        // 元素选择器不改名，都保留
        assert_eq!(merged.matches("{mso-number-format:General;}").count(), 2);
    }

    #[test]
    fn stray_markers_inside_a_fragment_do_not_cut_the_merged_one_short() {
        // 头部偏移量把一个多余的结束标记也圈进了片段
        let doc = "<html><body><!--StartFragment-->A<!--EndFragment-->B<!--EndFragment--></body></html>";
        let start = doc.find("A").unwrap();
        let end = doc.rfind("<!--EndFragment-->").unwrap();
        let part = HtmlPart::from_document(doc, Some((start, end)));
        assert_eq!(part.fragment, "AB");
        let merged = merge_html(&[part, HtmlPart::from_text("next")], "\n");
        assert_eq!(merged.matches("<!--EndFragment-->").count(), 1);
        let (start, end) = fragment_markers(&merged).unwrap();
        assert_eq!(&merged[start..end], "AB<br>next");
    }

    #[test]
    fn plain_text_becomes_escaped_html() {
        assert_eq!(text_to_html("a < b & c"), "a &lt; b &amp; c");
        assert_eq!(text_to_html("x\r\ny\nz"), "x<br>y<br>z");
        assert_eq!(text_to_html("  two   spaces"), "&nbsp; two &nbsp; spaces");
        assert_eq!(text_to_html("\tq"), "&nbsp;&nbsp;&nbsp;&nbsp;q");
    }

    #[test]
    fn parts_from_different_programs_drop_the_prog_id() {
        let mut word = HtmlPart::from_text("w");
        word.prog_id = Some("Word.Document".into());
        let merged = merge_html(&[excel("red", "1"), word, HtmlPart::from_text("plain")], "\n");
        assert!(!merged.contains("ProgId"));
        let merged = merge_html(&[excel("red", "1"), HtmlPart::from_text("plain")], "\n");
        assert!(merged.contains("ProgId content=Excel.Sheet"));
    }

    #[test]
    fn documents_without_header_offsets_use_the_markers_or_the_body() {
        let part = HtmlPart::from_document("<html><body><b>x</b><!--StartFragment--><i>y</i><!--EndFragment--></body></html>", None);
        assert_eq!(part.fragment, "<i>y</i>");
        assert_eq!(part.open, "<b></b>");
        let part = HtmlPart::from_document("<HTML><BODY class=c><p>z</p></BODY></HTML>", None);
        assert_eq!(part.fragment, "<p>z</p>");
        assert_eq!(HtmlPart::from_cf_html(b"no html here"), None);
    }

    #[test]
    fn offsets_inside_a_character_fall_back_to_the_markers() {
        let doc = "<html><body><!--StartFragment-->中文<!--EndFragment--></body></html>";
        let mut raw = cf_html(doc);
        // StartFragment 往后挪一个字节，落进「中」的中间
        let header = String::from_utf8(raw.clone()).unwrap();
        let start = header_offset(&header, "StartFragment").unwrap();
        let patched = header.replacen(&format!("StartFragment:{start:010}"), &format!("StartFragment:{:010}", start + 1), 1);
        raw = patched.into_bytes();
        assert_eq!(HtmlPart::from_cf_html(&raw).unwrap().fragment, "中文");
    }

    #[test]
    fn quoted_angle_brackets_do_not_end_a_tag() {
        let html = "<td title='a>b' class=\"x y\">t</td>";
        let renames = HashMap::from([("y".to_string(), "y-2".to_string())]);
        assert_eq!(rename_classes(html, &renames), "<td title='a>b' class=\"x y-2\">t</td>");
    }

    #[test]
    fn css_keeps_at_rules_and_comments_out_of_class_names() {
        let rules = parse_css("<!--\r\n/* Font Definitions */\r\n@font-face {font-family:宋体;}\r\np.MsoNormal, li.MsoNormal\r\n\t{margin:0cm; font-family:\"a.b\";}\r\n@page WordSection1 {size:595.3pt 841.9pt;}\r\n-->");
        assert_eq!(rules.len(), 3);
        let definitions = class_definitions(&rules);
        assert_eq!(definitions.keys().collect::<Vec<_>>(), vec!["MsoNormal"]);
    }

    fn rtf(text: &str) -> Vec<u8> {
        text.as_bytes().to_vec()
    }

    #[test]
    fn rtf_fonts_and_colors_are_merged_and_renumbered() {
        let first = rtf(r"{\rtf1\ansi\deff0{\fonttbl{\f0\fswiss Arial;}}{\colortbl ;\red255\green0\blue0;}\pard\f0\cf1 One\par}");
        let second = rtf(r"{\rtf1\ansi\deff0{\fonttbl{\f0\froman Times;}{\f1\fswiss Arial;}}{\colortbl ;\red0\green0\blue255;\red255\green0\blue0;}\pard\f0\cf1 Two\f1\cf2 Three\par}");
        let merged = merge_rtf(&[RtfSource::Rtf(&first, "One"), RtfSource::Rtf(&second, "Two")], "\n").unwrap();
        let merged = String::from_utf8(merged).unwrap();
        assert!(merged.contains(r"{\fonttbl {\f0 \fswiss Arial;}{\f1 \froman Times;}}"), "{merged}");
        assert!(merged.contains(r"{\colortbl ;\red255 \green0 \blue0 ;\red0 \green0 \blue255 ;}"), "{merged}");
        // 第二份：Times 成了 f1，原来的 f1（Arial）复用 f0；蓝色成了 2，红色复用 1
        assert!(merged.contains(r"\f1 \cf2 Two\f0 \cf1 Three"), "{merged}");
        assert!(merged.contains(r"\f0 \cf1 One"));
        assert!(merged.ends_with("}\0"));
        assert_eq!(parse_rtf_doc(merged.as_bytes()).map(|doc| doc.fonts.len()), Some(2));
    }

    #[test]
    fn rtf_style_and_list_references_of_later_documents_are_dropped() {
        let first = rtf(r"{\rtf1{\fonttbl{\f0 A;}}{\stylesheet{\s1 Heading;}}\pard\s1 Title\par}");
        let second = rtf(r"{\rtf1\uc2{\fonttbl{\f0 A;}}{\stylesheet{\s1 Other;}}{\*\listtable{\list}}\pard\s1\ls1\ilvl0{\listtext \'b7\tab}Item\par}");
        let merged = String::from_utf8(merge_rtf(&[RtfSource::Rtf(&first, ""), RtfSource::Rtf(&second, "")], "").unwrap()).unwrap();
        assert_eq!(merged.matches("stylesheet").count(), 1);
        assert!(!merged.contains("listtable"));
        assert!(merged.contains(r"\pard \s1 Title"));
        assert!(merged.contains(r"{\plain \uc2 \pard {\'b7\tab }Item\par }"), "{merged}");
    }

    #[test]
    fn plain_text_pieces_are_escaped_into_the_rtf() {
        let first = rtf(r"{\rtf1{\fonttbl{\f0 A;}}x}");
        let merged = merge_rtf(&[RtfSource::Text("{中}\\\tz"), RtfSource::Rtf(&first, "x")], "--\n").unwrap();
        let merged = String::from_utf8(merged).unwrap();
        assert!(merged.contains(r"{\plain \uc1 \{\u20013?\}\\\tab z}{\plain \uc1 --\par }{x}"), "{merged}");
        assert!(merge_rtf(&[RtfSource::Text("a"), RtfSource::Rtf(b"not rtf", "b")], "").is_none());
    }

    #[test]
    fn later_rtf_documents_restore_their_own_unicode_fallback_count() {
        for base_uc in [0, 1, 2] {
            let first = format!(r"{{\rtf1\ansi\uc{base_uc}{{\fonttbl{{\f0 Arial;}}}}A}}");
            for explicit_uc in [None, Some(0), Some(1), Some(2)] {
                let uc = explicit_uc.unwrap_or(1);
                let setting = explicit_uc.map(|n| format!(r"\uc{n}")).unwrap_or_default();
                let second = format!(
                    r"{{\rtf1\ansi{setting}{{\fonttbl{{\f0 Arial;}}}}\u20013{}B}}",
                    "?".repeat(uc),
                );
                let merged = merge_rtf(&[
                    RtfSource::Rtf(first.as_bytes(), "A"),
                    RtfSource::Rtf(second.as_bytes(), "中B"),
                ], "|").unwrap();
                let doc = parse_rtf_doc(&merged).unwrap();
                // 最后一段显式声明自己的 uc，省略时恢复独立文档的默认值 1。
                let Some(Rtf::Group(body)) = doc.body.last() else { panic!("缺少最后一段正文") };
                assert_eq!(body.get(1), Some(&Rtf::Word("uc".into(), Some(uc as i32))));
            }
        }
    }

    #[test]
    fn unicode_separators_always_use_one_fallback_character() {
        for base_uc in [0, 1, 2] {
            let first = format!(r"{{\rtf1\ansi\uc{base_uc}{{\fonttbl{{\f0 Arial;}}}}A}}");
            let second = br"{\rtf1\ansi{\fonttbl{\f0 Arial;}}B}";
            for (separator, encoded) in [
                ("中X", r"\u20013?X"),
                ("🙂X", r"\u-10179?\u-8638?X"),
            ] {
                let merged = merge_rtf(&[
                    RtfSource::Rtf(first.as_bytes(), "A"),
                    RtfSource::Rtf(second, "B"),
                ], separator).unwrap();
                let merged = String::from_utf8(merged).unwrap();
                assert!(merged.contains(&format!(r"{{\plain \uc1 {encoded}}}")), "{merged}");
            }
        }
    }

    #[test]
    fn unparsable_rtf_falls_back_to_its_text() {
        let good = rtf(r"{\rtf1{\fonttbl{\f0 A;}}ok}");
        let merged = merge_rtf(&[RtfSource::Rtf(&good, "ok"), RtfSource::Rtf(b"garbage", "plain")], "").unwrap();
        assert!(String::from_utf8(merged).unwrap().contains(r"{\plain \uc1 plain}"));
    }

    #[test]
    fn rtf_binary_data_and_escapes_survive_a_round_trip() {
        let data = b"{\\rtf1 a\\{b\\}\\\\{\\pict\\bin3 }{\\x}c\\'e9\\u-3? d}".to_vec();
        let doc = parse_rtf_doc(&data).unwrap();
        let mut out = Vec::new();
        write_rtf(&doc.body, &mut out);
        assert_eq!(out, b"a\\{b\\}\\\\{\\pict \\bin3 }{\\x}c\\'e9\\u-3 ? d".to_vec());
    }

    #[test]
    fn deeply_nested_rtf_is_rejected_instead_of_overflowing() {
        let mut data = b"{\\rtf1 ".to_vec();
        data.extend(std::iter::repeat(b'{').take(10_000));
        assert!(parse_rtf_doc(&data).is_none());
    }

    #[test]
    fn flat_font_tables_are_read_too() {
        let doc = parse_rtf_doc(br"{\rtf1{\fonttbl\f0\fswiss Helvetica;\f1\fmodern Courier;}x}").unwrap();
        assert_eq!(doc.fonts.iter().map(|(n, _)| *n).collect::<Vec<_>>(), vec![0, 1]);
    }

    #[test]
    fn stitched_canvas_sizes_and_limit() {
        assert_eq!(stitched_size(&[(10, 5), (20, 3)], true), Some((20, 8)));
        assert_eq!(stitched_size(&[(10, 5), (20, 3)], false), Some((30, 5)));
        assert_eq!(stitched_size(&[], true), None);
        assert_eq!(stitched_size(&[(10_000, 6_001)], true), None);
    }

    #[test]
    fn images_are_pasted_into_place_and_clipped() {
        let mut canvas = RgbaImage::from_pixel(3, 4, STITCH_BACKGROUND);
        let red = RgbaImage::from_pixel(2, 2, Rgba([255, 0, 0, 255]));
        paste_image(&mut canvas, &red, 0, 0);
        paste_image(&mut canvas, &red, 2, 3);
        assert_eq!(canvas.get_pixel(1, 1), &Rgba([255, 0, 0, 255]));
        assert_eq!(canvas.get_pixel(2, 0), &STITCH_BACKGROUND);
        assert_eq!(canvas.get_pixel(2, 3), &Rgba([255, 0, 0, 255]));
        paste_image(&mut canvas, &red, 5, 5);
    }

    #[test]
    fn image_dimensions_come_from_the_headers() {
        let image = RgbaImage::from_pixel(7, 3, Rgba([1, 2, 3, 4]));
        let mut png = Vec::new();
        image.write_to(&mut Cursor::new(&mut png), image::ImageFormat::Png).unwrap();
        assert_eq!(ImageSource::Png(png).dimensions(), Some((7, 3)));
        let mut dib = vec![0u8; 40];
        dib[0..4].copy_from_slice(&40u32.to_le_bytes());
        dib[4..8].copy_from_slice(&7i32.to_le_bytes());
        dib[8..12].copy_from_slice(&(-3i32).to_le_bytes());
        assert_eq!(ImageSource::Dib(dib).dimensions(), Some((7, 3)));
        assert_eq!(ImageSource::Dib(vec![1, 2]).dimensions(), None);
    }
}
