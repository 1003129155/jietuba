import ctypes
import sys
import os
import copy
from ctypes import Structure, byref, POINTER, c_int64, c_int32, c_float, c_ubyte, c_char, c_char_p
from PIL import Image
from contextlib import contextmanager
import tempfile
import shutil
import subprocess

# 使用项目统一的日志模块
try:
    from core.logger import log_info, log_warning, log_error, log_debug
except ImportError:
    # 如果独立运行（测试），使用简单的 print
    def log_info(msg, tag="windos_ocr"): print(f"[{tag}] {msg}")
    def log_warning(msg, tag="windos_ocr"): print(f"[{tag}] 警告: {msg}")
    def log_error(msg, tag="windos_ocr"): print(f"[{tag}] 错误: {msg}")
    def log_debug(msg, tag="windos_ocr"): print(f"[{tag}] [DEBUG] {msg}")

# 自动管理 DLL 文件：如果临时目录没有，则从 WindowsApps 复制
def _find_windowsapps_path():
    """
    查找 Windows ScreenSketch 的 SnippingTool 路径
    
    使用 PowerShell Get-AppxPackage 动态获取安装路径
    """
    try:
        result = subprocess.run(
            ['powershell', '-Command', 
             'Get-AppxPackage Microsoft.ScreenSketch | Select-Object -ExpandProperty InstallLocation'],
            capture_output=True,
            text=True,
            timeout=5,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0
        )
        
        if result.returncode == 0 and result.stdout.strip():
            install_location = result.stdout.strip()
            snipping_tool_path = os.path.join(install_location, "SnippingTool")
            
            if os.path.exists(snipping_tool_path):
                log_info(f"找到 ScreenSketch 路径: {snipping_tool_path}", "OCR")
                return snipping_tool_path
            else:
                log_warning(f"SnippingTool 子目录不存在: {snipping_tool_path}", "OCR")
        else:
            log_warning("PowerShell 未返回有效路径", "OCR")
            if result.stderr:
                log_debug(f"错误信息: {result.stderr.strip()}", "OCR")
                
    except subprocess.TimeoutExpired:
        log_warning("PowerShell 查询超时 (>5秒)", "OCR")
    except FileNotFoundError:
        log_warning("PowerShell 不可用 (系统可能不支持)", "OCR")
    except OSError as e:
        log_warning(f"PowerShell 执行失败: {e}", "OCR")
    
    log_error("无法找到 ScreenSketch 应用", "OCR")
    log_error("请从 Microsoft Store 安装 '截图工具 (Snipping Tool)' 应用", "OCR")
    return None

def _copy_files_from_source(source_path, temp_dir, required_files):
    """从指定源路径复制 DLL 文件到临时目录"""
    try:
        for filename in required_files:
            src = os.path.join(source_path, filename)
            dst = os.path.join(temp_dir, filename)
            
            if not os.path.exists(dst) and os.path.exists(src):
                file_size = os.path.getsize(src) / (1024 * 1024)  # MB
                log_info(f"正在复制 {filename} ({file_size:.1f} MB)...", "OCR")
                shutil.copy2(src, dst)
                log_info(f"{filename} 复制完成", "OCR")
        
        # 验证所有文件都已复制
        if all(os.path.exists(os.path.join(temp_dir, f)) for f in required_files):
            log_info("OCR 引擎文件初始化完成", "OCR")
            return True
    except (PermissionError, OSError) as e:
        log_error(f"文件复制失败 - {e}", "OCR")
    return False

def _get_config_dir():
    """获取 DLL 文件目录：自动从 WindowsApps 复制到临时目录"""
    temp_dir = os.path.join(tempfile.gettempdir(), 'Jietuba', 'oneocr_dlls')
    
    # 检查临时目录是否已有完整的文件
    required_files = ['oneocr.dll', 'oneocr.onemodel', 'onnxruntime.dll']
    all_exist = all(os.path.exists(os.path.join(temp_dir, f)) for f in required_files)
    
    if all_exist:
        return temp_dir
    
    # 创建临时目录
    os.makedirs(temp_dir, exist_ok=True)
    log_info(f"正在初始化 OCR 引擎文件到: {temp_dir}", "OCR")
    
    # 从 WindowsApps 复制文件
    source_path = _find_windowsapps_path()
    if source_path:
        log_info(f"从 WindowsApps 复制文件: {source_path}", "OCR")
        if _copy_files_from_source(source_path, temp_dir, required_files):
            return temp_dir
    else:
        log_warning("未找到 WindowsApps 路径", "OCR")
    
    # 如果失败，返回当前目录（用户可以手动放置文件）
    log_warning("未能从 WindowsApps 复制文件", "OCR")
    log_warning(f"可手动将 OCR 文件放置到: {temp_dir}", "OCR")
    log_warning("或将文件放置到当前目录", "OCR")
    return os.path.dirname(os.path.abspath(__file__))

