use rusqlite::{Connection, params};
use crate::types::{PyClipboardItem, PyPaginatedResult};

/// SQLite 数据库管理
pub struct Database {
    conn: Connection,
}

impl Database {
    /// 创建或打开数据库
    pub fn new(db_path: &str) -> Result<Self, String> {
        let conn = Connection::open(db_path)
            .map_err(|e| format!("打开数据库失败: {}", e))?;
        
        // 创建表
        conn.execute(
            "CREATE TABLE IF NOT EXISTS clipboard (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                content TEXT NOT NULL,
                html_content TEXT,
                content_type TEXT NOT NULL DEFAULT 'text',
                image_id TEXT,
                item_order INTEGER NOT NULL DEFAULT 0,
                is_pinned INTEGER NOT NULL DEFAULT 0,
                paste_count INTEGER NOT NULL DEFAULT 0,
                source_app TEXT,
                char_count INTEGER,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            )",
            [],
        ).map_err(|e| format!("创建表失败: {}", e))?;
        
        // 创建索引
        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_clipboard_order ON clipboard(is_pinned DESC, item_order DESC)",
            [],
        );
        
        let _ = conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_clipboard_content ON clipboard(content)",
            [],
        );
        
        // 性能优化
        conn.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             PRAGMA cache_size = 10000;"
        ).map_err(|e| format!("设置参数失败: {}", e))?;
        
        Ok(Self { conn })
    }
    
    /// 插入新记录
    pub fn insert_item(&self, item: &PyClipboardItem) -> Result<i64, String> {
        let now = chrono::Local::now().timestamp();
        let char_count = item.content.chars().count() as i64;
        
        // 检查重复（如果内容已存在，更新它的顺序）
        let exists: i64 = self.conn.query_row(
            "SELECT id FROM clipboard WHERE content = ?1 ORDER BY created_at DESC LIMIT 1",
            params![&item.content],
            |row| row.get(0)
        ).unwrap_or(0);
        
        if exists > 0 {
            // 更新已存在的项
            self.conn.execute(
                "UPDATE clipboard SET updated_at = ?1, item_order = (SELECT COALESCE(MAX(item_order), 0) + 1 FROM clipboard) WHERE id = ?2",
                params![now, exists],
            ).map_err(|e| format!("更新失败: {}", e))?;
            return Ok(exists);
        }
        
        // 获取最大顺序
        let max_order: i64 = self.conn.query_row(
            "SELECT COALESCE(MAX(item_order), 0) FROM clipboard",
            [],
            |row| row.get(0)
        ).unwrap_or(0);
        
        // 插入新记录
        self.conn.execute(
            "INSERT INTO clipboard (content, html_content, content_type, image_id, item_order, 
             is_pinned, paste_count, source_app, char_count, created_at, updated_at) 
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
            params![
                &item.content,
                &item.html_content,
                &item.content_type,
                &item.image_id,
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
            "SELECT id, content, html_content, content_type, image_id, is_pinned, 
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
                content: row.get(1)?,
                html_content: row.get(2)?,
                content_type: row.get(3)?,
                image_id: row.get(4)?,
                is_pinned: row.get::<_, i64>(5)? != 0,
                paste_count: row.get(6)?,
                source_app: row.get(7)?,
                char_count: row.get(8)?,
                created_at: row.get(9)?,
                updated_at: row.get(10)?,
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
            "SELECT id, content, html_content, content_type, image_id, is_pinned, 
             paste_count, source_app, char_count, created_at, updated_at 
             FROM clipboard WHERE id = ?",
            params![id],
            |row| {
                Ok(PyClipboardItem {
                    id: row.get(0)?,
                    content: row.get(1)?,
                    html_content: row.get(2)?,
                    content_type: row.get(3)?,
                    image_id: row.get(4)?,
                    is_pinned: row.get::<_, i64>(5)? != 0,
                    paste_count: row.get(6)?,
                    source_app: row.get(7)?,
                    char_count: row.get(8)?,
                    created_at: row.get(9)?,
                    updated_at: row.get(10)?,
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
        self.conn.execute("DELETE FROM clipboard WHERE id = ?", params![id])
            .map_err(|e| format!("删除失败: {}", e))?;
        Ok(())
    }
    
    /// 清空所有记录
    pub fn clear_all(&self) -> Result<(), String> {
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
}
