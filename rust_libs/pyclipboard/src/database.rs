use rusqlite::{Connection, params};
use crate::types::{PyClipboardItem, PyPaginatedResult, PyGroup};
use std::path::PathBuf;

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
        
        // 性能优化
        conn.execute_batch(
            "PRAGMA journal_mode = WAL;
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
        
        // 检查重复：同时比较 content 和 html_content，只有两者都相同才算重复
        // 这样从不同来源复制相同文本但格式不同时，会保存为不同的记录
        let existing_id: Option<i64> = self.conn.query_row(
            "SELECT id FROM clipboard WHERE content = ?1 AND content_type = ?2 AND (html_content IS ?3 OR (html_content IS NULL AND ?3 IS NULL)) ORDER BY created_at DESC LIMIT 1",
            params![&item.content, &item.content_type, &item.html_content],
            |row| row.get(0)
        ).ok();
        
        if let Some(id) = existing_id {
            // 内容完全相同，只更新顺序和时间，让它排到最前面
            self.conn.execute(
                "UPDATE clipboard SET updated_at = ?1, item_order = (SELECT COALESCE(MAX(item_order), 0) + 1 FROM clipboard) WHERE id = ?2",
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
                max_order + 1,
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
    
    /// 清空所有记录
    pub fn clear_all(&self) -> Result<(), String> {
        // 先获取所有 image_id，以便删除图片文件
        let mut stmt = self.conn.prepare(
            "SELECT image_id FROM clipboard WHERE image_id IS NOT NULL AND image_id != ''"
        ).map_err(|e| format!("准备查询失败: {}", e))?;
        
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
        
        // 删除所有记录
        self.conn.execute("DELETE FROM clipboard", [])
            .map_err(|e| format!("清空失败: {}", e))?;
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
            params![name, color, icon, max_order + 1, now],
        ).map_err(|e| format!("创建分组失败: {}", e))?;
        
        Ok(self.conn.last_insert_rowid())
    }
    
    /// 获取所有分组
    pub fn get_groups(&self) -> Result<Vec<PyGroup>, String> {
        let mut stmt = self.conn.prepare(
            "SELECT id, name, color, icon, item_order, created_at FROM groups ORDER BY item_order"
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
        let (where_clause, count_params, query_params): (String, Vec<i64>, Vec<i64>) = if let Some(gid) = group_id {
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
        
        // 查询数据 - 分组内按 id 排序保持稳定顺序（先加入的在前）
        let query_sql = format!(
            "SELECT id, title, content, html_content, content_type, image_id, thumbnail, is_pinned, 
             paste_count, source_app, char_count, created_at, updated_at 
             FROM clipboard {} 
             ORDER BY is_pinned DESC, id ASC 
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
    
    /// 将某项移到最前（更新 item_order 为最大值 + 1）
    pub fn move_item_to_top(&self, id: i64) -> Result<(), String> {
        self.conn.execute(
            "UPDATE clipboard SET item_order = (SELECT COALESCE(MAX(item_order), 0) + 1 FROM clipboard), updated_at = ? WHERE id = ?",
            params![chrono::Local::now().timestamp(), id],
        ).map_err(|e| format!("移动到最前失败: {}", e))?;
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
