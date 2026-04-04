use rusqlite::{Connection, params};
use crate::types::{PyClipboardItem, PyPaginatedResult, PyGroup};
use std::path::PathBuf;

// 压缩阈值：超过 100KB 的 data 用 zstd 压缩
const COMPRESS_THRESHOLD: usize = 100 * 1024;

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
        
        // 创建剪贴板表
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
        
        // 迁移：添加 title 字段（如果不存在）
        let _ = conn.execute("ALTER TABLE clipboard ADD COLUMN title TEXT", []);

        // ── Ditto 风格：原始格式数据表 ──────────────────────────────────────
        // clipboard_formats 与 clipboard 通过 event_id 关联（一次复制对应一个 event_id）
        // event_id 就是 clipboard.id（主记录的 rowid）
        // 每行保存一个 Windows 剪贴板格式的原始 BLOB
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

        // 兼容旧数据库：若 compressed 列不存在则添加
        let _ = conn.execute(
            "ALTER TABLE clipboard_formats ADD COLUMN compressed INTEGER NOT NULL DEFAULT 0",
            [],
        );

        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_formats_event ON clipboard_formats(event_id)",
            [],
        );

        // 唯一约束：同一次复制事件中，同一个 format_id 只存一条
        // 这样 INSERT OR IGNORE 才真正有效（防止重复抓取同一格式）
        let _ = conn.execute(
            "CREATE UNIQUE INDEX IF NOT EXISTS ux_formats_event_format
             ON clipboard_formats(event_id, format_id)",
            [],
        );

        // 创建分组表
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
        
        // 创建索引
        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_clipboard_order ON clipboard(is_pinned DESC, item_order DESC)",
            [],
        );
        
        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_clipboard_content ON clipboard(content)",
            [],
        );
        
        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_clipboard_group ON clipboard(group_id)",
            [],
        );
        
        // 为 image_id 创建索引（优化图片去重查询）
        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_clipboard_image_id ON clipboard(image_id)",
            [],
        );
        
        // 性能优化 + 启用外键（必须开启，否则 ON DELETE CASCADE 不生效）
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
    
    /// 插入新记录
    pub fn insert_item(&self, item: &PyClipboardItem) -> Result<i64, String> {
        let now = chrono::Local::now().timestamp();
        let char_count = item.content.chars().count() as i64;
        
        // 检查重复：
        // 1. 如果有 title（收藏内容），则不去重，允许相同内容不同标题的多条记录
        // 2. 如果是图片类型，用 image_id 去重（避免相同尺寸的不同图片被误判为重复）
        // 3. 如果是文本/文件类型，用 content 和 html_content 去重
        let existing_id: Option<i64> = if item.title.is_none() {
            if item.content_type == "image" && item.image_id.is_some() {
                // 图片类型：用 image_id 去重（精确匹配，不会误判）
                self.conn.query_row(
                    "SELECT id FROM clipboard WHERE image_id = ?1 AND content_type = 'image' ORDER BY created_at DESC LIMIT 1",
                    params![&item.image_id],
                    |row| row.get(0)
                ).ok()
            } else {
                // 文本/文件类型：用 content 去重
                self.conn.query_row(
                    "SELECT id FROM clipboard WHERE content = ?1 AND content_type = ?2 AND (html_content IS ?3 OR (html_content IS NULL AND ?3 IS NULL)) AND title IS NULL ORDER BY created_at DESC LIMIT 1",
                    params![&item.content, &item.content_type, &item.html_content],
                    |row| row.get(0)
                ).ok()
            }
        } else {
            None  // 有 title 的不去重
        };
        
        if let Some(id) = existing_id {
            // 内容完全相同，只更新顺序和时间，让它排到最前面
            self.conn.execute(
                "UPDATE clipboard SET updated_at = ?1, item_order = (SELECT COALESCE(MAX(item_order), 0) + 1000 FROM clipboard) WHERE id = ?2",
                params![now, id],
            ).map_err(|e| format!("更新失败: {}", e))?;
            return Ok(id);
        }
        
        // 获取最大顺序
        let max_order: i64 = self.conn.query_row(
            "SELECT COALESCE(MAX(item_order), 0) FROM clipboard",
            [],
            |row| row.get(0)
        ).unwrap_or(0);
        
        // 插入新记录
        self.conn.execute(
            "INSERT INTO clipboard (title, content, html_content, content_type, image_id, thumbnail, item_order, 
             is_pinned, paste_count, source_app, char_count, created_at, updated_at) 
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)",
            params![
                &item.title,
                &item.content,
                &item.html_content,
                &item.content_type,
                &item.image_id,
                &item.thumbnail,
                max_order + 1000,
                item.is_pinned,
                item.paste_count,
                &item.source_app,
                char_count,
                now,
                now,
            ],
        ).map_err(|e| format!("插入失败: {}", e))?;
        
        Ok(self.conn.last_insert_rowid())
    }
    
    /// 分页查询
    pub fn query_items(
        &self,
        offset: i64,
        limit: i64,
        search: Option<String>,
        content_type: Option<String>,
    ) -> Result<PyPaginatedResult, String> {
        let mut where_clauses = vec![];
        let mut params_vec: Vec<String> = vec![];
        
        if let Some(ref s) = search {
            if !s.trim().is_empty() {
                where_clauses.push("content LIKE ?".to_string());
                params_vec.push(format!("%{}%", s));
            }
        }
        
        if let Some(ref ct) = content_type {
            if ct != "all" {
                where_clauses.push("content_type = ?".to_string());
                params_vec.push(ct.clone());
            }
        }
        
        let where_clause = if where_clauses.is_empty() {
            String::new()
        } else {
            format!("WHERE {}", where_clauses.join(" AND "))
        };
        
        // 获取总数
        let count_sql = format!("SELECT COUNT(*) FROM clipboard {}", where_clause);
        let total_count: i64 = if params_vec.is_empty() {
            self.conn.query_row(&count_sql, [], |row| row.get(0)).unwrap_or(0)
        } else if params_vec.len() == 1 {
            self.conn.query_row(&count_sql, [&params_vec[0]], |row| row.get(0)).unwrap_or(0)
        } else {
            self.conn.query_row(&count_sql, [&params_vec[0], &params_vec[1]], |row| row.get(0)).unwrap_or(0)
        };
        
        // 查询数据
        let query_sql = format!(
            "SELECT id, title, content, html_content, content_type, image_id, thumbnail, is_pinned, 
             paste_count, source_app, char_count, created_at, updated_at 
             FROM clipboard {} 
             ORDER BY is_pinned DESC, item_order DESC 
             LIMIT ? OFFSET ?",
            where_clause
        );
        
        let mut stmt = self.conn.prepare(&query_sql)
            .map_err(|e| format!("准备查询失败: {}", e))?;
        
        let map_row = |row: &rusqlite::Row| -> rusqlite::Result<PyClipboardItem> {
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
        };
        
        let items: Vec<PyClipboardItem> = if params_vec.is_empty() {
            stmt.query_map([limit, offset], map_row)
        } else if params_vec.len() == 1 {
            stmt.query_map(params![&params_vec[0], limit, offset], map_row)
        } else {
            stmt.query_map(params![&params_vec[0], &params_vec[1], limit, offset], map_row)
        }.map_err(|e| format!("查询失败: {}", e))?
        .filter_map(|r| r.ok())
        .collect();
        
        Ok(PyPaginatedResult::new(total_count, items, offset, limit))
    }
    
    /// 获取总记录数
    pub fn get_count(&self) -> Result<i64, String> {
        self.conn.query_row("SELECT COUNT(*) FROM clipboard", [], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))
    }
    
    /// 根据 ID 获取记录
    pub fn get_item_by_id(&self, id: i64) -> Result<Option<PyClipboardItem>, String> {
        let result = self.conn.query_row(
            "SELECT id, title, content, html_content, content_type, image_id, thumbnail, is_pinned, 
             paste_count, source_app, char_count, created_at, updated_at 
             FROM clipboard WHERE id = ?",
            params![id],
            |row| {
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
        );
        
        match result {
            Ok(item) => Ok(Some(item)),
            Err(rusqlite::Error::QueryReturnedNoRows) => Ok(None),
            Err(e) => Err(format!("查询失败: {}", e)),
        }
    }
    
    /// 删除记录
    pub fn delete_item(&self, id: i64) -> Result<(), String> {
        // 先获取 image_id，以便删除图片文件
        let image_id: Option<String> = self.conn.query_row(
            "SELECT image_id FROM clipboard WHERE id = ?",
            params![id],
            |row| row.get(0)
        ).ok();
        
        // 删除图片文件
        if let Some(img_id) = image_id {
            if !img_id.is_empty() {
                let images_dir = self.get_images_dir();
                let image_path = images_dir.join(format!("{}.png", img_id));
                let _ = std::fs::remove_file(&image_path);
            }
        }
        
        self.conn.execute("DELETE FROM clipboard WHERE id = ?", params![id])
            .map_err(|e| format!("删除失败: {}", e))?;
        Ok(())
    }
    
    /// 清空记录
    ///
    /// Args:
    ///     keep_grouped: true = 保留已加入分组的条目（只删历史区），false = 删除全部
    pub fn clear_all(&self, keep_grouped: bool) -> Result<(), String> {
        // 构建 WHERE 条件
        let where_clause = if keep_grouped {
            "WHERE group_id IS NULL"
        } else {
            ""
        };

        // 先获取要删除记录的 image_id，以便清理图片文件
        let sql_images = if keep_grouped {
            "SELECT image_id FROM clipboard WHERE group_id IS NULL AND image_id IS NOT NULL AND image_id != ''"
        } else {
            "SELECT image_id FROM clipboard WHERE image_id IS NOT NULL AND image_id != ''"
        };

        let mut stmt = self.conn.prepare(sql_images)
            .map_err(|e| format!("准备查询失败: {}", e))?;

        let image_ids: Vec<String> = stmt.query_map([], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))?
            .filter_map(|r| r.ok())
            .collect();

        // 删除图片文件
        let images_dir = self.get_images_dir();
        for img_id in image_ids {
            let image_path = images_dir.join(format!("{}.png", img_id));
            let _ = std::fs::remove_file(&image_path);
        }

        // 删除记录（ON DELETE CASCADE 自动清理 clipboard_formats）
        let sql_delete = format!("DELETE FROM clipboard {}", where_clause);
        self.conn.execute(&sql_delete, [])
            .map_err(|e| format!("清空失败: {}", e))?;

        // WAL checkpoint：把 WAL 文件的内容合并回主库并截断 WAL 文件
        let _ = self.conn.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);");

        // VACUUM：整理主库文件，将空闲页回收给操作系统，文件大小真正缩小
        self.conn.execute_batch("VACUUM;")
            .map_err(|e| format!("VACUUM 失败: {}", e))?;

        Ok(())
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
            params![new_state, chrono::Local::now().timestamp(), id]
        ).map_err(|e| format!("更新失败: {}", e))?;
        
        Ok(new_state == 1)
    }
    
    // ==================== 分组功能 ====================
    
    /// 创建分组
    pub fn create_group(&self, name: &str, color: Option<&str>, icon: Option<&str>) -> Result<i64, String> {
        let now = chrono::Local::now().timestamp();
        let max_order: i64 = self.conn.query_row(
            "SELECT COALESCE(MAX(item_order), 0) FROM groups",
            [],
            |row| row.get(0)
        ).unwrap_or(0);
        
        self.conn.execute(
            "INSERT INTO groups (name, color, icon, item_order, created_at) VALUES (?1, ?2, ?3, ?4, ?5)",
            params![name, color, icon, max_order + 1000, now],
        ).map_err(|e| format!("创建分组失败: {}", e))?;
        
        Ok(self.conn.last_insert_rowid())
    }
    
    /// 获取所有分组
    pub fn get_groups(&self) -> Result<Vec<PyGroup>, String> {
        let mut stmt = self.conn.prepare(
            "SELECT id, name, color, icon, item_order, created_at FROM groups ORDER BY item_order ASC"
        ).map_err(|e| format!("查询分组失败: {}", e))?;
        
        let groups = stmt.query_map([], |row| {
            Ok(PyGroup {
                id: row.get(0)?,
                name: row.get(1)?,
                color: row.get(2)?,
                icon: row.get(3)?,
                item_order: row.get(4)?,
                created_at: row.get(5)?,
            })
        }).map_err(|e| format!("查询分组失败: {}", e))?
        .filter_map(|r| r.ok())
        .collect();
        
        Ok(groups)
    }
    
    /// 删除分组
    pub fn delete_group(&self, id: i64) -> Result<(), String> {
        // 先将该分组下的项目移到无分组
        self.conn.execute(
            "UPDATE clipboard SET group_id = NULL WHERE group_id = ?",
            params![id],
        ).map_err(|e| format!("更新项目失败: {}", e))?;
        
        self.conn.execute("DELETE FROM groups WHERE id = ?", params![id])
            .map_err(|e| format!("删除分组失败: {}", e))?;
        Ok(())
    }
    
    /// 重命名分组
    pub fn rename_group(&self, id: i64, name: &str) -> Result<(), String> {
        self.conn.execute(
            "UPDATE groups SET name = ? WHERE id = ?",
            params![name, id],
        ).map_err(|e| format!("重命名分组失败: {}", e))?;
        Ok(())
    }
    
    /// 更新分组（名称、颜色、图标）
    pub fn update_group(&self, id: i64, name: &str, color: Option<&str>, icon: Option<&str>) -> Result<(), String> {
        self.conn.execute(
            "UPDATE groups SET name = ?, color = ?, icon = ? WHERE id = ?",
            params![name, color, icon, id],
        ).map_err(|e| format!("更新分组失败: {}", e))?;
        Ok(())
    }
    
    /// 将项目移动到分组
    pub fn move_to_group(&self, item_id: i64, group_id: Option<i64>) -> Result<(), String> {
        self.conn.execute(
            "UPDATE clipboard SET group_id = ?, updated_at = ? WHERE id = ?",
            params![group_id, chrono::Local::now().timestamp(), item_id],
        ).map_err(|e| format!("移动到分组失败: {}", e))?;
        Ok(())
    }
    
    /// 按分组查询
    pub fn query_by_group(&self, group_id: Option<i64>, offset: i64, limit: i64) -> Result<PyPaginatedResult, String> {
        let (where_clause, _count_params, _query_params): (String, Vec<i64>, Vec<i64>) = if let Some(gid) = group_id {
            (
                "WHERE group_id = ?".to_string(),
                vec![gid],
                vec![gid, limit, offset]
            )
        } else {
            (
                "WHERE group_id IS NULL".to_string(),
                vec![],
                vec![limit, offset]
            )
        };
        
        // 获取总数
        let total_count: i64 = if group_id.is_some() {
            self.conn.query_row(
                &format!("SELECT COUNT(*) FROM clipboard {}", where_clause),
                params![group_id.unwrap()],
                |row| row.get(0)
            ).unwrap_or(0)
        } else {
            self.conn.query_row(
                &format!("SELECT COUNT(*) FROM clipboard {}", where_clause),
                [],
                |row| row.get(0)
            ).unwrap_or(0)
        };
        
        // 查询数据 - 分组内按 ASC 排序（新内容在下，适合收藏内容）
        let query_sql = format!(
            "SELECT id, title, content, html_content, content_type, image_id, thumbnail, is_pinned, 
             paste_count, source_app, char_count, created_at, updated_at 
             FROM clipboard {} 
             ORDER BY is_pinned DESC, item_order ASC 
             LIMIT ? OFFSET ?",
            where_clause
        );
        
        let mut stmt = self.conn.prepare(&query_sql)
            .map_err(|e| format!("准备查询失败: {}", e))?;
        
        let map_row = |row: &rusqlite::Row| -> rusqlite::Result<PyClipboardItem> {
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
        };
        
        let items: Vec<PyClipboardItem> = if group_id.is_some() {
            stmt.query_map(params![group_id.unwrap(), limit, offset], map_row)
        } else {
            stmt.query_map(params![limit, offset], map_row)
        }.map_err(|e| format!("查询失败: {}", e))?
        .filter_map(|r| r.ok())
        .collect();
        
        Ok(PyPaginatedResult::new(total_count, items, offset, limit))
    }
    
    /// 增加粘贴次数
    pub fn increment_paste_count(&self, id: i64) -> Result<i64, String> {
        self.conn.execute(
            "UPDATE clipboard SET paste_count = paste_count + 1, updated_at = ? WHERE id = ?",
            params![chrono::Local::now().timestamp(), id],
        ).map_err(|e| format!("更新粘贴次数失败: {}", e))?;
        
        let count: i64 = self.conn.query_row(
            "SELECT paste_count FROM clipboard WHERE id = ?",
            params![id],
            |row| row.get(0)
        ).unwrap_or(0);
        
        Ok(count)
    }
    
    /// 将某项移到最前（更新 item_order 为最大值 + 1000）
    pub fn move_item_to_top(&self, id: i64) -> Result<(), String> {
        self.conn.execute(
            "UPDATE clipboard SET item_order = (SELECT COALESCE(MAX(item_order), 0) + 1000 FROM clipboard), updated_at = ? WHERE id = ?",
            params![chrono::Local::now().timestamp(), id],
        ).map_err(|e| format!("移动到最前失败: {}", e))?;
        Ok(())
    }
    
    /// 移动剪贴板内容到指定位置（拖拽排序核心接口）
    /// 
    /// 使用稀疏整数算法，在 before 和 after 之间插入
    /// 
    /// Args:
    ///     id: 要移动的项
    ///     before_id: 它前面的项（None = 移到最前）
    ///     after_id: 它后面的项（None = 移到最后）
    pub fn move_item_between(
        &self,
        id: i64,
        before_id: Option<i64>,
        after_id: Option<i64>,
    ) -> Result<(), String> {
        self.move_item_between_impl(id, before_id, after_id, 0)
    }
    
    /// 内部实现，带递归深度检查
    fn move_item_between_impl(
        &self,
        id: i64,
        before_id: Option<i64>,
        after_id: Option<i64>,
        depth: i32,
    ) -> Result<(), String> {
        // 防止无限递归
        if depth > 5 {
            return Err("重新索引次数过多，可能存在问题".to_string());
        }
        
        // 注意：分组内容使用 ASC 排序（小的在上面）
        // before_id 是界面上方的项，order 更小
        // after_id 是界面下方的项，order 更大
        
        // 获取该项的 group_id（用于查询范围）
        let group_id: Option<i64> = self.conn.query_row(
            "SELECT group_id FROM clipboard WHERE id = ?",
            params![id],
            |row| row.get(0)
        ).unwrap_or(None);
        
        // 获取上方项的 item_order（应该更小）
        let upper_order = if let Some(bid) = before_id {
            self.conn.query_row(
                "SELECT item_order FROM clipboard WHERE id = ?",
                params![bid],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(i64::MIN)
        } else {
            // 移到最前，获取当前最小值 - 1000
            self.conn.query_row(
                "SELECT COALESCE(MIN(item_order), 1000) - 1000 FROM clipboard WHERE group_id = ?",
                params![group_id],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(0)
        };
        
        // 获取下方项的 item_order（应该更大）
        let lower_order = if let Some(aid) = after_id {
            self.conn.query_row(
                "SELECT item_order FROM clipboard WHERE id = ?",
                params![aid],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(i64::MAX)
        } else {
            // 移到最后，获取当前最大值 + 1000
            self.conn.query_row(
                "SELECT COALESCE(MAX(item_order), 0) + 1000 FROM clipboard WHERE group_id = ?",
                params![group_id],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(1000)
        };
        
        // 检查空间是否足够（lower 应该 > upper）
        if lower_order <= upper_order || lower_order - upper_order < 10 {
            // 重新索引该分组的内容
            self.reindex_group_items(group_id.unwrap())?;
            return self.move_item_between_impl(id, before_id, after_id, depth + 1);
        }
        
        // 计算新的 item_order（中间值）
        let new_order = (upper_order + lower_order) / 2;
        
        // 更新位置
        self.conn.execute(
            "UPDATE clipboard SET item_order = ?, updated_at = ? WHERE id = ?",
            params![new_order, chrono::Local::now().timestamp(), id],
        ).map_err(|e| format!("移动失败: {}", e))?;
        
        Ok(())
    }
    
    /// 移动分组到指定位置（拖拽排序核心接口）
    /// 
    /// 与 move_item_between 完全同构，操作的是 groups.item_order
    /// 
    /// Args:
    ///     id: 要移动的分组
    ///     before_id: 它前面的分组（None = 移到最前）
    ///     after_id: 它后面的分组（None = 移到最后）
    pub fn move_group_between(
        &self,
        id: i64,
        before_id: Option<i64>,
        after_id: Option<i64>,
    ) -> Result<(), String> {
        self.move_group_between_impl(id, before_id, after_id, 0)
    }
    
    /// 内部实现，带递归深度检查
    fn move_group_between_impl(
        &self,
        id: i64,
        before_id: Option<i64>,
        after_id: Option<i64>,
        depth: i32,
    ) -> Result<(), String> {
        // 防止无限递归
        if depth > 5 {
            return Err("分组重新索引次数过多，可能存在问题".to_string());
        }
        
        // 注意：界面按 item_order ASC 排序（小的在上面）
        // before_id 是界面上方的项，order 更小
        // after_id 是界面下方的项，order 更大
        
        // 获取上方项的 item_order（应该更小）
        let upper_order = if let Some(bid) = before_id {
            self.conn.query_row(
                "SELECT item_order FROM groups WHERE id = ?",
                params![bid],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(i64::MIN)
        } else {
            // 移到最前，获取当前最小值 - 1000
            self.conn.query_row(
                "SELECT COALESCE(MIN(item_order), 1000) - 1000 FROM groups",
                [],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(0)
        };
        
        // 获取下方项的 item_order（应该更大）
        let lower_order = if let Some(aid) = after_id {
            self.conn.query_row(
                "SELECT item_order FROM groups WHERE id = ?",
                params![aid],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(i64::MAX)
        } else {
            // 移到最后，获取当前最大值 + 1000
            self.conn.query_row(
                "SELECT COALESCE(MAX(item_order), 0) + 1000 FROM groups",
                [],
                |row| row.get::<_, i64>(0)
            ).unwrap_or(1000)
        };
        
        // 检查空间是否足够（lower 应该 > upper）
        if lower_order <= upper_order || lower_order - upper_order < 10 {
            // 空间不足或逆序，触发 reindex
            self.reindex_groups()?;
            // 递归调用，深度+1
            return self.move_group_between_impl(id, before_id, after_id, depth + 1);
        }
        
        // 计算新的 item_order（中间值）
        let new_order = (upper_order + lower_order) / 2;
        
        // 更新位置
        self.conn.execute(
            "UPDATE groups SET item_order = ? WHERE id = ?",
            params![new_order, id],
        ).map_err(|e| format!("移动分组失败: {}", e))?;
        
        Ok(())
    }
    
    /// 重新索引剪贴板内容的 item_order（按当前顺序重新分配稀疏值）
    /// 
    /// 只在空间不足时调用，重新分配为 1000, 2000, 3000, ...
    #[allow(dead_code)]
    fn reindex_clipboard_items(&self) -> Result<(), String> {
        // 按当前排序获取所有 ID
        let mut stmt = self.conn.prepare(
            "SELECT id FROM clipboard ORDER BY is_pinned DESC, item_order DESC"
        ).map_err(|e| format!("准备查询失败: {}", e))?;
        
        let ids: Vec<i64> = stmt.query_map([], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))?
            .filter_map(|r| r.ok())
            .collect();
        
        // 重新分配 item_order（倒序，因为原本是 DESC）
        for (index, id) in ids.iter().enumerate() {
            let new_order = (ids.len() - index) as i64 * 1000;
            self.conn.execute(
                "UPDATE clipboard SET item_order = ? WHERE id = ?",
                params![new_order, id],
            ).map_err(|e| format!("重新索引失败: {}", e))?;
        }
        
        Ok(())
    }
    
    /// 重新索引分组的 item_order（按当前顺序重新分配稀疏值）
    fn reindex_groups(&self) -> Result<(), String> {
        // 按当前排序获取所有 ID（ASC：小的在前，旧的在前）
        let mut stmt = self.conn.prepare(
            "SELECT id FROM groups ORDER BY item_order ASC"
        ).map_err(|e| format!("准备查询失败: {}", e))?;
        
        let ids: Vec<i64> = stmt.query_map([], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))?
            .filter_map(|r| r.ok())
            .collect();
        
        // 重新分配 item_order（按序分配：第一个1000，第二个2000...）
        for (index, id) in ids.iter().enumerate() {
            let new_order = (index + 1) as i64 * 1000;  // 1000, 2000, 3000, ...
            self.conn.execute(
                "UPDATE groups SET item_order = ? WHERE id = ?",
                params![new_order, id],
            ).map_err(|e| format!("重新索引分组失败: {}", e))?;
        }
        
        Ok(())
    }
    
    /// 重新索引分组内容的 item_order（按当前顺序重新分配稀疏值）
    fn reindex_group_items(&self, group_id: i64) -> Result<(), String> {
        // 按当前排序获取该分组内所有内容的 ID（ASC：小的在前）
        let mut stmt = self.conn.prepare(
            "SELECT id FROM clipboard WHERE group_id = ? ORDER BY item_order ASC"
        ).map_err(|e| format!("准备查询失败: {}", e))?;
        
        let ids: Vec<i64> = stmt.query_map(params![group_id], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))?
            .filter_map(|r| r.ok())
            .collect();
        
        // 重新分配 item_order（按序分配：第一个1000，第二个2000...）
        for (index, id) in ids.iter().enumerate() {
            let new_order = (index + 1) as i64 * 1000;  // 1000, 2000, 3000, ...
            self.conn.execute(
                "UPDATE clipboard SET item_order = ? WHERE id = ?",
                params![new_order, id],
            ).map_err(|e| format!("重新索引分组内容失败: {}", e))?;
        }
        
        Ok(())
    }
    
    /// 更新内容项（标题和内容）
    pub fn update_item(&self, id: i64, title: Option<&str>, content: &str) -> Result<(), String> {
        self.conn.execute(
            "UPDATE clipboard SET title = ?, content = ?, updated_at = ? WHERE id = ?",
            params![title, content, chrono::Local::now().timestamp(), id],
        ).map_err(|e| format!("更新内容失败: {}", e))?;
        Ok(())
    }

    // ==================== 原始格式存取（Ditto 风格）====================

    /// 保存一批原始剪贴板格式数据，关联到指定 event_id（即 clipboard.id）
    /// 数据在此函数内进行 zstd 压缩（超过阈值时），适合外部传入原始数据的场景
    pub fn insert_formats(&self, event_id: i64, formats: &[(u32, String, Vec<u8>)]) -> Result<(), String> {
        for (format_id, format_name, data) in formats {
            let (store_data, compressed): (Vec<u8>, i64) =
                if data.len() > COMPRESS_THRESHOLD {
                    match zstd::encode_all(data.as_slice(), 3) {
                        Ok(compressed_data) => (compressed_data, 1),
                        Err(_) => (data.clone(), 0),
                    }
                } else {
                    (data.clone(), 0)
                };

            self.conn.execute(
                "INSERT OR IGNORE INTO clipboard_formats (event_id, format_id, format_name, data, compressed)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![event_id, *format_id as i64, format_name, store_data, compressed],
            ).map_err(|e| format!("插入 format 失败: {}", e))?;
        }
        Ok(())
    }

    /// 保存一批已预压缩的格式数据（监听线程专用）
    /// 调用方已在外部完成压缩，此处直接写库，不再重复压缩
    /// formats: (format_id, format_name, data, is_compressed)
    pub fn insert_precompressed_formats(
        &self,
        event_id: i64,
        formats: &[(u32, String, Vec<u8>, bool)],
    ) -> Result<(), String> {
        for (format_id, format_name, data, is_compressed) in formats {
            let compressed_flag: i64 = if *is_compressed { 1 } else { 0 };
            self.conn.execute(
                "INSERT OR IGNORE INTO clipboard_formats (event_id, format_id, format_name, data, compressed)
                 VALUES (?1, ?2, ?3, ?4, ?5)",
                params![event_id, *format_id as i64, format_name, data, compressed_flag],
            ).map_err(|e| format!("插入 format 失败: {}", e))?;
        }
        Ok(())
    }

    /// 读取某个 event 的所有原始格式数据（自动解压 zstd 数据）
    /// 返回 Vec<(format_id, format_name, data)>
    pub fn get_formats(&self, event_id: i64) -> Result<Vec<(u32, String, Vec<u8>)>, String> {
        let mut stmt = self.conn.prepare(
            "SELECT format_id, format_name, data, compressed FROM clipboard_formats WHERE event_id = ? ORDER BY format_id ASC"
        ).map_err(|e| format!("准备查询 formats 失败: {}", e))?;

        let rows = stmt.query_map(params![event_id], |row| {
            Ok((
                row.get::<_, i64>(0)? as u32,
                row.get::<_, String>(1)?,
                row.get::<_, Vec<u8>>(2)?,
                row.get::<_, i64>(3).unwrap_or(0),
            ))
        }).map_err(|e| format!("查询 formats 失败: {}", e))?
        .filter_map(|r| r.ok())
        .map(|(fid, fname, data, compressed)| {
            let decoded = if compressed == 1 {
                zstd::decode_all(data.as_slice()).unwrap_or(data)
            } else {
                data
            };
            (fid, fname, decoded)
        })
        .collect();

        Ok(rows)
    }

    /// 删除某个 event 的所有原始格式数据（级联删除时自动触发，也可手动调用）
    #[allow(dead_code)]
    pub fn delete_formats(&self, event_id: i64) -> Result<(), String> {
        self.conn.execute(
            "DELETE FROM clipboard_formats WHERE event_id = ?",
            params![event_id],
        ).map_err(|e| format!("删除 formats 失败: {}", e))?;
        Ok(())
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
        
        // 获取当前非分组内容的总数（只统计自动监听的历史记录）
        let total: i64 = self.conn.query_row(
            "SELECT COUNT(*) FROM clipboard WHERE group_id IS NULL",
            [],
            |row| row.get(0)
        ).unwrap_or(0);
        
        if total <= limit {
            return Ok(0);
        }
        
        // 计算需要删除的数量
        let to_delete = total - limit;
        
        // 先获取要删除记录的 image_id 列表（用于清理图片文件）
        // 注意：必须使用与删除相同的查询条件，确保只获取真正要删除的记录的图片
        let mut stmt = self.conn.prepare(
            "SELECT image_id FROM clipboard 
             WHERE id IN (
                 SELECT id FROM clipboard 
                 WHERE is_pinned = 0 AND group_id IS NULL
                 ORDER BY item_order ASC 
                 LIMIT ?
             )
             AND image_id IS NOT NULL AND image_id != ''"
        ).map_err(|e| format!("准备查询失败: {}", e))?;
        
        let image_ids: Vec<String> = stmt.query_map(params![to_delete], |row| row.get(0))
            .map_err(|e| format!("查询失败: {}", e))?
            .filter_map(|r| r.ok())
            .collect();
        
        // 删除图片文件
        let images_dir = self.get_images_dir();
        for img_id in image_ids {
            let image_path = images_dir.join(format!("{}.png", img_id));
            let _ = std::fs::remove_file(&image_path);
        }
        
        // 删除最旧的非置顶、非分组记录
        // 按 item_order 升序（最旧的在前）
        // 只清理自动监听的历史记录，不清理分组内的收藏内容
        let deleted = self.conn.execute(
            "DELETE FROM clipboard WHERE id IN (
                SELECT id FROM clipboard 
                WHERE is_pinned = 0 AND group_id IS NULL
                ORDER BY item_order ASC 
                LIMIT ?
            )",
            params![to_delete],
        ).map_err(|e| format!("清理失败: {}", e))?;
        
        Ok(deleted as i64)
    }
}
