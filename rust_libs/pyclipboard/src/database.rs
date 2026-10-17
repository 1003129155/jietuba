use rusqlite::{Connection, OptionalExtension, params, params_from_iter};
use rusqlite::types::Value;
use crate::group_kind;
use crate::types::{PyClipboardItem, PyPaginatedResult, PyGroup};
use std::path::{Path, PathBuf};

// 压缩阈值：超过 100KB 的 data 用 zstd 压缩
const COMPRESS_THRESHOLD: usize = 100 * 1024;

/// 读一条记录要的列，顺序与 item_from_row 对应
const ITEM_COLUMNS: &str = "id, title, content, html_content, content_type, image_id, thumbnail, is_pinned, \
                            paste_count, source_app, char_count, created_at, updated_at";

/// 排到最上面用的 item_order：比现有最大值大 1000
const TOP_ORDER: &str = "(SELECT COALESCE(MAX(item_order), 0) + 1000 FROM clipboard)";

/// 既没置顶、也没加入分组的条目：一键清理和超出条数上限时只删这些
const UNPROTECTED: &str = "is_pinned = 0 AND group_id IS NULL";

fn now() -> i64 {
    chrono::Local::now().timestamp()
}

fn item_from_row(row: &rusqlite::Row) -> rusqlite::Result<PyClipboardItem> {
    Ok(PyClipboardItem {
        id: row.get(0)?,
        title: row.get(1)?,
        content: row.get(2)?,
        html_content: row.get(3)?,
        content_type: row.get(4)?,
        image_id: row.get(5)?,
        thumbnail: row.get(6)?,
        is_pinned: row.get::<_, i64>(7)? != 0,
        paste_count: row.get(8)?,
        source_app: row.get(9)?,
        char_count: row.get(10)?,
        created_at: row.get(11)?,
        updated_at: row.get(12)?,
    })
}

/// 图片按 image_id 存成 images 目录下的 PNG
pub fn image_file(images_dir: &Path, image_id: &str) -> PathBuf {
    images_dir.join(format!("{image_id}.png"))
}

/// 超过阈值的格式数据用 zstd 压缩，返回 (要存的数据, 是否已压缩)；压缩失败时存原始数据
pub fn compress_format(data: Vec<u8>) -> (Vec<u8>, bool) {
    if data.len() <= COMPRESS_THRESHOLD {
        return (data, false);
    }
    match zstd::encode_all(data.as_slice(), 3) {
        Ok(compressed) => (compressed, true),
        Err(_) => (data, false),
    }
}

/// 给旧库补上后来才加的列
fn add_column_if_missing(conn: &Connection, table: &str, column: &str, definition: &str) -> Result<(), String> {
    let exists: bool = conn.query_row(
        "SELECT COUNT(*) > 0 FROM pragma_table_info(?1) WHERE name = ?2",
        params![table, column],
        |row| row.get(0),
    ).map_err(|e| format!("检查表结构失败: {}", e))?;
    if !exists {
        conn.execute(&format!("ALTER TABLE {table} ADD COLUMN {column} {definition}"), [])
            .map_err(|e| format!("升级表结构失败: {}", e))?;
    }
    Ok(())
}

/// 能拖拽排序的一组记录：某个分组里的内容，或全部分组；界面按 item_order 升序排列
#[derive(Clone, Copy)]
enum OrderScope {
    GroupItems(i64),
    Groups,
}

impl OrderScope {
    fn table(self) -> &'static str {
        match self {
            Self::GroupItems(_) => "clipboard",
            Self::Groups => "groups",
        }
    }

    /// 限定在这一组里的条件及其参数
    fn filter(self) -> (&'static str, Vec<i64>) {
        match self {
            Self::GroupItems(group_id) => ("group_id = ?", vec![group_id]),
            Self::Groups => ("1", Vec::new()),
        }
    }
}

/// SQLite 数据库管理
pub struct Database {
    conn: Connection,
    db_path: String,
}

impl Database {
    /// 创建或打开数据库
    pub fn new(db_path: &str) -> Result<Self, String> {
        let conn = Connection::open(db_path)
            .map_err(|e| format!("打开数据库失败: {}", e))?;

        conn.execute(
            "CREATE TABLE IF NOT EXISTS clipboard (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                title TEXT,
                content TEXT NOT NULL,
                html_content TEXT,
                content_type TEXT NOT NULL DEFAULT 'text',
                image_id TEXT,
                thumbnail TEXT,
                item_order INTEGER NOT NULL DEFAULT 0,
                is_pinned INTEGER NOT NULL DEFAULT 0,
                paste_count INTEGER NOT NULL DEFAULT 0,
                source_app TEXT,
                char_count INTEGER,
                group_id INTEGER,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            )",
            [],
        ).map_err(|e| format!("创建表失败: {}", e))?;
        add_column_if_missing(&conn, "clipboard", "title", "TEXT")?;

