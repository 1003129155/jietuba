//! 文件子类型：文件记录存在 clipboard.sub_kind 里的字符串，取值是库的存储格式，不能改。
//!
//! 只看路径里的后缀，不访问文件系统：监听线程上查网络路径可能卡住几秒。
//! 一条记录里的文件都是同一类才记成这一类；没有后缀的记成文件夹（偶尔会是 LICENSE 这类
//! 没后缀的文件，只影响图标）；后缀认不出、或者几个文件不是同一类的，记成空。
//! 增删类别或后缀改这张表，并把 database::SUB_KIND_RULES_VERSION 加一，旧记录打开库时重判。

pub struct Kind {
    pub id: &'static str,
    /// 小写，不带点
    pub extensions: &'static [&'static str],
}

/// 路径没有后缀
pub const FOLDER: &str = "folder";

pub const KINDS: &[Kind] = &[
    Kind { id: "image", extensions: &["png", "jpg", "jpeg", "gif", "webp", "bmp", "tif", "tiff", "ico", "heic"] },
    // et、wps、dps 是 WPS 自己的表格、文字、演示格式
    Kind { id: "spreadsheet", extensions: &["xls", "xlsx", "xlsm", "xlsb", "csv", "ods", "et"] },
    Kind { id: "document", extensions: &["doc", "docx", "rtf", "odt", "wps"] },
    Kind { id: "presentation", extensions: &["ppt", "pptx", "pps", "ppsx", "odp", "dps"] },
    Kind { id: "pdf", extensions: &["pdf"] },
    Kind { id: "markdown", extensions: &["md", "markdown"] },
    Kind { id: "text", extensions: &["txt", "log"] },
    Kind { id: "archive", extensions: &["zip", "7z", "rar", "tar", "gz", "tgz", "bz2", "xz"] },
    Kind { id: "audio", extensions: &["mp3", "wav", "flac", "aac", "m4a", "ogg", "wma"] },
    Kind { id: "video", extensions: &["mp4", "mkv", "mov", "avi", "wmv", "webm", "flv", "m4v"] },
];

/// 能写进库的全部取值（不含表示「认不出」的空字符串）
pub fn ids() -> impl Iterator<Item = &'static str> {
    KINDS.iter().map(|kind| kind.id).chain(std::iter::once(FOLDER))
}

pub fn is_known(id: &str) -> bool {
    ids().any(|known| known == id)
}

/// 文件记录内容 `{"files": [...]}` 里的路径；内容不是这个格式时为空
pub fn paths(content: &str) -> Vec<String> {
    serde_json::from_str::<serde_json::Value>(content)
        .ok()
        .and_then(|json| json.get("files").and_then(|files| files.as_array()).cloned())
        .unwrap_or_default()
        .iter()
        .filter_map(|file| file.as_str().map(str::to_string))
        .collect()
}

pub fn detect(paths: &[String]) -> &'static str {
    let mut kinds = paths.iter().map(|path| kind_of(path));
    let Some(first) = kinds.next() else {
        return "";
    };
    if kinds.all(|kind| kind == first) {
        first
    } else {
        ""
    }
}

fn kind_of(path: &str) -> &'static str {
    let name = path.trim_end_matches(['\\', '/']).rsplit(['\\', '/']).next().unwrap_or("");
    // 「.gitignore」这种只有点开头的名字也算没有后缀
    let extension = match name.rsplit_once('.') {
        Some((stem, extension)) if !stem.is_empty() && !extension.is_empty() => extension,
        _ => return FOLDER,
    };
    KINDS
        .iter()
        .find(|kind| kind.extensions.iter().any(|known| known.eq_ignore_ascii_case(extension)))
        .map_or("", |kind| kind.id)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    fn kind(paths: &[&str]) -> &'static str {
        detect(&paths.iter().map(|path| path.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn the_table_has_no_overlaps() {
        let mut seen = HashSet::new();
        for id in ids() {
            assert!(seen.insert(id), "duplicate id {id}");
            assert!(!id.is_empty() && !id.contains(':'), "{id}");
        }
        let mut extensions = HashSet::new();
        for kind in KINDS {
            for extension in kind.extensions {
                assert!(extensions.insert(*extension), "{extension} is in two kinds");
                assert_eq!(*extension, extension.to_ascii_lowercase());
                assert!(!extension.starts_with('.'));
            }
        }
    }

    #[test]
    fn files_are_sorted_by_their_extension() {
        assert_eq!(kind(&[r"C:\Users\me\Desktop\Q4 报价单.xlsm"]), "spreadsheet");
        assert_eq!(kind(&[r"D:\notes\README.MD"]), "markdown");
        assert_eq!(kind(&[r"\\server\share\合同.docx"]), "document");
        assert_eq!(kind(&["C:/a/b/clip.mp4"]), "video");
        assert_eq!(kind(&[r"C:\backup\site.tar.gz"]), "archive");
        assert_eq!(kind(&[r"C:\a\x.png", r"C:\b\y.JPG"]), "image");
    }

    #[test]
    fn paths_without_an_extension_are_folders() {
        assert_eq!(kind(&[r"C:\Users\me\Desktop\资料"]), FOLDER);
        assert_eq!(kind(&[r"C:\Users\me\Desktop\资料\"]), FOLDER);
        assert_eq!(kind(&[r"C:\repo\.gitignore"]), FOLDER);
        assert_eq!(kind(&[r"C:\a", r"C:\b"]), FOLDER);
    }

    #[test]
    fn unknown_and_mixed_records_stay_plain() {
        assert_eq!(kind(&[r"C:\print\model.3mf"]), "");
        assert_eq!(kind(&[r"C:\a\x.png", r"C:\a\y.pdf"]), "");
        assert_eq!(kind(&[r"C:\a\x.png", r"C:\a\folder"]), "");
        assert_eq!(kind(&[r"C:\a\trailing."]), FOLDER);
        assert_eq!(kind(&[]), "");
    }

    #[test]
    fn file_contents_give_their_paths() {
        let content = serde_json::json!({ "files": [r"C:\a.txt", r"D:\图.png"] }).to_string();
        assert_eq!(paths(&content), vec![r"C:\a.txt".to_string(), r"D:\图.png".to_string()]);
        for bad in ["", "not json", r#"{"files": "C:\\a"}"#, r#"{"other": []}"#] {
            assert!(paths(bad).is_empty(), "{bad}");
        }
    }

    #[test]
    fn known_ids_are_the_table_plus_folder() {
        assert!(is_known("spreadsheet") && is_known(FOLDER));
        assert!(!is_known("") && !is_known("url") && !is_known("xlsx"));
    }
}
