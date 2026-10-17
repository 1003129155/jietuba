//! 分组类型：存在 groups.group_type 里的整数，数值是库的存储格式，不能改。

/// 普通分组：什么内容都收
pub const NORMAL: i64 = 0;
/// 快速启动分组：只收文件，点击时打开
pub const FILE: i64 = 1;
/// 隐藏分组：行为同普通分组，只是不在面板上显示
pub const HIDDEN: i64 = 2;
/// 筛选分组：自己不存内容，显示全部历史里某一类内容。每个类型一个取值，库结构不用加列
pub const FILTER_TEXT: i64 = 3;
pub const FILTER_IMAGE: i64 = 4;
pub const FILTER_FILE: i64 = 5;

pub fn is_known(group_type: i64) -> bool {
    (NORMAL..=FILTER_FILE).contains(&group_type)
}

/// 筛选分组筛的内容类型；其他类型的分组返回 None
pub fn filter_content_type(group_type: i64) -> Option<&'static str> {
    match group_type {
        FILTER_TEXT => Some("text"),
        FILTER_IMAGE => Some("image"),
        FILTER_FILE => Some("file"),
        _ => None,
    }
}

/// 分组能不能存这类内容
pub fn accepts(group_type: i64, content_type: &str) -> bool {
    if filter_content_type(group_type).is_some() {
        return false;
    }
    group_type != FILE || content_type == "file"
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_filter_types_name_a_content_type() {
        assert_eq!(filter_content_type(FILTER_TEXT), Some("text"));
        assert_eq!(filter_content_type(FILTER_IMAGE), Some("image"));
        assert_eq!(filter_content_type(FILTER_FILE), Some("file"));
        for group_type in [NORMAL, FILE, HIDDEN, 6, -1] {
            assert_eq!(filter_content_type(group_type), None);
        }
    }

    #[test]
    fn groups_accept_what_their_type_allows() {
        for content_type in ["text", "image", "file"] {
            assert!(accepts(NORMAL, content_type));
            assert!(accepts(HIDDEN, content_type));
            assert!(!accepts(FILTER_TEXT, content_type));
            assert!(!accepts(FILTER_IMAGE, content_type));
            assert!(!accepts(FILTER_FILE, content_type));
            assert_eq!(accepts(FILE, content_type), content_type == "file");
        }
    }

    #[test]
    fn unknown_types_are_not_known() {
        assert!(is_known(NORMAL) && is_known(FILTER_FILE));
        assert!(!is_known(-1) && !is_known(FILTER_FILE + 1));
    }
}