# 使用函数封装,避免 global 声明问题
def _get_or_init_config_dir():
    """获取或初始化 CONFIG_DIR"""
    if not hasattr(_get_or_init_config_dir, '_cached_dir'):
        _get_or_init_config_dir._cached_dir = _get_config_dir()
    return _get_or_init_config_dir._cached_dir

MODEL_NAME = 'oneocr.onemodel'
DLL_NAME = 'oneocr.dll'
MODEL_KEY = b"kj)TGtrK>f]b[Piow.gU+nC@s\"\"\"\"\"\"4"

c_int64_p = POINTER(c_int64)
c_float_p = POINTER(c_float)
c_ubyte_p = POINTER(c_ubyte)

class ImageStructure(Structure):
    '''Image data structure'''
    _fields_ = [
        ('type', c_int32),
        ('width', c_int32),      # Image width in pixels
        ('height', c_int32),     # Image height in pixels
        ('_reserved', c_int32),
        ('step_size', c_int64),  # Bytes per row
        ('data_ptr', c_ubyte_p)  # Pointer to image data
    ]

class BoundingBox(Structure):
    '''Text bounding box coordinates'''
    _fields_ = [
        ('x1', c_float),
        ('y1', c_float),
        ('x2', c_float),
        ('y2', c_float),
        ('x3', c_float),
        ('y3', c_float),
        ('x4', c_float),
        ('y4', c_float)
    ]

BoundingBox_p = POINTER(BoundingBox)

# DLL 函数列表: (函数名, 参数类型, 返回类型, 是否可选)
DLL_FUNCTIONS = [
    ('CreateOcrInitOptions', [c_int64_p], c_int64, False),
    ('OcrInitOptionsSetUseModelDelayLoad', [c_int64, c_char], c_int64, False),
    ('CreateOcrPipeline', [c_char_p, c_char_p, c_int64, c_int64_p], c_int64, False),
    ('CreateOcrProcessOptions', [c_int64_p], c_int64, False),
    ('OcrProcessOptionsSetMaxRecognitionLineCount', [c_int64, c_int64], c_int64, True),  # 可选:旧版DLL可能没有
    ('RunOcrPipeline', [c_int64, POINTER(ImageStructure), c_int64, c_int64_p], c_int64, False),

    ('GetImageAngle', [c_int64, c_float_p], c_int64, False),
    ('GetOcrLineCount', [c_int64, c_int64_p], c_int64, False),
    ('GetOcrLine', [c_int64, c_int64, c_int64_p], c_int64, False),
    ('GetOcrLineContent', [c_int64, POINTER(c_char_p)], c_int64, False),
    ('GetOcrLineBoundingBox', [c_int64, POINTER(BoundingBox_p)], c_int64, False),
    ('GetOcrLineWordCount', [c_int64, c_int64_p], c_int64, False),
    ('GetOcrWord', [c_int64, c_int64, c_int64_p], c_int64, False),
    ('GetOcrWordContent', [c_int64, POINTER(c_char_p)], c_int64, False),
    ('GetOcrWordBoundingBox', [c_int64, POINTER(BoundingBox_p)], c_int64, False),
    ('GetOcrWordConfidence', [c_int64, c_float_p], c_int64, False),

    ('ReleaseOcrResult', [c_int64], None, False),
    ('ReleaseOcrInitOptions', [c_int64], None, False),
    ('ReleaseOcrPipeline', [c_int64], None, False),
    ('ReleaseOcrProcessOptions', [c_int64], None, False)
]

def bind_dll_functions(dll, functions):
    '''Dynamically bind function specifications to DLL methods'''
    for func_info in functions:
        if len(func_info) == 4:
            name, argtypes, restype, optional = func_info
        else:
            name, argtypes, restype = func_info
            optional = False
        
        try:
            func = getattr(dll, name)
            func.argtypes = argtypes
            func.restype = restype
        except AttributeError as e:
            if not optional:
                raise RuntimeError(f'Missing DLL function: {name}') from e
            else:
                log_warning(f"可选函数 {name} 不存在(旧版DLL),将跳过", "OCR")

