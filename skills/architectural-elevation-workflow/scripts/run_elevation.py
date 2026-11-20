"""Launch the installed controlled elevation runtime, without legacy fallback."""
import runpy
import os
from pathlib import Path

if os.environ.get('ELEVATION_RUNTIME_ROOT'):
    runtime = Path(os.environ['ELEVATION_RUNTIME_ROOT']) / 'elevation_runtime.py'
else:
    parent = Path(__file__).resolve().parents[3]
    candidates = [parent/'runtime/elevation_runtime.py', parent/'elevation-runtime/elevation_runtime.py']
    runtime = next((p for p in candidates if p.is_file()), candidates[0])
if not runtime.is_file():
    raise SystemExit('正式立面运行入口缺失，请安装仓库 runtime 目录，或设置 ELEVATION_RUNTIME_ROOT：' + str(runtime))
runpy.run_path(str(runtime), run_name='__main__')
