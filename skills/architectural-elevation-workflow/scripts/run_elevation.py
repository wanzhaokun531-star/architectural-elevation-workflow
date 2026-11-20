"""Launch this skill's bundled runtime; do not load an external legacy copy."""
import runpy
from pathlib import Path

runtime = Path(__file__).resolve().parents[1] / 'runtime/elevation_runtime.py'
if not runtime.is_file():
    raise SystemExit('技能内运行代码缺失，请完整安装 architectural-elevation-workflow 文件夹：' + str(runtime))
runpy.run_path(str(runtime), run_name='__main__')