        // 一次复制的每个剪贴板格式存一行原始数据，event_id 就是 clipboard.id
        conn.execute(
            "CREATE TABLE IF NOT EXISTS clipboard_formats (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id    INTEGER NOT NULL,   -- 关联 clipboard.id
                format_id   INTEGER NOT NULL,   -- Windows CF_* 数字 ID
                format_name TEXT    NOT NULL,   -- 格式名称（CF_UNICODETEXT / PNG / ...）
                data        BLOB,               -- 原始内存数据（允许 NULL / 空，兼容延迟渲染格式）
                compressed  INTEGER NOT NULL DEFAULT 0,  -- 1=zstd压缩，0=原始
                FOREIGN KEY (event_id) REFERENCES clipboard(id) ON DELETE CASCADE
            )",
            [],
        ).map_err(|e| format!("创建 clipboard_formats 表失败: {}", e))?;
        add_column_if_missing(&conn, "clipboard_formats", "compressed", "INTEGER NOT NULL DEFAULT 0")?;

        conn.execute(
            "CREATE TABLE IF NOT EXISTS groups (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT NOT NULL UNIQUE,
                color TEXT,
                icon TEXT,
                item_order INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL
            )",
            [],
        ).map_err(|e| format!("创建分组表失败: {}", e))?;
        add_column_if_missing(&conn, "groups", "group_type", "INTEGER NOT NULL DEFAULT 0")?;

        conn.execute_batch(
            "CREATE INDEX IF NOT EXISTS idx_formats_event ON clipboard_formats(event_id);
             CREATE INDEX IF NOT EXISTS idx_clipboard_order ON clipboard(is_pinned DESC, item_order DESC);
             CREATE INDEX IF NOT EXISTS idx_clipboard_content ON clipboard(content);
             CREATE INDEX IF NOT EXISTS idx_clipboard_group ON clipboard(group_id);
             CREATE INDEX IF NOT EXISTS idx_clipboard_image_id ON clipboard(image_id);"
        ).map_err(|e| format!("创建索引失败: {}", e))?;
        // 同一次复制的同一格式只存一条，INSERT OR IGNORE 靠它去重。
        // 有这个索引之前的旧库里可能已有重复行，建不起来时照常使用
        let _ = conn.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS ux_formats_event_format ON clipboard_formats(event_id, format_id)",
            [],
        );

        // 外键必须开，否则 ON DELETE CASCADE 不生效
        conn.execute_batch(
            "PRAGMA foreign_keys = ON;
             PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             PRAGMA cache_size = 10000;"
        ).map_err(|e| format!("设置参数失败: {}", e))?;

        Ok(Self {
            conn,
            db_path: db_path.to_string(),
        })
    }

    /// 获取图片存储目录
    pub fn get_images_dir(&self) -> PathBuf {
        let db_dir = std::path::Path::new(&self.db_path).parent()
            .unwrap_or_else(|| std::path::Path::new("."));
        let images_dir = db_dir.join("images");
        let _ = std::fs::create_dir_all(&images_dir);
        images_dir
    }
    
    /// 插入新记录；和已有记录内容相同时只把那条移到最前，返回它的 ID
    pub fn insert_item(&self, item: &PyClipboardItem) -> Result<i64, String> {
        let now = now();
        if let Some(id) = self.find_duplicate(item)? {
            self.conn.execute(
                &format!("UPDATE clipboard SET updated_at = ?1, item_order = {TOP_ORDER} WHERE id = ?2"),
                params![now, id],
            ).map_err(|e| format!("更新失败: {}", e))?;
            return Ok(id);
        }

        self.conn.execute(
            &format!(
                "INSERT INTO clipboard (title, content, html_content, content_type, image_id, thumbnail, item_order,
                 is_pinned, paste_count, source_app, char_count, created_at, updated_at)
                 VALUES (?1, ?2, ?3, ?4, ?5, ?6, {TOP_ORDER}, ?7, ?8, ?9, ?10, ?11, ?11)"
            ),
            params![
                &item.title,
                &item.content,
                &item.html_content,
                &item.content_type,
                &item.image_id,
                &item.thumbnail,
                item.is_pinned,
                item.paste_count,
                &item.source_app,
                item.content.chars().count() as i64,
                now,
            ],
        ).map_err(|e| format!("插入失败: {}", e))?;
        Ok(self.conn.last_insert_rowid())
    }

    /// 内容相同的已有记录。有标题的是手动收藏的内容，允许重复；
    /// 图片按 image_id 比，文本、文件按内容和 HTML 比
    fn find_duplicate(&self, item: &PyClipboardItem) -> Result<Option<i64>, String> {
        if item.title.is_some() {
            return Ok(None);
        }
        let found = if item.content_type == "image" && item.image_id.is_some() {
            self.conn.query_row(
                "SELECT id FROM clipboard WHERE image_id = ?1 AND content_type = 'image' ORDER BY created_at DESC LIMIT 1",
                params![&item.image_id],
                |row| row.get(0),
            )
        } else {
            self.conn.query_row(
                "SELECT id FROM clipboard WHERE content = ?1 AND content_type = ?2 AND html_content IS ?3 AND title IS NULL
                 ORDER BY created_at DESC LIMIT 1",
                params![&item.content, &item.content_type, &item.html_content],
                |row| row.get(0),
            )
        };
        found.optional().map_err(|e| format!("查重失败: {}", e))
    }

    /// 分页查询
    pub fn query_items(
        &self,
        offset: i64,
        limit: i64,
        search: Option<String>,
        content_type: Option<String>,
        start_time: Option<i64>,
        end_time: Option<i64>,
    ) -> Result<PyPaginatedResult, String> {
        let mut where_clauses = vec![];
        let mut params_vec: Vec<Value> = vec![];

        if let Some(ref s) = search {
            if !s.trim().is_empty() {
                // 关键词按字面匹配：% 和 _ 在 LIKE 里是通配符，要转义
                let escaped = s.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
                where_clauses.push("content LIKE ? ESCAPE '\\'".to_string());
                params_vec.push(Value::from(format!("%{}%", escaped)));
            }
        }

        if let Some(ref ct) = content_type {
            if ct != "all" {
                where_clauses.push("content_type = ?".to_string());
                params_vec.push(Value::from(ct.clone()));
            }
        }

        if let Some(start) = start_time {
            where_clauses.push("created_at >= ?".to_string());
            params_vec.push(Value::from(start));
        }

        if let Some(end) = end_time {
            where_clauses.push("created_at < ?".to_string());
            params_vec.push(Value::from(end));
        }

        let where_clause = if where_clauses.is_empty() {
            String::new()
        } else {
            format!("WHERE {}", where_clauses.join(" AND "))
        };

        let total_count: i64 = self.conn
            .query_row(&format!("SELECT COUNT(*) FROM clipboard {where_clause}"), params_from_iter(params_vec.iter()), |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))?;

        let mut stmt = self.conn.prepare(&format!(
            "SELECT {ITEM_COLUMNS} FROM clipboard {where_clause} ORDER BY is_pinned DESC, item_order DESC LIMIT ? OFFSET ?"
        )).map_err(|e| format!("准备查询失败: {}", e))?;
        params_vec.push(Value::from(limit));
        params_vec.push(Value::from(offset));
        let items = stmt.query_map(params_from_iter(params_vec.iter()), item_from_row)
            .and_then(|rows| rows.collect::<rusqlite::Result<Vec<_>>>())
            .map_err(|e| format!("查询失败: {}", e))?;

        Ok(PyPaginatedResult::new(total_count, items, offset, limit))
    }

    /// 获取总记录数
    pub fn get_count(&self) -> Result<i64, String> {
        self.conn.query_row("SELECT COUNT(*) FROM clipboard", [], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))
    }

    /// 根据 ID 获取记录
    pub fn get_item_by_id(&self, id: i64) -> Result<Option<PyClipboardItem>, String> {
        self.conn.query_row(&format!("SELECT {ITEM_COLUMNS} FROM clipboard WHERE id = ?"), params![id], item_from_row)
            .optional()
            .map_err(|e| format!("查询失败: {}", e))
    }

    /// 删除记录及其图片文件
    pub fn delete_item(&self, id: i64) -> Result<(), String> {
        let image_ids = self.image_ids("SELECT image_id FROM clipboard WHERE id = ?", params![id])?;
        self.conn.execute("DELETE FROM clipboard WHERE id = ?", params![id])
            .map_err(|e| format!("删除失败: {}", e))?;
        self.remove_image_files(image_ids);
        Ok(())
    }

    /// 清空记录
    ///
    /// Args:
    ///     keep_grouped: true = 保留已加入分组的条目（只删历史区），false = 删除全部
    pub fn clear_all(&self, keep_grouped: bool) -> Result<(), String> {
        self.clear_where(if keep_grouped { Some("group_id IS NULL") } else { None }, true)
    }

    /// 清理没有置顶、也没有加入分组的条目；置顶和分组里的内容都保留
    ///
    /// Args:
    ///     vacuum: true = 删完整理数据库文件、立即回收磁盘空间；
    ///             false = 只删行，空间留在库里供后续写入复用。
    ///             VACUUM 的耗时跟留下的数据量成正比，只为让条目不再显示时传 false
    pub fn clear_unpinned_ungrouped(&self, vacuum: bool) -> Result<(), String> {
        self.clear_where(Some(UNPROTECTED), vacuum)
    }

    /// 删除满足条件的记录及其图片文件，None 表示全部；vacuum 为 true 时再回收空间。
    /// condition 直接拼进 SQL，只能传本文件里的常量。
    fn clear_where(&self, condition: Option<&str>, vacuum: bool) -> Result<(), String> {
        let filter = condition.map(|cond| format!("WHERE {cond}")).unwrap_or_default();
        let image_ids = self.image_ids(&format!("SELECT image_id FROM clipboard {filter}"), [])?;
        // ON DELETE CASCADE 连带删掉 clipboard_formats
        self.conn.execute(&format!("DELETE FROM clipboard {filter}"), [])
            .map_err(|e| format!("清空失败: {}", e))?;
        self.remove_image_files(image_ids);

        if vacuum {
            // VACUUM 整理出的新内容先写进 WAL，要等 checkpoint 合并回主库，库文件才变小
            self.conn.execute_batch("VACUUM;")
                .map_err(|e| format!("VACUUM 失败: {}", e))?;
            let _ = self.conn.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);");
        }

        Ok(())
    }

    /// 这条查询选出的 image_id，去掉空值
    fn image_ids(&self, sql: &str, args: impl rusqlite::Params) -> Result<Vec<String>, String> {
        let mut stmt = self.conn.prepare(sql)
            .map_err(|e| format!("准备查询失败: {}", e))?;
        let ids = stmt.query_map(args, |row| row.get::<_, Option<String>>(0))
            .and_then(|rows| rows.collect::<rusqlite::Result<Vec<_>>>())
            .map_err(|e| format!("查询失败: {}", e))?;
        Ok(ids.into_iter().flatten().filter(|id| !id.is_empty()).collect())
    }

    /// 删掉图片文件。只在记录删除成功之后调用，否则删记录失败时会留下图片已经没了的记录。
    /// 文件已经不在时不算错
    fn remove_image_files(&self, image_ids: Vec<String>) {
        let images_dir = self.get_images_dir();
        for image_id in image_ids {
            let _ = std::fs::remove_file(image_file(&images_dir, &image_id));
        }
    }

    /// 切换置顶状态
    pub fn toggle_pin(&self, id: i64) -> Result<bool, String> {
        let current: i64 = self.conn.query_row(
            "SELECT is_pinned FROM clipboard WHERE id = ?",
            params![id],
            |row| row.get(0)
        ).map_err(|e| format!("查询失败: {}", e))?;

        let new_state = if current == 0 { 1 } else { 0 };

        self.conn.execute(
            "UPDATE clipboard SET is_pinned = ?, updated_at = ? WHERE id = ?",
            params![new_state, now(), id]
        ).map_err(|e| format!("更新失败: {}", e))?;

        Ok(new_state == 1)
    }

    // ==================== 分组功能 ====================

    /// 创建分组，排在最后
    pub fn create_group(&self, name: &str, color: Option<&str>, icon: Option<&str>, group_type: i64) -> Result<i64, String> {
        Self::check_group_type(group_type)?;
        self.conn.execute(
            "INSERT INTO groups (name, color, icon, item_order, created_at, group_type)
             VALUES (?1, ?2, ?3, (SELECT COALESCE(MAX(item_order), 0) + 1000 FROM groups), ?4, ?5)",
            params![name, color, icon, now(), group_type],
        ).map_err(|e| format!("创建分组失败: {}", e))?;

        Ok(self.conn.last_insert_rowid())
    }

    /// 获取所有分组
    pub fn get_groups(&self) -> Result<Vec<PyGroup>, String> {
        let mut stmt = self.conn.prepare(
            "SELECT id, name, color, icon, item_order, created_at, group_type FROM groups ORDER BY item_order ASC"
        ).map_err(|e| format!("查询分组失败: {}", e))?;

        stmt.query_map([], |row| {
            Ok(PyGroup {
                id: row.get(0)?,
                name: row.get(1)?,
                color: row.get(2)?,
                icon: row.get(3)?,
                item_order: row.get(4)?,
                created_at: row.get(5)?,
                group_type: row.get(6)?,
            })
        })
        .and_then(|rows| rows.collect::<rusqlite::Result<Vec<_>>>())
        .map_err(|e| format!("查询分组失败: {}", e))
    }

    /// 删除分组，里面的内容回到历史区
    pub fn delete_group(&self, id: i64) -> Result<(), String> {
        let tx = self.conn.unchecked_transaction()
            .map_err(|e| format!("删除分组失败: {}", e))?;
        tx.execute(
            "UPDATE clipboard SET group_id = NULL WHERE group_id = ?",
            params![id],
        ).map_err(|e| format!("更新项目失败: {}", e))?;
        tx.execute("DELETE FROM groups WHERE id = ?", params![id])
            .map_err(|e| format!("删除分组失败: {}", e))?;
        tx.commit().map_err(|e| format!("删除分组失败: {}", e))
    }

    /// 重命名分组
    pub fn rename_group(&self, id: i64, name: &str) -> Result<(), String> {
        self.conn.execute(
            "UPDATE groups SET name = ? WHERE id = ?",
            params![name, id],
        ).map_err(|e| format!("重命名分组失败: {}", e))?;
        Ok(())
    }

    fn check_group_type(group_type: i64) -> Result<(), String> {
        if group_kind::is_known(group_type) {
            Ok(())
        } else {
            Err(format!("未知的分组类型: {group_type}"))
        }
    }

    /// 更新分组（名称、颜色、图标、类型）
    ///
    /// 筛选分组不存内容，所以里面还有内容的分组不能改成筛选分组；
    /// 内容自动移出的话会失去分组的清理保护，只能让用户先处理
    pub fn update_group(&self, id: i64, name: &str, color: Option<&str>, icon: Option<&str>, group_type: i64) -> Result<(), String> {
        Self::check_group_type(group_type)?;
        if group_kind::filter_content_type(group_type).is_some() {
            let members: i64 = self.conn
                .query_row("SELECT COUNT(*) FROM clipboard WHERE group_id = ?", params![id], |row| row.get(0))
                .map_err(|e| format!("更新分组失败: {}", e))?;
            if members > 0 {
                return Err("分组里还有内容，不能改成筛选分组".to_string());
            }
        }
        self.conn.execute(
            "UPDATE groups SET name = ?, color = ?, icon = ?, group_type = ? WHERE id = ?",
            params![name, color, icon, group_type, id],
        ).map_err(|e| format!("更新分组失败: {}", e))?;
        Ok(())
    }

    /// 将项目移动到分组；目标分组不接受这类内容时报错
    pub fn move_to_group(&self, item_id: i64, group_id: Option<i64>) -> Result<(), String> {
        if let Some(group_id) = group_id {
            let group_type: Option<i64> = self.conn
                .query_row("SELECT group_type FROM groups WHERE id = ?", params![group_id], |row| row.get(0))
                .optional()
                .map_err(|e| format!("移动到分组失败: {}", e))?;
            let Some(group_type) = group_type else {
                return Err("分组不存在".to_string());
            };
            let content_type: Option<String> = self.conn
                .query_row("SELECT content_type FROM clipboard WHERE id = ?", params![item_id], |row| row.get(0))
                .optional()
                .map_err(|e| format!("移动到分组失败: {}", e))?;
            if content_type.is_some_and(|content_type| !group_kind::accepts(group_type, &content_type)) {
                return Err("该分组不接受这类内容".to_string());
            }
        }
        self.conn.execute(
            "UPDATE clipboard SET group_id = ?, updated_at = ? WHERE id = ?",
            params![group_id, now(), item_id],
        ).map_err(|e| format!("移动到分组失败: {}", e))?;
        Ok(())
    }

    /// 按分组查询，group_id 为 None 时查未分组的；分组内按 item_order 升序（新内容在下）
    pub fn query_by_group(&self, group_id: Option<i64>, offset: i64, limit: i64) -> Result<PyPaginatedResult, String> {
        // IS 对 NULL 也成立，有分组、未分组走同一条语句
        let total_count: i64 = self.conn.query_row(
            "SELECT COUNT(*) FROM clipboard WHERE group_id IS ?1",
            params![group_id],
            |row| row.get(0),
        ).map_err(|e| format!("查询失败: {}", e))?;

        let mut stmt = self.conn.prepare(&format!(
            "SELECT {ITEM_COLUMNS} FROM clipboard WHERE group_id IS ?1 ORDER BY is_pinned DESC, item_order ASC LIMIT ?2 OFFSET ?3"
        )).map_err(|e| format!("准备查询失败: {}", e))?;
        let items = stmt.query_map(params![group_id, limit, offset], item_from_row)
            .and_then(|rows| rows.collect::<rusqlite::Result<Vec<_>>>())
            .map_err(|e| format!("查询失败: {}", e))?;

        Ok(PyPaginatedResult::new(total_count, items, offset, limit))
    }

    /// 增加粘贴次数，返回新的次数；记录不存在时返回 0
    pub fn increment_paste_count(&self, id: i64) -> Result<i64, String> {
        self.conn.query_row(
            "UPDATE clipboard SET paste_count = paste_count + 1, updated_at = ?1 WHERE id = ?2 RETURNING paste_count",
            params![now(), id],
            |row| row.get(0),
        )
        .optional()
        .map(|count| count.unwrap_or(0))
        .map_err(|e| format!("更新粘贴次数失败: {}", e))
    }

    /// 将某项移到最前
    pub fn move_item_to_top(&self, id: i64) -> Result<(), String> {
        self.conn.execute(
            &format!("UPDATE clipboard SET item_order = {TOP_ORDER}, updated_at = ? WHERE id = ?"),
            params![now(), id],
        ).map_err(|e| format!("移动到最前失败: {}", e))?;
        Ok(())
    }

    /// 把几条记录一起移到最前，彼此的先后不变
    pub fn move_items_to_top(&self, ids: &[i64]) -> Result<(), String> {
        let tx = self.conn.unchecked_transaction()
            .map_err(|e| format!("移动到最前失败: {}", e))?;
        let mut ordered: Vec<(i64, i64)> = Vec::with_capacity(ids.len());
        for id in ids {
            match tx.query_row("SELECT item_order FROM clipboard WHERE id = ?", params![id], |row| row.get::<_, i64>(0)) {
                Ok(order) => ordered.push((order, *id)),
                Err(rusqlite::Error::QueryReturnedNoRows) => {}
                Err(e) => return Err(format!("移动到最前失败: {}", e)),
            }
        }
        // 原来靠下的先挪，最后挪的排在最上面
        ordered.sort();
        let top: i64 = tx.query_row("SELECT COALESCE(MAX(item_order), 0) FROM clipboard", [], |row| row.get(0))
            .map_err(|e| format!("移动到最前失败: {}", e))?;
        let now = now();
        for (index, (_, id)) in ordered.iter().enumerate() {
            tx.execute(
                "UPDATE clipboard SET item_order = ?, updated_at = ? WHERE id = ?",
                params![top + 1000 * (index as i64 + 1), now, id],
            ).map_err(|e| format!("移动到最前失败: {}", e))?;
        }
        tx.commit().map_err(|e| format!("移动到最前失败: {}", e))
    }

    /// 在分组内拖拽排序：放到 before_id（界面上方）和 after_id（界面下方）之间，
    /// None 表示移到最前或最后
    pub fn move_item_between(&self, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> Result<(), String> {
        // 只有分组内的内容按这套顺序排
        let group_id: Option<i64> = self.conn.query_row(
            "SELECT group_id FROM clipboard WHERE id = ?",
            params![id],
            |row| row.get(0),
        ).optional().map_err(|e| format!("移动失败: {}", e))?.flatten();
        let Some(group_id) = group_id else {
            return Err("只能在分组内移动内容".to_string());
        };
        self.move_between(OrderScope::GroupItems(group_id), id, before_id, after_id)
    }

    /// 拖拽排序分组，参数含义同 move_item_between
    pub fn move_group_between(&self, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> Result<(), String> {
        self.move_between(OrderScope::Groups, id, before_id, after_id)
    }

    /// 取上下两个邻居 item_order 的中间值。邻居为 None 或已不存在时贴着这一组的顶端或末端放；
    /// 间隔不够时整组重新编号再放一次
    fn move_between(&self, scope: OrderScope, id: i64, before_id: Option<i64>, after_id: Option<i64>) -> Result<(), String> {
        for attempt in 0..2 {
            let upper = match self.order_of(scope, before_id)? {
                Some(order) => order,
                None => self.edge_order(scope, "COALESCE(MIN(item_order), 1000) - 1000")?,
            };
            let lower = match self.order_of(scope, after_id)? {
                Some(order) => order,
                None => self.edge_order(scope, "COALESCE(MAX(item_order), 0) + 1000")?,
            };
            if lower.saturating_sub(upper) >= 10 {
                return self.set_order(scope, id, upper + (lower - upper) / 2);
            }
            if attempt == 0 {
                self.reindex(scope)?;
            }
        }
        Err("移动失败：上方那项排在下方那项后面".to_string())
    }

    fn order_of(&self, scope: OrderScope, id: Option<i64>) -> Result<Option<i64>, String> {
        let Some(id) = id else {
            return Ok(None);
        };
        self.conn.query_row(
            &format!("SELECT item_order FROM {} WHERE id = ?", scope.table()),
            params![id],
            |row| row.get(0),
        ).optional().map_err(|e| format!("读取排序失败: {}", e))
    }

    /// 这一组 item_order 的聚合值，expr 只能传本文件里的常量
    fn edge_order(&self, scope: OrderScope, expr: &str) -> Result<i64, String> {
        let (filter, args) = scope.filter();
        self.conn.query_row(
            &format!("SELECT {expr} FROM {} WHERE {filter}", scope.table()),
            params_from_iter(args),
            |row| row.get(0),
        ).map_err(|e| format!("读取排序失败: {}", e))
    }

    fn set_order(&self, scope: OrderScope, id: i64, order: i64) -> Result<(), String> {
        match scope {
            OrderScope::GroupItems(_) => self.conn.execute(
                "UPDATE clipboard SET item_order = ?, updated_at = ? WHERE id = ?",
                params![order, now(), id],
            ),
            OrderScope::Groups => self.conn.execute(
                "UPDATE groups SET item_order = ? WHERE id = ?",
                params![order, id],
            ),
        }.map(|_| ()).map_err(|e| format!("移动失败: {}", e))
    }

    /// 按当前顺序重新编号为 1000, 2000, 3000, ...
    fn reindex(&self, scope: OrderScope) -> Result<(), String> {
        let (filter, args) = scope.filter();
        let tx = self.conn.unchecked_transaction()
            .map_err(|e| format!("重新编号失败: {}", e))?;
        let ids: Vec<i64> = {
            let mut stmt = tx.prepare(&format!("SELECT id FROM {} WHERE {filter} ORDER BY item_order ASC", scope.table()))
                .map_err(|e| format!("重新编号失败: {}", e))?;
            stmt.query_map(params_from_iter(args), |row| row.get(0))
                .and_then(|rows| rows.collect::<rusqlite::Result<Vec<_>>>())
                .map_err(|e| format!("重新编号失败: {}", e))?
        };
        let update = format!("UPDATE {} SET item_order = ? WHERE id = ?", scope.table());
        for (index, id) in ids.iter().enumerate() {
            tx.execute(&update, params![(index as i64 + 1) * 1000, id])
                .map_err(|e| format!("重新编号失败: {}", e))?;
        }
        tx.commit().map_err(|e| format!("重新编号失败: {}", e))
    }

    /// 更新内容项（标题和内容）
    ///
    /// 内容变化时一并清掉 html_content 和原始格式：粘贴优先按原始格式还原，
    /// 留着会粘出编辑前的内容。只改标题时保留，避免无谓丢失富文本。
    pub fn update_item(&self, id: i64, title: Option<&str>, content: &str) -> Result<(), String> {
        let now = now();
        let content_changed = match self.conn.query_row(
            "SELECT content FROM clipboard WHERE id = ?",
            params![id],
            |row| row.get::<_, String>(0),
        ) {
            Ok(old) => old != content,
            Err(rusqlite::Error::QueryReturnedNoRows) => return Ok(()),
            Err(e) => return Err(format!("更新内容失败: {}", e)),
        };

        if !content_changed {
            self.conn.execute(
                "UPDATE clipboard SET title = ?, updated_at = ? WHERE id = ?",
                params![title, now, id],
            ).map_err(|e| format!("更新内容失败: {}", e))?;
            return Ok(());
        }

        let tx = self.conn.unchecked_transaction()
            .map_err(|e| format!("更新内容失败: {}", e))?;
        tx.execute(
            "UPDATE clipboard SET title = ?, content = ?, html_content = NULL, char_count = ?, updated_at = ? WHERE id = ?",
            params![title, content, content.chars().count() as i64, now, id],
        ).map_err(|e| format!("更新内容失败: {}", e))?;
        tx.execute(
            "DELETE FROM clipboard_formats WHERE event_id = ?",
            params![id],
        ).map_err(|e| format!("删除 formats 失败: {}", e))?;
        tx.commit().map_err(|e| format!("更新内容失败: {}", e))?;
        Ok(())
    }

    // ==================== 原始格式存取 ====================

    /// 保存一批原始剪贴板格式数据，关联到 event_id（即 clipboard.id）；超过阈值的在这里压缩
    pub fn insert_formats(&self, event_id: i64, formats: &[(u32, String, Vec<u8>)]) -> Result<(), String> {
        for (format_id, format_name, data) in formats {
            let (data, compressed) = compress_format(data.clone());
            self.conn.execute(
                "INSERT OR IGNORE INTO clipboard_formats (event_id, format_id, format_name, data, compressed)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![event_id, *format_id as i64, format_name, data, compressed],
            ).map_err(|e| format!("插入 format 失败: {}", e))?;
        }
        Ok(())
    }

    /// 用监听线程抓到的格式整组替换这条记录原有的格式（数据已按需压缩）
    ///
    /// 去重会把同样内容的复制并到旧记录上，而同样的文字在不同来源里可能带着
    /// 不同的富文本或公式，粘贴应还原最近这次复制，所以旧格式不能留。
    /// formats: (format_id, format_name, data, is_compressed)
    pub fn replace_formats(
        &self,
        event_id: i64,
        formats: &[(u32, String, Vec<u8>, bool)],
    ) -> Result<(), String> {
        let tx = self.conn.unchecked_transaction()
            .map_err(|e| format!("替换 formats 失败: {}", e))?;
        tx.execute(
            "DELETE FROM clipboard_formats WHERE event_id = ?",
            params![event_id],
        ).map_err(|e| format!("删除 formats 失败: {}", e))?;
        for (format_id, format_name, data, is_compressed) in formats {
            tx.execute(
                "INSERT OR IGNORE INTO clipboard_formats (event_id, format_id, format_name, data, compressed)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![event_id, *format_id as i64, format_name, data, is_compressed],
            ).map_err(|e| format!("插入 format 失败: {}", e))?;
        }
        tx.commit().map_err(|e| format!("替换 formats 失败: {}", e))
    }

    /// 读取某个 event 的所有原始格式数据，压缩过的解压后返回；数据为 NULL 的读成空。
    /// 解压失败时报错，不把压缩数据当原文返回
    /// 返回 Vec<(format_id, format_name, data)>
    pub fn get_formats(&self, event_id: i64) -> Result<Vec<(u32, String, Vec<u8>)>, String> {
        let mut stmt = self.conn.prepare(
            "SELECT format_id, format_name, data, compressed FROM clipboard_formats WHERE event_id = ? ORDER BY format_id ASC"
        ).map_err(|e| format!("准备查询 formats 失败: {}", e))?;

        let rows = stmt.query_map(params![event_id], |row| {
            Ok((
                row.get::<_, i64>(0)? as u32,
                row.get::<_, String>(1)?,
                row.get::<_, Option<Vec<u8>>>(2)?.unwrap_or_default(),
                row.get::<_, bool>(3)?,
            ))
        })
        .and_then(|rows| rows.collect::<rusqlite::Result<Vec<_>>>())
        .map_err(|e| format!("查询 formats 失败: {}", e))?;

        rows.into_iter()
            .map(|(format_id, format_name, data, compressed)| {
                if !compressed {
                    return Ok((format_id, format_name, data));
                }
                match zstd::decode_all(data.as_slice()) {
                    Ok(decoded) => Ok((format_id, format_name, decoded)),
                    Err(e) => Err(format!("解压 {} 失败: {}", format_name, e)),
                }
            })
            .collect()
    }

    /// 清理超出限制的旧记录
    ///
    /// 保留置顶项和分组内容，只删除非置顶、非分组的旧记录
    ///
    /// Args:
    ///     limit: 保留的最大记录数
    ///
    /// Returns:
    ///     删除的记录数
    pub fn cleanup_old_items(&self, limit: i64) -> Result<i64, String> {
        if limit <= 0 {
            return Ok(0);
        }

        // 只数历史区（未分组）的记录
        let total: i64 = self.conn.query_row(
            "SELECT COUNT(*) FROM clipboard WHERE group_id IS NULL",
            [],
            |row| row.get(0)
        ).map_err(|e| format!("查询失败: {}", e))?;
        if total <= limit {
            return Ok(0);
        }

        // 按 item_order 升序取最旧的；查图片和删除用同一个条件，只删这些记录的图片
        let oldest = format!("SELECT id FROM clipboard WHERE {UNPROTECTED} ORDER BY item_order ASC LIMIT ?1");
        let to_delete = total - limit;
        let image_ids = self.image_ids(&format!("SELECT image_id FROM clipboard WHERE id IN ({oldest})"), params![to_delete])?;
        let deleted = self.conn.execute(&format!("DELETE FROM clipboard WHERE id IN ({oldest})"), params![to_delete])
            .map_err(|e| format!("清理失败: {}", e))?;
        self.remove_image_files(image_ids);

        Ok(deleted as i64)
    }
}

#[cfg(test)]
mod update_item_tests {
    use super::Database;
    use crate::types::PyClipboardItem;

    fn db_with_captured_text(text: &str) -> (Database, i64) {
        let db = Database::new(":memory:").unwrap();
        let mut item = PyClipboardItem::new(0, text.to_string(), "text".to_string());
        item.html_content = Some(format!("<b>{}</b>", text));
        let id = db.insert_item(&item).unwrap();
        let raw: Vec<u8> = format!("{}\0", text).encode_utf16().flat_map(u16::to_le_bytes).collect();
        db.insert_formats(id, &[(13, "CF_UNICODETEXT".to_string(), raw)]).unwrap();
        (db, id)
    }

    #[test]
    fn editing_content_drops_stale_formats_and_html() {
        let (db, id) = db_with_captured_text("旧内容");
        db.update_item(id, None, "新内容").unwrap();

        let item = db.get_item_by_id(id).unwrap().unwrap();
        assert_eq!(item.content, "新内容");
        assert_eq!(item.html_content, None);
        assert_eq!(item.char_count, Some(3));
        assert!(db.get_formats(id).unwrap().is_empty());
    }

    #[test]
    fn editing_only_title_keeps_rich_formats() {
        let (db, id) = db_with_captured_text("内容");
        db.update_item(id, Some("标题"), "内容").unwrap();

        let item = db.get_item_by_id(id).unwrap().unwrap();
        assert_eq!(item.title.as_deref(), Some("标题"));
        assert_eq!(item.html_content.as_deref(), Some("<b>内容</b>"));
        assert_eq!(db.get_formats(id).unwrap().len(), 1);
    }

    #[test]
    fn updating_missing_item_is_a_no_op() {
        let db = Database::new(":memory:").unwrap();
        assert!(db.update_item(42, None, "x").is_ok());
    }

    #[test]
    fn a_repeated_copy_replaces_the_stored_formats() {
        let (db, id) = db_with_captured_text("20");
        let formula = b"<Cell ss:Formula=\"=A1*2\"/>".to_vec();
        db.replace_formats(id, &[(50042, "XML Spreadsheet".to_string(), formula, false)]).unwrap();
        assert_eq!(db.get_formats(id).unwrap().len(), 1);

        // 同样的文字再复制一次（去重落到同一条记录），这次没有公式
        let text: Vec<u8> = "20\0".encode_utf16().flat_map(u16::to_le_bytes).collect();
        db.replace_formats(id, &[(13, "CF_UNICODETEXT".to_string(), text, false)]).unwrap();
        let names: Vec<String> = db.get_formats(id).unwrap().into_iter().map(|(_, n, _)| n).collect();
        assert_eq!(names, vec!["CF_UNICODETEXT".to_string()]);
    }
}

#[cfg(test)]
mod maintenance_tests {
    use super::{Database, OrderScope};
    use crate::group_kind;
    use crate::types::PyClipboardItem;

    fn add(db: &Database, text: &str) -> i64 {
        db.insert_item(&PyClipboardItem::new(0, text.to_string(), "text".to_string())).unwrap()
    }

    fn search(db: &Database, keyword: &str) -> Vec<String> {
        let mut found: Vec<String> = db.query_items(0, 50, Some(keyword.to_string()), None, None, None)
            .unwrap().items.into_iter().map(|item| item.content).collect();
        found.sort();
        found
    }

    #[test]
    fn search_matches_percent_and_underscore_literally() {
        let db = Database::new(":memory:").unwrap();
        for text in ["50%", "5000", "a_b", "axb", r"C:\temp"] {
            add(&db, text);
        }
        assert_eq!(search(&db, "50%"), vec!["50%"]);
        assert_eq!(search(&db, "a_b"), vec!["a_b"]);
        assert_eq!(search(&db, r"\t"), vec![r"C:\temp"]);
    }

    #[test]
    fn items_moved_to_the_top_together_keep_their_order() {
        let db = Database::new(":memory:").unwrap();
        let ids: Vec<i64> = ["一", "二", "三", "四"].iter().map(|text| add(&db, text)).collect();
        // 列表从新到旧：四 三 二 一；把「一」和「三」挪上去，传入顺序不影响结果
        db.move_items_to_top(&[ids[0], ids[2], 999]).unwrap();
        let order: Vec<String> = db.query_items(0, 10, None, None, None, None)
            .unwrap().items.into_iter().map(|item| item.content).collect();
        assert_eq!(order, vec!["三", "一", "四", "二"]);
    }

    #[test]
    fn moving_an_ungrouped_item_is_an_error_not_a_panic() {
        let db = Database::new(":memory:").unwrap();
        let first = add(&db, "一");
        let second = add(&db, "二");
        assert!(db.move_item_between(second, None, Some(first)).is_err());
    }

    fn clear_keeping_pinned_and_grouped(vacuum: bool) {
        let dir = std::env::temp_dir().join(format!("pyclipboard_clear_unpinned_{}_{}", std::process::id(), vacuum));
        std::fs::create_dir_all(&dir).unwrap();
        {
            let db = Database::new(dir.join("clipboard.db").to_str().unwrap()).unwrap();
            let group = db.create_group("收藏", None, None, 0).unwrap();
            let image_file = |id: &str| db.get_images_dir().join(format!("{}.png", id));
            let with_image = |text: &str, image_id: &str| {
                let mut item = PyClipboardItem::new(0, text.to_string(), "image".to_string());
                item.image_id = Some(image_id.to_string());
                std::fs::write(image_file(image_id), b"png").unwrap();
                db.insert_item(&item).unwrap()
            };

            let plain = with_image("普通", "plain");
            let pinned = with_image("置顶", "pinned");
            let grouped = with_image("分组", "grouped");
            let both = with_image("置顶又分组", "both");
            db.toggle_pin(pinned).unwrap();
            db.toggle_pin(both).unwrap();
            db.move_to_group(grouped, Some(group)).unwrap();
            db.move_to_group(both, Some(group)).unwrap();

            db.clear_unpinned_ungrouped(vacuum).unwrap();

            assert!(db.get_item_by_id(plain).unwrap().is_none());
            for kept in [pinned, grouped, both] {
                assert!(db.get_item_by_id(kept).unwrap().is_some());
            }
            assert!(!image_file("plain").exists());
            for kept in ["pinned", "grouped", "both"] {
                assert!(image_file(kept).exists());
            }
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn clearing_unpinned_ungrouped_keeps_pinned_and_grouped_items() {
        clear_keeping_pinned_and_grouped(true);
        clear_keeping_pinned_and_grouped(false);
    }

    #[test]
    fn clearing_without_vacuum_deletes_rows_but_leaves_the_file_size() {
        let dir = std::env::temp_dir().join(format!("pyclipboard_clear_novacuum_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("clipboard.db");
        {
            let db = Database::new(path.to_str().unwrap()).unwrap();
            let blob: Vec<u8> = (0..4_000_000u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 24) as u8).collect();
            let id = add(&db, "大图");
            db.replace_formats(id, &[(8, "CF_DIB".to_string(), blob, false)]).unwrap();
            let _ = db.conn.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);");

            db.clear_unpinned_ungrouped(false).unwrap();
            let _ = db.conn.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);");
            assert!(db.get_item_by_id(id).unwrap().is_none());
            // 行已删，空间只是进了空闲页，库文件没缩
            assert!(std::fs::metadata(&path).unwrap().len() > 3_000_000);
            let free_pages: i64 = db.conn.query_row("PRAGMA freelist_count", [], |row| row.get(0)).unwrap();
            assert!(free_pages > 0);

            db.clear_unpinned_ungrouped(true).unwrap();
            assert!(std::fs::metadata(&path).unwrap().len() < 1_000_000);
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn clearing_history_gives_the_space_back_right_away() {
        let dir = std::env::temp_dir().join(format!("pyclipboard_clear_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("clipboard.db");
        {
            let db = Database::new(path.to_str().unwrap()).unwrap();
            let blob: Vec<u8> = (0..4_000_000u32).map(|i| (i.wrapping_mul(2_654_435_761) >> 24) as u8).collect();
            let id = add(&db, "大图");
            db.replace_formats(id, &[(8, "CF_DIB".to_string(), blob, false)]).unwrap();
            let _ = db.conn.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);");
            assert!(std::fs::metadata(&path).unwrap().len() > 3_000_000);

            db.clear_all(false).unwrap();
            assert!(std::fs::metadata(&path).unwrap().len() < 1_000_000);
        }
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn group_contents(db: &Database, group_id: Option<i64>) -> Vec<String> {
        db.query_by_group(group_id, 0, 50).unwrap().items.into_iter().map(|item| item.content).collect()
    }

    #[test]
    fn query_by_group_covers_grouped_and_ungrouped_items() {
        let db = Database::new(":memory:").unwrap();
        let group = db.create_group("收藏", None, None, 0).unwrap();
        let grouped = add(&db, "分组里的");
        db.move_to_group(grouped, Some(group)).unwrap();
        add(&db, "没分组的");
        assert_eq!(group_contents(&db, Some(group)), vec!["分组里的"]);
        assert_eq!(group_contents(&db, None), vec!["没分组的"]);
        assert_eq!(db.query_by_group(None, 0, 50).unwrap().total_count, 1);
    }

    fn add_file(db: &Database, path: &str) -> i64 {
        db.insert_item(&PyClipboardItem::new(0, path.to_string(), "file".to_string())).unwrap()
    }

    #[test]
    fn a_quick_launch_group_only_takes_files() {
        let db = Database::new(":memory:").unwrap();
        let group = db.create_group("启动", None, None, group_kind::FILE).unwrap();
        let text = add(&db, "文本");
        let file = add_file(&db, "C:/a.txt");
        assert!(db.move_to_group(text, Some(group)).is_err());
        db.move_to_group(file, Some(group)).unwrap();
        assert_eq!(group_contents(&db, Some(group)), vec!["C:/a.txt"]);
        // 被拒绝的条目原样留着
        assert_eq!(group_contents(&db, None), vec!["文本"]);
    }

    #[test]
    fn a_filter_group_takes_nothing() {
        let db = Database::new(":memory:").unwrap();
        let group = db.create_group("只看图片", None, None, group_kind::FILTER_IMAGE).unwrap();
        assert!(db.move_to_group(add(&db, "文本"), Some(group)).is_err());
        assert!(db.move_to_group(add_file(&db, "C:/a.txt"), Some(group)).is_err());
        assert!(group_contents(&db, Some(group)).is_empty());
    }

    #[test]
    fn moving_into_a_missing_group_is_an_error_but_leaving_a_group_is_not() {
        let db = Database::new(":memory:").unwrap();
        let item = add(&db, "x");
        assert!(db.move_to_group(item, Some(999)).is_err());
        let group = db.create_group("收藏", None, None, group_kind::NORMAL).unwrap();
        db.move_to_group(item, Some(group)).unwrap();
        db.move_to_group(item, None).unwrap();
        assert_eq!(group_contents(&db, None), vec!["x"]);
    }

    #[test]
    fn a_group_with_content_cannot_become_a_filter_group() {
        let db = Database::new(":memory:").unwrap();
        let group = db.create_group("收藏", None, None, group_kind::NORMAL).unwrap();
        let item = add(&db, "x");
        db.move_to_group(item, Some(group)).unwrap();
        assert!(db.update_group(group, "收藏", None, None, group_kind::FILTER_TEXT).is_err());
        let kept = db.get_groups().unwrap().into_iter().find(|g| g.id == group).unwrap();
        assert_eq!(kept.group_type, group_kind::NORMAL);

        // 腾空后可以改；筛选分组改回别的类型不受限制
        db.move_to_group(item, None).unwrap();
        db.update_group(group, "只看文本", None, None, group_kind::FILTER_TEXT).unwrap();
        db.update_group(group, "只看图片", None, None, group_kind::FILTER_IMAGE).unwrap();
        db.update_group(group, "收藏", None, None, group_kind::FILE).unwrap();
    }

    #[test]
    fn a_group_with_text_can_still_become_quick_launch() {
        let db = Database::new(":memory:").unwrap();
        let group = db.create_group("收藏", None, None, group_kind::NORMAL).unwrap();
        db.move_to_group(add(&db, "x"), Some(group)).unwrap();
        db.update_group(group, "收藏", None, None, group_kind::FILE).unwrap();
    }

    #[test]
    fn unknown_group_types_are_refused() {
        let db = Database::new(":memory:").unwrap();
        assert!(db.create_group("坏", None, None, 99).is_err());
        let group = db.create_group("好", None, None, group_kind::NORMAL).unwrap();
        assert!(db.update_group(group, "好", None, None, -1).is_err());
    }

    #[test]
    fn a_missing_neighbour_puts_the_item_at_the_edge_instead_of_overflowing() {
        let db = Database::new(":memory:").unwrap();
        let group = db.create_group("收藏", None, None, 0).unwrap();
        let ids: Vec<i64> = ["一", "二", "三"].iter().map(|text| {
            let id = add(&db, text);
            db.move_to_group(id, Some(group)).unwrap();
            id
        }).collect();
        db.reindex(OrderScope::GroupItems(group)).unwrap();
        db.delete_item(ids[2]).unwrap();

        // 下方邻居已被删：放到末尾
        db.move_item_between(ids[0], Some(ids[1]), Some(ids[2])).unwrap();
        assert_eq!(group_contents(&db, Some(group)), vec!["二", "一"]);
        // 上方邻居已被删：放到最前
        db.move_item_between(ids[0], Some(ids[2]), Some(ids[1])).unwrap();
        assert_eq!(group_contents(&db, Some(group)), vec!["一", "二"]);
    }

    #[test]
    fn groups_are_renumbered_when_there_is_no_room_between_neighbours() {
        let db = Database::new(":memory:").unwrap();
        let ids: Vec<i64> = ["甲", "乙", "丙"].iter().map(|name| db.create_group(name, None, None, 0).unwrap()).collect();
        for (offset, id) in ids.iter().enumerate() {
            db.conn.execute("UPDATE groups SET item_order = ? WHERE id = ?", rusqlite::params![1000 + offset as i64, id]).unwrap();
        }
        db.move_group_between(ids[2], Some(ids[0]), Some(ids[1])).unwrap();
        let names: Vec<String> = db.get_groups().unwrap().into_iter().map(|group| group.name).collect();
        assert_eq!(names, vec!["甲", "丙", "乙"]);
        // 上下颠倒的邻居重新编号也放不下，报错而不是乱放
        assert!(db.move_group_between(ids[0], Some(ids[1]), Some(ids[2])).is_err());
    }

    #[test]
    fn old_databases_get_the_newer_columns() {
        let dir = std::env::temp_dir().join(format!("pyclipboard_migrate_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("clipboard.db");
        {
            let old = rusqlite::Connection::open(&path).unwrap();
            old.execute_batch(
                "CREATE TABLE clipboard (id INTEGER PRIMARY KEY AUTOINCREMENT, content TEXT NOT NULL,
                     html_content TEXT, content_type TEXT NOT NULL DEFAULT 'text', image_id TEXT, thumbnail TEXT,
                     item_order INTEGER NOT NULL DEFAULT 0, is_pinned INTEGER NOT NULL DEFAULT 0,
                     paste_count INTEGER NOT NULL DEFAULT 0, source_app TEXT, char_count INTEGER, group_id INTEGER,
                     created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL);
                 CREATE TABLE clipboard_formats (id INTEGER PRIMARY KEY AUTOINCREMENT, event_id INTEGER NOT NULL,
                     format_id INTEGER NOT NULL, format_name TEXT NOT NULL, data BLOB);
                 CREATE TABLE groups (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE, color TEXT,
                     icon TEXT, item_order INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL);
                 INSERT INTO clipboard (content, created_at, updated_at) VALUES ('旧内容', 1, 1);"
            ).unwrap();
        }
        {
            let db = Database::new(path.to_str().unwrap()).unwrap();
            let id = db.create_group("新分组", None, None, 1).unwrap();
            assert_eq!(db.get_groups().unwrap().into_iter().find(|g| g.id == id).unwrap().group_type, 1);
            assert_eq!(search(&db, "旧内容"), vec!["旧内容"]);
            db.update_item(1, Some("标题"), "旧内容").unwrap();
            assert_eq!(db.get_item_by_id(1).unwrap().unwrap().title.as_deref(), Some("标题"));
            db.insert_formats(1, &[(13, "CF_UNICODETEXT".to_string(), vec![1, 2])]).unwrap();
            assert_eq!(db.get_formats(1).unwrap().len(), 1);
        }
        // 再开一次：列都在了，不再改表
        assert!(Database::new(path.to_str().unwrap()).is_ok());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn corrupt_compressed_formats_are_an_error_and_null_data_reads_as_empty() {
        let db = Database::new(":memory:").unwrap();
        let id = add(&db, "x");
        db.conn.execute(
            "INSERT INTO clipboard_formats (event_id, format_id, format_name, data, compressed) VALUES (?1, 13, 'CF_UNICODETEXT', NULL, 0)",
            rusqlite::params![id],
        ).unwrap();
        assert_eq!(db.get_formats(id).unwrap(), vec![(13, "CF_UNICODETEXT".to_string(), Vec::new())]);

        db.conn.execute(
            "INSERT INTO clipboard_formats (event_id, format_id, format_name, data, compressed) VALUES (?1, 8, 'CF_DIB', X'0102030405', 1)",
            rusqlite::params![id],
        ).unwrap();
        assert!(db.get_formats(id).is_err());
    }

    #[test]
    fn large_formats_are_compressed_and_read_back_intact() {
        let db = Database::new(":memory:").unwrap();
        let id = add(&db, "大块");
        let data: Vec<u8> = (0..300_000u32).map(|i| (i % 7) as u8).collect();
        db.insert_formats(id, &[(8, "CF_DIB".to_string(), data.clone())]).unwrap();
        let stored: (i64, i64) = db.conn.query_row(
            "SELECT compressed, length(data) FROM clipboard_formats WHERE event_id = ?", rusqlite::params![id],
            |row| Ok((row.get(0)?, row.get(1)?)),
        ).unwrap();
        assert_eq!(stored.0, 1);
        assert!((stored.1 as usize) < data.len());
        assert_eq!(db.get_formats(id).unwrap()[0].2, data);
    }

    #[test]
    fn paste_count_goes_up_and_a_missing_item_counts_zero() {
        let db = Database::new(":memory:").unwrap();
        let id = add(&db, "x");
        assert_eq!(db.increment_paste_count(id).unwrap(), 1);
        assert_eq!(db.increment_paste_count(id).unwrap(), 2);
        assert_eq!(db.get_item_by_id(id).unwrap().unwrap().paste_count, 2);
        assert_eq!(db.increment_paste_count(id + 100).unwrap(), 0);
    }

    #[test]
    fn copies_are_merged_only_when_text_and_html_both_match() {
        let db = Database::new(":memory:").unwrap();
        let plain = add(&db, "同一句");
        assert_eq!(add(&db, "同一句"), plain);
        let mut rich = PyClipboardItem::new(0, "同一句".to_string(), "text".to_string());
        rich.html_content = Some("<b>同一句</b>".to_string());
        let rich_id = db.insert_item(&rich).unwrap();
        assert_ne!(rich_id, plain);
        assert_eq!(db.insert_item(&rich).unwrap(), rich_id);
        // 合并后排到最前
        let first = db.query_items(0, 1, None, None, None, None).unwrap().items[0].id;
        assert_eq!(first, rich_id);
    }
}