# 启动时就初始化 DLL (复制文件)
try:
    CONFIG_DIR = _get_or_init_config_dir()
    
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    if hasattr(kernel32, 'SetDllDirectoryW'):
        kernel32.SetDllDirectoryW(CONFIG_DIR)

    dll_path = os.path.join(CONFIG_DIR, DLL_NAME)
    ocr_dll = ctypes.WinDLL(dll_path)
    bind_dll_functions(ocr_dll, DLL_FUNCTIONS)
    dll_init_error = None
except (OSError, RuntimeError) as e:
    dll_init_error = f'DLL initialization failed: {e}'
    ocr_dll = None

@contextmanager
def suppress_output():
    '''Suppress stdout/stderr'''
    devnull = os.open(os.devnull, os.O_WRONLY)
    original_stdout = os.dup(1)
    original_stderr = os.dup(2)
    os.dup2(devnull, 1)
    os.dup2(devnull, 2)
    try:
        yield
    finally:
        os.dup2(original_stdout, 1)
        os.dup2(original_stderr, 2)
        os.close(original_stdout)
        os.close(original_stderr)
        os.close(devnull)

class OcrEngine:    
    def __init__(self):
        # 检查 DLL 是否已加载
        if ocr_dll is None:
            raise RuntimeError(f'OCR DLL not available: {dll_init_error}')
        
        self.init_options = self._create_init_options()
        self.pipeline = self._create_pipeline()
        self.process_options = self._create_process_options()
        self.empty_result = {
                'text': '',
                'text_angle': None,
                'lines': []
            }

    def __del__(self):
        if ocr_dll is not None:
            try:
                ocr_dll.ReleaseOcrProcessOptions(self.process_options)
                ocr_dll.ReleaseOcrPipeline(self.pipeline)
                ocr_dll.ReleaseOcrInitOptions(self.init_options)
            except:
                pass  # 忽略释放时的错误

    def _create_init_options(self):
        init_options = c_int64()
        self._check_dll_result(
            ocr_dll.CreateOcrInitOptions(byref(init_options)),
            'Init options creation failed'
        )
        
        self._check_dll_result(
            ocr_dll.OcrInitOptionsSetUseModelDelayLoad(init_options, 0),
            'Model loading config failed'
        )
        return init_options

    def _create_pipeline(self):
        model_path = os.path.join(CONFIG_DIR, MODEL_NAME)
        model_buf = ctypes.create_string_buffer(model_path.encode())
        key_buf = ctypes.create_string_buffer(MODEL_KEY)

        pipeline = c_int64()
        # 移除 suppress_output() 以避免 Windows 上的文件句柄问题
        self._check_dll_result(
            ocr_dll.CreateOcrPipeline(
                model_buf,
                key_buf,
                self.init_options,
                byref(pipeline)
            ),
            'Pipeline creation failed'
        )
        return pipeline

    def _create_process_options(self):
        process_options = c_int64()
        self._check_dll_result(
            ocr_dll.CreateOcrProcessOptions(byref(process_options)),
            'Process options creation failed'
        )
        
        # 尝试设置最大识别行数(旧版DLL可能不支持此函数)
        if hasattr(ocr_dll, 'OcrProcessOptionsSetMaxRecognitionLineCount'):
            self._check_dll_result(
                ocr_dll.OcrProcessOptionsSetMaxRecognitionLineCount(
                    process_options, 1000),
                'Line count config failed'
            )
        return process_options

    def recognize_pil(self, image):
        '''Process PIL Image object'''
        if any(x < 50 or x > 10000 for x in image.size):
            result = copy.deepcopy(self.empty_result)
            result['error'] = 'Unsupported image size'
            return result

        if image.mode != 'RGBA':
            image = image.convert('RGBA')
        
        # Convert to BGRA format expected by DLL
        b, g, r, a = image.split()
        bgra_image = Image.merge('RGBA', (b, g, r, a))

        return self._process_image(
            cols=bgra_image.width,
            rows=bgra_image.height,
            step=bgra_image.width * 4,
            data=bgra_image.tobytes()
        )

    def _process_image(self, cols, rows, step, data):
        '''Create image structure'''
        if isinstance(data, bytes):
            data_ptr = (c_ubyte * len(data)).from_buffer_copy(data)
        else:
            data_ptr = ctypes.cast(ctypes.c_void_p(data), c_ubyte_p)
        
        img_struct = ImageStructure(
            type=3,
            width=cols,
            height=rows,
            _reserved=0,
            step_size=step,
            data_ptr=data_ptr
        )

        return self._perform_ocr(img_struct)

    def _perform_ocr(self, image_struct):
        '''Execute OCR pipeline and parse results'''
        ocr_result = c_int64()
        if ocr_dll.RunOcrPipeline(
                self.pipeline,
                byref(image_struct),
                self.process_options,
                byref(ocr_result)
            ) != 0:
            return self.empty_result

        parsed_result = self._parse_ocr_results(ocr_result)
        ocr_dll.ReleaseOcrResult(ocr_result)
        return parsed_result

    def _parse_ocr_results(self, ocr_result):
        '''Extract and format OCR results from DLL'''
        line_count = c_int64()
        if ocr_dll.GetOcrLineCount(ocr_result, byref(line_count)) != 0:
            return self.empty_result

        lines = self._get_lines(ocr_result, line_count)
        return {
            'text': '\n'.join(line['text'] for line in lines),
            'text_angle': self._get_text_angle(ocr_result),
            'lines': lines
        }

    def _get_text_angle(self, ocr_result):
        '''Extract text angle'''
        text_angle = c_float()
        if ocr_dll.GetImageAngle(ocr_result, byref(text_angle)) != 0:
            return None
        return text_angle.value

    def _get_lines(self, ocr_result, line_count):
        '''Extract individual text lines'''
        return [self._process_line(ocr_result, idx) for idx in range(line_count.value)]

    def _process_line(self, ocr_result, line_index):
        '''Process a single text line'''
        line_handle = c_int64()
        if ocr_dll.GetOcrLine(ocr_result, line_index, byref(line_handle)) != 0:
            return {
                'text': None,
                'bounding_rect': None,
                'words': []
            }

        return {
            'text': self._get_text(line_handle, ocr_dll.GetOcrLineContent),
            'bounding_rect': self._get_bounding_box(line_handle, ocr_dll.GetOcrLineBoundingBox),
            'words': self._get_words(line_handle)
        }

    def _get_words(self, line_handle):
        '''Extract words from a text line'''
        word_count = c_int64()
        if ocr_dll.GetOcrLineWordCount(line_handle, byref(word_count)) != 0:
            return []

        return [self._process_word(line_handle, idx) for idx in range(word_count.value)]

    def _process_word(self, line_handle, word_index):
        '''Process individual word'''
        word_handle = c_int64()
        if ocr_dll.GetOcrWord(line_handle, word_index, byref(word_handle)) != 0:
            return {
                'text': None,
                'bounding_rect': None,
                'confidence': None
            }

        return {
            'text': self._get_text(word_handle, ocr_dll.GetOcrWordContent),
            'bounding_rect': self._get_bounding_box(word_handle, ocr_dll.GetOcrWordBoundingBox),
            'confidence': self._get_word_confidence(word_handle)
        }

    def _get_text(self, handle, text_function):
        '''Extract text content from handle'''
        content = c_char_p()
        if text_function(handle, byref(content)) == 0:
            return content.value.decode('utf-8', errors='ignore')
        return None

    def _get_bounding_box(self, handle, bbox_function):
        '''Extract bounding box from handle'''
        bbox_ptr = BoundingBox_p()
        if bbox_function(handle, byref(bbox_ptr)) == 0 and bbox_ptr:
            bbox = bbox_ptr.contents
            return {
                'x1': bbox.x1,
                'y1': bbox.y1,
                'x2': bbox.x2,
                'y2': bbox.y2,
                'x3': bbox.x3,
                'y3': bbox.y3,
                'x4': bbox.x4,
                'y4': bbox.y4
            }
        return None

    def _get_word_confidence(self, word_handle):
        '''Extract confidence value from word handle'''
        confidence = c_float()
        if ocr_dll.GetOcrWordConfidence(word_handle, byref(confidence)) == 0:
            return confidence.value
        return None

    def _check_dll_result(self, result_code, error_message):
        if result_code != 0:
            raise RuntimeError(f'{error_message} (Code: {result_code})')


if __name__ == '__main__':  # 简单测试
    engine = OcrEngine()
    test_image_path = os.path.join(os.path.dirname(__file__), 'test_image.png')
    if os.path.exists(test_image_path):
        img = Image.open(test_image_path)
        result = engine.recognize_pil(img)
        print("OCR 结果:")
        print(result)
    else:
        print("未找到测试图像文件: test_image.png")
