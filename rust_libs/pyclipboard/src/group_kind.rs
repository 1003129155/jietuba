//! 分组类型：存在 groups.group_type 里的整数，0..=3 是库的存储格式，不能改。
//! 4、5 只在未发布的开发库里出现过（图片、文件筛选分组），打开库时迁成 3 加规则。

use rusqlite::types::Value;
use serde::{Deserialize, Serialize};

use crate::{file_kind, text_kind};

/// 普通分组：什么内容都收
pub const NORMAL: i64 = 0;
/// 快速启动分组：收文件、路径和链接文本，点击时打开
pub const FILE: i64 = 1;
/// 隐藏分组：行为同普通分组，只是不在面板上显示
pub const HIDDEN: i64 = 2;
/// 筛选分组：自己不存内容，显示全部历史里符合 groups.filter_rule 的内容
pub const FILTER: i64 = 3;

pub fn is_known(group_type: i64) -> bool {
    (NORMAL..=FILTER).contains(&group_type)
}

/// 分组能不能存这类内容；`sub_kind` 是条目的子类型
pub fn accepts(group_type: i64, content_type: &str, sub_kind: &str) -> bool {
    match group_type {
        FILTER => false,
        FILE => content_type == "file" || (content_type == "text" && matches!(sub_kind, text_kind::PATH | text_kind::URL)),
        _ => true,
    }
}

/// 来源规则里代表本程序自己：几种发布包和开发构建的 exe 名都以它开头
pub const OWN_APP: &str = "jietuba";

/// 筛选条件：内容类型和来源应用，两项同时给时都要满足。也用于面板上的类型筛选
#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct FilterRule {
    /// text / image / file，文本子类型 url / color / path，或 `file:` 加文件子类型（如 `file:spreadsheet`）；
    /// text、file 包括它们的全部子类型。文本子类型不带前缀是早先的写法，已存进分组里的规则照常可用
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kind: Option<String>,
    /// 来源 exe 文件名，不分大小写；OWN_APP 匹配本程序的所有 exe
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub app: Option<String>,
}

const CONTENT_KINDS: [&str; 3] = ["text", "image", "file"];
const TEXT_SUB_KINDS: [&str; 3] = [text_kind::URL, text_kind::COLOR, text_kind::PATH];
const FILE_KIND_PREFIX: &str = "file:";

/// 规则里的 `file:<子类型>` 对应的文件子类型
fn file_sub_kind(kind: &str) -> Option<&str> {
    kind.strip_prefix(FILE_KIND_PREFIX).filter(|sub_kind| file_kind::is_known(sub_kind))
}

/// 规则能写的全部类型，按内容类型、文本子类型、文件子类型排
pub fn filter_kinds() -> Vec<String> {
    CONTENT_KINDS
        .iter()
        .chain(TEXT_SUB_KINDS.iter())
        .map(|kind| kind.to_string())
        .chain(file_kind::ids().map(|id| format!("{FILE_KIND_PREFIX}{id}")))
        .collect()
}

impl FilterRule {
    pub fn kind(kind: &str) -> Self {
        Self { kind: Some(kind.to_string()), app: None }
    }

    /// 解析并校验分组里存的规则；至少要有一项条件，否则就是全部历史
    pub fn parse(json: &str) -> Result<Self, String> {
        let rule: Self = serde_json::from_str(json).map_err(|e| format!("筛选规则格式错误: {e}"))?;
        rule.check()?;
        if rule.is_empty() {
            return Err("筛选规则至少要有一项条件".to_string());
        }
        Ok(rule)
    }

    pub fn to_json(&self) -> String {
        serde_json::to_string(self).unwrap_or_default()
    }

    pub fn is_empty(&self) -> bool {
        self.kind.is_none() && self.app.is_none()
    }

    pub fn check(&self) -> Result<(), String> {
        if let Some(kind) = &self.kind {
            if !CONTENT_KINDS.contains(&kind.as_str()) && !TEXT_SUB_KINDS.contains(&kind.as_str()) && file_sub_kind(kind).is_none() {
                return Err(format!("未知的内容类型: {kind}"));
            }
        }
        if self.app.as_deref().is_some_and(|app| app.trim().is_empty()) {
            return Err("来源应用不能为空".to_string());
        }
        Ok(())
    }

