"""Developer-only release update, after regression tests; never called by runtime."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SKILL = Path(os.environ.get('ELEVATION_SKILL_ROOT',
             str(ROOT.parent / 'skills/architectural-elevation-workflow'))).resolve()
POLICY = Path(os.environ.get('ELEVATION_POLICY_PATH',
              str(ROOT / 'config/立面标注Skill执行规则_20261106.json'))).resolve()


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def inventory():
    files = {'elevation_runtime.py': sha(ROOT/'elevation_runtime.py')}
    for folder in ('native', 'native-axis', 'native-dim', 'shared', 'projects', 'tools', 'tests', 'host', 'config', 'examples'):
        for p in sorted((ROOT/folder).rglob('*')):
            if p.is_file() and '__pycache__' not in p.parts:
                files[p.relative_to(ROOT).as_posix()] = sha(p)
    rules = [SKILL/'SKILL.md', *sorted((SKILL/'references').glob('*.md')),
             SKILL/'scripts/read_elevation_rules.py', SKILL/'scripts/check_workflow.py',
             SKILL/'scripts/run_elevation.py', SKILL/'references/立面补充执行参数.json',
             POLICY,
             SKILL.parent/'architectural-annotation-rules/references/common.md',
             SKILL.parent/'architectural-annotation-rules/references/elevation.md']
    return files, {Path(os.path.relpath(p,ROOT)).as_posix():sha(p) for p in rules}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--version', required=True)
    p.add_argument('--write', action='store_true')
    args = p.parse_args()
    subprocess.run([sys.executable,'-B','-X','utf8','-m','unittest','discover',
                    '-s',str(ROOT/'tests'),'-v'],check=True)
    files, rule_files = inventory()
    result = dict(schema='elevation.runtime.release.v1',version=args.version,
                  files=files,ruleFiles=rule_files)
    if args.write:
        (ROOT/'current.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print('Updated release:',args.version,sha(ROOT/'current.json'))
    else:
        print(json.dumps(result,ensure_ascii=False,indent=2))


if __name__ == '__main__':
    main()
