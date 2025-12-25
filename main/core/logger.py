"""
日志管理模块 - 统一日志记录

功能：
1. 自动创建日志目录
2. 按日期分割日志文件（runtime_YYYYMMDD.log）
3. 支持日志开关（从配置读取）
4. 同时输出到终端和文件
5. 异常捕获和记录

使用方式：
    from core.logger import setup_logger, get_logger
    
    # 初始化（在 main_app.py 启动时调用）
    setup_logger()
    
    # 获取日志实例
    logger = get_logger()
    logger.info("程序启动")
    logger.error("发生错误")
"""

import sys
import os
import io
import threading
from datetime import datetime
from pathlib import Path
from typing import Optional


class TeeStream(io.TextIOBase):
    """将输出同时写入多个流（终端 + 文件）"""
    
    def __init__(self, *targets):
        super().__init__()
        self._targets = [t for t in targets if t]
    
    def write(self, data):
        for target in self._targets:
            try:
                target.write(data)
            except Exception:
                pass
        return len(data)
    
    def flush(self):
        for target in self._targets:
            try:
                target.flush()
            except Exception:
                pass


class Logger:
    """
    日志管理器
    
    特性：
    - 单例模式
    - 自动按日期创建日志文件
    - 支持日志开关
    - 线程安全
    """
    
    _instance: Optional['Logger'] = None
    _lock = threading.Lock()
    
    def __new__(cls):
        """确保只有一个日志实例（单例模式）"""
        if cls._instance is None:
            with cls._lock:
                if cls._instance is None:
                    cls._instance = super().__new__(cls)
        return cls._instance
    
    def __init__(self):
        """初始化日志管理器"""
        # 避免重复初始化
        if hasattr(self, '_initialized'):
            return
        
        self.enabled = False
        self.log_dir: Optional[Path] = None
        self.log_file: Optional[io.TextIOWrapper] = None
        
        # 保存原始流
        self._original_stdout = sys.stdout
        self._original_stderr = sys.stderr
        
        self._ready = False
        self._initialized = True
    
    def setup(self, enabled: bool = True, log_dir: Optional[str] = None):
        """
        初始化日志系统
        
        Args:
            enabled: 是否启用日志
            log_dir: 日志目录路径（默认：~/AppData/Local/Jietuba/Logs）
        """
        self.enabled = enabled
        
        if not enabled:
            print("⚠️ [Logger] 日志功能已禁用")
            return
        
        if self._ready:
            print("⚠️ [Logger] 日志系统已经初始化")
            return
        
        # 设置日志目录
        if log_dir:
            self.log_dir = Path(log_dir)
        else:
            self.log_dir = Path.home() / "AppData" / "Local" / "Jietuba" / "Logs"
        
        try:
            # 创建日志目录
            self.log_dir.mkdir(parents=True, exist_ok=True)
            
            # 打开日志文件（按日期命名）
            log_filename = f"runtime_{datetime.now():%Y%m%d}.log"
            log_path = self.log_dir / log_filename
            
            # 以追加模式打开，行缓冲
            self.log_file = open(log_path, "a", encoding="utf-8", buffering=1)
            
            # 记录启动信息
            self._write_header()
            
            # 重定向 stdout 和 stderr（同时输出到终端和文件）
            sys.stdout = TeeStream(self._original_stdout, self.log_file)
            sys.stderr = TeeStream(self._original_stderr, self.log_file)
            
            self._ready = True
            self.info(f"✅ [Logger] 日志系统启动成功，日志文件：{log_path}")
            
        except Exception as e:
            print(f"❌ [Logger] 无法创建日志文件: {e}")
            self.enabled = False
    
    def _write_header(self):
        """写入日志文件头部信息"""
        if not self.log_file:
            return
        
        header = f"""
{'=' * 80}
Jietuba 截图工具 - 运行日志
启动时间: {datetime.now():%Y-%m-%d %H:%M:%S}
日志目录: {self.log_dir}
{'=' * 80}
"""
        self.log_file.write(header)
        self.log_file.flush()
    
    def _log(self, level: str, message: str):
        """
        写入日志
        
        Args:
            level: 日志级别（INFO/WARNING/ERROR）
            message: 日志内容
        """
        if not self.enabled:
            return
        
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        log_line = f"[{timestamp}] [{level}] {message}\n"
        
        # 直接输出（已经重定向到 TeeStream）
        print(log_line, end='')
    
    def info(self, message: str):
        """记录信息日志"""
        self._log("INFO", message)
    
    def warning(self, message: str):
        """记录警告日志"""
        self._log("WARNING", message)
    
    def error(self, message: str):
        """记录错误日志"""
        self._log("ERROR", message)
    
    def debug(self, message: str):
        """记录调试日志"""
        self._log("DEBUG", message)
    
    def set_enabled(self, enabled: bool):
        """
        动态开启/关闭日志
        
        注意：如果日志已经初始化，修改此设置不会关闭已打开的日志文件
        """
        self.enabled = enabled
        if enabled:
            self.info("📝 [Logger] 日志已启用")
        else:
            print("🔇 [Logger] 日志已禁用")
    
    def set_log_dir(self, log_dir: str):
        """
        设置日志目录（仅在未初始化时有效）
        
        Args:
            log_dir: 新的日志目录路径
        """
        if self._ready:
            self.warning("[Logger] 日志系统已初始化，无法更改日志目录")
            return
        
        self.log_dir = Path(log_dir)
        print(f"✅ [Logger] 日志目录已设置为: {log_dir}")
    
    def close(self):
        """关闭日志系统"""
        if not self._ready:
            return
        
        self.info("🛑 [Logger] 日志系统关闭")
        
        # 恢复原始流
        sys.stdout = self._original_stdout
        sys.stderr = self._original_stderr
        
        # 关闭日志文件
        if self.log_file:
            try:
                self.log_file.flush()
                self.log_file.close()
            except Exception:
                pass
            self.log_file = None
        
        self._ready = False


# ============================================================================
#  全局接口
# ============================================================================

_logger_instance: Optional[Logger] = None


def get_logger() -> Logger:
    """
    获取全局日志实例
    
    Returns:
        Logger: 日志管理器单例
    """
    global _logger_instance
    if _logger_instance is None:
        _logger_instance = Logger()
    return _logger_instance


def setup_logger(config_manager=None):
    """
    初始化日志系统（从配置读取设置）
    
    Args:
        config_manager: 配置管理器实例（ToolSettingsManager）
                       如果为 None，使用默认设置
    
    使用示例：
        from settings import get_tool_settings_manager
        setup_logger(get_tool_settings_manager())
    """
    logger = get_logger()
    
    if config_manager:
        # 从配置读取设置
        enabled = config_manager.get_log_enabled()
        log_dir = config_manager.get_log_dir()
    else:
        # 使用默认设置
        enabled = True
        log_dir = str(Path.home() / "AppData" / "Local" / "Jietuba" / "Logs")
    
    logger.setup(enabled=enabled, log_dir=log_dir)