    /// 对应的 SQL 条件（用 AND 连接）及参数
    pub fn sql_conditions(&self) -> (Vec<&'static str>, Vec<Value>) {
        let mut conditions = Vec::new();
        let mut params = Vec::new();
        if let Some(kind) = &self.kind {
            if TEXT_SUB_KINDS.contains(&kind.as_str()) {
                conditions.push("content_type = 'text' AND sub_kind = ?");
                params.push(Value::from(kind.clone()));
            } else if let Some(sub_kind) = file_sub_kind(kind) {
                conditions.push("content_type = 'file' AND sub_kind = ?");
                params.push(Value::from(sub_kind.to_string()));
            } else {
                conditions.push("content_type = ?");
                params.push(Value::from(kind.clone()));
            }
        }
        if let Some(app) = &self.app {
            if app.eq_ignore_ascii_case(OWN_APP) {
                // LIKE 对 ASCII 不分大小写；exe 名里的 _ 是通配符，这里无妨
                conditions.push("source_app LIKE 'jietuba%'");
            } else {
                conditions.push("source_app = ? COLLATE NOCASE");
                params.push(Value::from(app.trim().to_string()));
            }
        }
        (conditions, params)
    }
}

/// 开发库里旧的图片、文件筛选分组，以及还没有规则的文字筛选分组，对应的规则
pub(crate) const LEGACY_FILTERS: [(i64, &str); 3] = [(3, "text"), (4, "image"), (5, "file")];

#[cfg(test)]
mod tests {
    use super::*;
    use crate::file_kind;

    #[test]
    fn groups_accept_what_their_type_allows() {
        for (content_type, sub_kind) in [("text", ""), ("text", "url"), ("text", "path"), ("image", ""), ("file", "")] {
            assert!(accepts(NORMAL, content_type, sub_kind));
            assert!(accepts(HIDDEN, content_type, sub_kind));
            assert!(!accepts(FILTER, content_type, sub_kind));
            let quick_launch = content_type == "file" || sub_kind == "path" || sub_kind == "url";
            assert_eq!(accepts(FILE, content_type, sub_kind), quick_launch, "{content_type} {sub_kind}");
        }
    }

    #[test]
    fn unknown_types_are_not_known() {
        assert!(is_known(NORMAL) && is_known(FILTER));
        assert!(!is_known(-1) && !is_known(4) && !is_known(5));
    }

    #[test]
    fn rules_need_a_known_condition() {
        assert_eq!(FilterRule::parse(r#"{"kind":"url"}"#), Ok(FilterRule::kind("url")));
        let both = FilterRule::parse(r#"{"kind":"image","app":"Weixin.exe"}"#).unwrap();
        assert_eq!(both.app.as_deref(), Some("Weixin.exe"));
        assert_eq!(FilterRule::parse(&both.to_json()), Ok(both));
        for bad in ["{}", r#"{"kind":"email"}"#, r#"{"app":"  "}"#, "not json", r#"{"kind":1}"#] {
            assert!(FilterRule::parse(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn file_kinds_are_named_with_a_prefix() {
        assert_eq!(FilterRule::parse(r#"{"kind":"file:spreadsheet"}"#), Ok(FilterRule::kind("file:spreadsheet")));
        assert!(FilterRule::parse(r#"{"kind":"file:folder"}"#).is_ok());
        // 不带前缀的文件子类型、写成 text: 的文本子类型、后缀本身都不算
        for bad in ["spreadsheet", "file:", "file:xlsx", "file:url", "text:url", "file:Spreadsheet"] {
            assert!(FilterRule::parse(&FilterRule::kind(bad).to_json()).is_err(), "{bad}");
        }
    }

    #[test]
    fn every_listed_kind_is_a_valid_rule() {
        let kinds = filter_kinds();
        assert_eq!(&kinds[..6], ["text", "image", "file", "url", "color", "path"]);
        assert!(kinds.contains(&"file:markdown".to_string()) && kinds.contains(&"file:folder".to_string()));
        assert_eq!(kinds.len(), 6 + file_kind::ids().count());
        for kind in &kinds {
            assert_eq!(FilterRule::parse(&FilterRule::kind(kind).to_json()), Ok(FilterRule::kind(kind)), "{kind}");
        }
    }

    #[test]
    fn rules_turn_into_sql() {
        let (conditions, params) = FilterRule::kind("text").sql_conditions();
        assert_eq!((conditions, params), (vec!["content_type = ?"], vec![Value::from("text".to_string())]));
        let (conditions, _) = FilterRule::kind("color").sql_conditions();
        assert_eq!(conditions, vec!["content_type = 'text' AND sub_kind = ?"]);
        let (conditions, params) = FilterRule::kind("file:pdf").sql_conditions();
        assert_eq!((conditions, params), (vec!["content_type = 'file' AND sub_kind = ?"], vec![Value::from("pdf".to_string())]));
        let own = FilterRule { kind: None, app: Some("JIETUBA".to_string()) };
        assert_eq!(own.sql_conditions(), (vec!["source_app LIKE 'jietuba%'"], vec![]));
        assert_eq!(FilterRule::default().sql_conditions(), (vec![], vec![]));
    }
}
