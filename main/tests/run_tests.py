import subprocess
import sys
import os

tests_dir = os.path.dirname(os.path.abspath(__file__))
main_dir = os.path.dirname(tests_dir)
project_root = os.path.dirname(main_dir)
python_exe = os.path.join(project_root, "venv311", "Scripts", "python.exe")

result = subprocess.run(
    [python_exe, "-m", "pytest", ".", "-v"],
    cwd=tests_dir,
)
sys.exit(result.returncode)
 