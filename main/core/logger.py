"""
日志管理模块 - 统一日志记录

功能：
1. 自动创建日志目录
2. 按日期分割日志文件（runtime_YYYYMMDD.log）
3. 支持日志开关（从配置读取）
4. 同时输出到终端和文件
5. 异常捕获和记录
6. 静默异常记录（替代 except: pass）
7. 分级日志（DEBUG/INFO/WARNING/ERROR）
8. 模块标签支持

使用方式：
    from core.logger import setup_logger, get_logger
    from core.logger import log_debug, log_info, log_warning, log_error
    
    # 初始化（在 main_app.py 启动时调用）
    setup_logger()
    
    # 方式1：使用全局便捷函数（推荐）
    log_debug("调试信息", "ModuleName")
    log_info("普通信息", "ModuleName")
    log_warning("警告信息", "ModuleName")
    log_error("错误信息", "ModuleName")
    
    # 方式2：获取日志实例
    logger = get_logger()
    logger.info("程序启动")
    
    # 静默异常记录（替代 except: pass）
    try:
        some_risky_operation()
    except Exception as e:
        log_exception(e, "操作失败")
        
日志级别（从低到高）：
    DEBUG = 10   # 调试信息（开发时使用）
    INFO = 20    # 普通信息（功能状态）
    WARNING = 30 # 警告信息（潜在问题）
    ERROR = 40   # 错误信息（功能异常）
    SILENT = 50  # 静默异常（只写文件）
"""

import sys
import os
import io
import re
import threading
import traceback
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional


# ============================================================================
# 日志级别常量
# ============================================================================
class LogLevel:
    DEBUG = 10
    INFO = 20
    WARNING = 30
    ERROR = 40
    SILENT = 50  # 静默异常，只写入文件
    
    @staticmethod
    def name(level: int) -> str:
        names = {10: "DEBUG", 20: "INFO", 30: "WARNING", 40: "ERROR", 50: "SILENT"}
        return names.get(level, "UNKNOWN")


class TeeStream(io.TextIOBase):
    """将输出同时写入多个流（终端 + 文件）"""
    
    def __init__(self, *targets):
        super().__init__()
        self._targets = [t for t in targets if t]
    
    def write(self, data):
        for target in self._targets:
            try:
                target.write(data)
                # TeeStream 只是转发，不会触发目标文件的自动行缓冲
                # 因此需要在检测到换行时手动 flush，模拟行缓冲行为
                # 这样 print() 等走 stdout 的输出也能及时写入日志文件
                if '\n' in data:
                    target.flush()
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
    - 支持日志级别过滤
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
        
        # 日志级别（默认 WARNING，减少日志量）
        self.min_level = LogLevel.WARNING
        # 控制台最低级别（与文件级别保持一致）
        self.console_min_level = LogLevel.WARNING
        
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
            print("[WARN] [Logger] 日志功能已禁用")
            return
        
        if self._ready:
            print("[WARN] [Logger] 日志系统已经初始化")
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
            self.info(f"[OK] [Logger] 日志系统启动成功，日志文件：{log_path}")
            
        except Exception as e:
            print(f"[ERROR] [Logger] 无法创建日志文件: {e}")
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
日志级别: {LogLevel.name(self.min_level)} (文件) / {LogLevel.name(self.console_min_level)} (控制台)
{'=' * 80}
"""
        self.log_file.write(header)
        self.log_file.flush()
    
    def _log(self, level: int, message: str, module: str = ""):
        """
        写入日志
        
        Args:
            level: 日志级别（LogLevel.DEBUG/INFO/WARNING/ERROR）
            message: 日志内容
            module: 模块名（可选，用于追踪来源）
        """
        if not self.enabled:
            return
        
        # 检查日志级别
        if level < self.min_level:
            return
        
        timestamp = datetime.now().strftime("%H:%M:%S")
        level_name = LogLevel.name(level)
        
        # 构建日志行
        if module:
            log_line = f"[{timestamp}] [{level_name}] [{module}] {message}"
        else:
            log_line = f"[{timestamp}] [{level_name}] {message}"
        
        # 写入文件（总是写入，如果文件可用）
        if self.log_file and self._ready:
            try:
                self.log_file.write(log_line + "\n")
                # buffering=1（行缓冲）：写入 \n 时自动 flush，无需手动调用
                # 仅 ERROR 及以上才强制 flush，确保严重错误不会因崩溃而丢失
                if level >= LogLevel.ERROR:
                    self.log_file.flush()
            except Exception:
                pass
        
        # 输出到控制台（根据控制台级别过滤）
        # stdout 本身是行缓冲，写入 \n 后无需额外 flush
        if level >= self.console_min_level:
            try:
                self._original_stdout.write(log_line + "\n")
            except Exception:
                pass
    
    def debug(self, message: str, module: str = ""):
        """记录调试日志（开发时使用，生产环境可关闭）"""
        self._log(LogLevel.DEBUG, message, module)
    
    def info(self, message: str, module: str = ""):
        """记录信息日志（普通状态信息）"""
        self._log(LogLevel.INFO, message, module)
    
    def warning(self, message: str, module: str = ""):
        """记录警告日志（潜在问题）"""
        self._log(LogLevel.WARNING, message, module)
    
    def error(self, message: str, module: str = ""):
        """记录错误日志（功能异常）"""
        self._log(LogLevel.ERROR, message, module)
    
    def set_level(self, level: int):
        """
        设置文件日志最低级别
        
        Args:
            level: LogLevel.DEBUG/INFO/WARNING/ERROR
        """
        self.min_level = level
        self.info(f"日志级别已设置为: {LogLevel.name(level)}", "Logger")
    
    def set_console_level(self, level: int):
        """
        设置控制台日志最低级别（减少控制台噪音）
        
        Args:
            level: LogLevel.DEBUG/INFO/WARNING/ERROR
        """
        self.console_min_level = level
    
    def exception(self, e: Exception, context: str = "", silent: bool = True):
        """
        记录异常信息（替代 except: pass）
        
        Args:
            e: 异常对象
            context: 上下文描述（例如："断开信号连接时"）
            silent: 是否静默处理（True=只记录不打印到控制台，False=同时打印）
        """
        if not self.enabled:
            return
        
        # 获取异常信息
        exc_type = type(e).__name__
        exc_msg = str(e)
        
        # 构建日志消息
        if context:
            log_msg = f"[静默异常] {context}: {exc_type}: {exc_msg}"
        else:
            log_msg = f"[静默异常] {exc_type}: {exc_msg}"
        
        # 如果是静默模式，只写入文件不打印到控制台
        if silent and self.log_file and self._ready:
            timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
            log_line = f"[{timestamp}] [SILENT] {log_msg}\n"
            try:
                self.log_file.write(log_line)
                self.log_file.flush()
            except Exception:
                pass  # 写日志本身失败，忽略
        else:
            # 非静默模式，正常输出
            self._log(LogLevel.ERROR, log_msg)
    
    def exception_with_traceback(self, e: Exception, context: str = ""):
        """
        记录异常信息（包含完整堆栈，用于调试严重错误）
        
        Args:
            e: 异常对象
            context: 上下文描述
        """
        if not self.enabled:
            return
        
        exc_type = type(e).__name__
        exc_msg = str(e)
        tb = traceback.format_exc()
        
        if context:
            log_msg = f"[异常] {context}: {exc_type}: {exc_msg}\n{tb}"
        else:
            log_msg = f"[异常] {exc_type}: {exc_msg}\n{tb}"
        
        self._log(LogLevel.ERROR, log_msg)

    def set_enabled(self, enabled: bool):
        """
        动态开启/关闭日志
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
        print(f"[OK] [Logger] 日志目录已设置为: {log_dir}")
    
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
# 全局接口
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

    """
    logger = get_logger()
    
    if config_manager:
        # 从配置读取设置
        enabled = config_manager.get_log_enabled()
        log_dir = config_manager.get_log_dir()
        
        # 读取日志等级
        log_level_str = config_manager.get_log_level()
        level_map = {
            "DEBUG": LogLevel.DEBUG,
            "INFO": LogLevel.INFO,
            "WARNING": LogLevel.WARNING,
            "ERROR": LogLevel.ERROR
        }
        log_level = level_map.get(log_level_str, LogLevel.WARNING)
        
        # 读取日志保留天数
        retention_days = config_manager.get_log_retention_days()
    else:
        # 使用默认设置
        enabled = True
        log_dir = str(Path.home() / "AppData" / "Local" / "Jietuba" / "Logs")
        log_level = LogLevel.WARNING
        retention_days = 7
    
    logger.setup(enabled=enabled, log_dir=log_dir)
    
    # 设置日志等级
    logger.set_level(log_level)
    logger.set_console_level(log_level)
    
    # 清理过期日志文件
    if retention_days > 0:
        cleanup_old_logs(log_dir, retention_days)


def cleanup_old_logs(log_dir: str, retention_days: int):
    """
    清理过期的日志文件
    
    Args:
        log_dir: 日志目录路径
        retention_days: 保留天数（0表示永久保留）
    """
    if retention_days <= 0:
        return
    
    try:
        log_path = Path(log_dir)
        if not log_path.exists():
            return
        
        # 计算截止日期
        cutoff_date = datetime.now() - timedelta(days=retention_days)
        
        # 日志文件名格式: runtime_YYYYMMDD.log
        pattern = re.compile(r'^runtime_(\d{8})\.log$')
        
        deleted_count = 0
        for file in log_path.iterdir():
            if not file.is_file():
                continue
            
            match = pattern.match(file.name)
            if not match:
                continue
            
            try:
                # 从文件名解析日期
                date_str = match.group(1)
                file_date = datetime.strptime(date_str, "%Y%m%d")
                
                # 如果文件日期早于或等于截止日期，删除它
                # 使用 <= 确保保留天数准确（例如设置6天就只保留6个文件）
                if file_date <= cutoff_date:
                    file.unlink()
                    deleted_count += 1
            except (ValueError, OSError):
                continue
        
        if deleted_count > 0:
            # 使用 print 而不是 log_info，因为此时日志系统可能还未完全初始化
            print(f"🗑️ [日志清理] 已删除 {deleted_count} 个过期日志文件（保留 {retention_days} 天）")
    
    except Exception as e:
        print(f"[WARN] [日志清理] 清理失败: {e}")


def log_exception(e: Exception, context: str = "", silent: bool = True):
    """
    记录静默异常（替代 except: pass 的全局便捷函数）
    
    Args:
        e: 异常对象
        context: 上下文描述（例如："断开信号连接时"）
        silent: 是否静默处理（True=只写入日志文件，False=同时打印到控制台）
    
    使用示例（替代 except: pass）：
        # 旧写法：
        try:
            signal.disconnect()
        except:
            pass
        
        # 新写法：
        try:
            signal.disconnect()
        except Exception as e:
            log_exception(e, "断开信号连接")
    """
    get_logger().exception(e, context, silent)


def log_exception_full(e: Exception, context: str = ""):
    """
    记录异常（包含完整堆栈，用于调试严重错误）
    
    Args:
        e: 异常对象
        context: 上下文描述
    
    使用示例：
        try:
            critical_operation()
        except Exception as e:
            log_exception_full(e, "关键操作失败")
            raise  # 可选：继续抛出
    """
    get_logger().exception_with_traceback(e, context)


# ============================================================================
# 便捷的全局日志函数（推荐使用）
# ============================================================================

def log_debug(message: str, module: str = ""):
    """
    记录调试日志（开发时使用，生产环境可关闭）
    
    Args:
        message: 日志内容
        module: 模块名（例如 "PinWindow", "ToolController"）
    
    示例：
        log_debug("开始处理图像", "ImageProcessor")
        log_debug(f"变量值: x={x}, y={y}")
    """
    logger = get_logger()
    if logger.min_level <= LogLevel.DEBUG:
        logger.debug(message, module)


def log_info(message: str, module: str = ""):
    """
    记录信息日志（普通状态信息）
    
    Args:
        message: 日志内容
        module: 模块名
    
    示例：
        log_info("截图窗口已创建", "ScreenshotWindow")
        log_info("[OK] 保存成功")
    """
    get_logger().info(message, module)


def log_warning(message: str, module: str = ""):
    """
    记录警告日志（潜在问题，但不影响功能）
    
    Args:
        message: 日志内容
        module: 模块名
    
    示例：
        log_warning("OCR 引擎未加载，跳过文字识别", "OCR")
        log_warning("[WARN] 配置文件不存在，使用默认值")
    """
    get_logger().warning(message, module)


def log_error(message: str, module: str = ""):
    """
    记录错误日志（功能异常）
    
    Args:
        message: 日志内容
        module: 模块名
    
    示例：
        log_error("截图失败", "CaptureService")
        log_error(f"[ERROR] 保存文件失败: {path}")
    """
    get_logger().error(message, module)


def set_log_level(level: int):
    """
    设置日志级别
    
    Args:
        level: LogLevel.DEBUG / INFO / WARNING / ERROR
    
    示例：
        from core.logger import LogLevel, set_log_level
        set_log_level(LogLevel.WARNING)  # 只显示警告和错误
    """
    get_logger().set_level(level)


def set_console_log_level(level: int):
    """
    设置控制台日志级别（减少控制台噪音）
    
    Args:
        level: LogLevel.DEBUG / INFO / WARNING / ERROR
    
    示例：
        set_console_log_level(LogLevel.WARNING)  # 控制台只显示警告和错误
    """
    get_logger().set_console_level(level)
 