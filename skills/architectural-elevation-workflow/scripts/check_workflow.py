"""Check evidence links and ordering; do not substitute for native drawing review."""
import argparse
import hashlib
import json
from pathlib import Path

MODES = {
    'geometry': ('sources', 'geometry', 'shape-review', 'plan-review', 'reopen', 'drawing-review'),
    'annotations': ('sources', 'annotation-review', 'reopen', 'drawing-review'),
    'review': ('sources', 'drawing-review'),
}

def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(chunk)
    return result.hexdigest()

def check(data, base):
    base = Path(base)
    errors, pending = [], []
    mode = data.get('mode') if isinstance(data, dict) else None
    if not isinstance(mode, str) or mode not in MODES:
        return {'complete': False, 'recordValid': False, 'errors': ['未知任务mode'], 'pending': []}
    views = data.get('views')
    if not isinstance(views, list) or not views or any(not isinstance(v, str) or not v.strip() for v in views) or len(set(views)) != len(views):
        errors.append('views必须是按任务独立确定的非空且不重复视向清单')
        views = []
    def verify_file(row, label):
        if not isinstance(row, dict) or not isinstance(row.get('path'), str) or not row['path']:
            errors.append(label + '缺文件路径')
            return None
        path = (base / row['path']).resolve()
        try:
            actual = digest(path)
        except OSError:
            errors.append(label + '文件不可读取：' + str(path))
            return None
        if not isinstance(row.get('sha256'), str) or actual != row['sha256'].lower():
            errors.append(label + '指纹过期：' + str(path))
        return actual
    source_versions = {}
    sources = data.get('sourceFiles')
    if not isinstance(sources, list) or not sources:
        errors.append('缺sourceFiles')
    else:
        for row in sources:
            version = verify_file(row, '来源')
            if version:
                path = (base / row['path']).resolve()
                if path in source_versions:
                    errors.append('来源重复：' + str(path))
                source_versions[path] = version
    drawing_hash = verify_file(data.get('drawingFile'), '当前DWG')
    geometry_hash = verify_file(data.get('geometryFile'), '最终几何') if mode == 'geometry' else None
    steps = data.get('steps')
    if not isinstance(steps, list):
        steps = []
        errors.append('缺steps')
    ids = [row['id'] for row in steps if isinstance(row, dict) and isinstance(row.get('id'), str)]
    if len(set(ids)) != len(ids):
        errors.append('阶段ID重复；只登记各阶段的当前有效版本')
    expected = MODES[mode]
    if [key for key in ids if key in expected] != list(expected):
        errors.append('阶段缺失或顺序不符：' + ' → '.join(expected))
    for row in steps:
        if not isinstance(row, dict):
            errors.append('阶段记录必须为对象')
            continue
        key = row.get('id')
        if key not in expected:
            errors.append('未声明的阶段：' + str(key))
            continue
        status = row.get('status')
        if status not in ('passed', 'pending', 'failed'):
            errors.append(str(key) + '状态无效')
            continue
        if status != 'passed':
            pending.append({'stage': key, 'status': status, 'reason': row.get('reason', '')})
            if not row.get('reason'):
                errors.append(str(key) + '未完成但没有定位/原因')
            continue
        if key != 'sources':
            checked_views = row.get('views')
            if (not isinstance(checked_views, list) or not all(isinstance(v, str) for v in checked_views)
                    or len(checked_views) != len(views) or set(checked_views) != set(views)):
                errors.append(key + '视向覆盖与任务应有清单不符')
        if key in ('shape-review', 'plan-review', 'annotation-review') or (mode == 'review' and key == 'drawing-review'):
            consumed = row.get('sourceFiles')
            if not isinstance(consumed, list) or not consumed:
                errors.append(key + '缺本次回核实际采用的来源版本')
            else:
                for item in consumed:
                    if not isinstance(item, dict) or not isinstance(item.get('path'), str):
                        errors.append(key + '来源引用缺路径')
                        continue
                    path = (base / item['path']).resolve()
                    version = item.get('sha256')
                    if not isinstance(version, str) or source_versions.get(path) != version.lower():
                        errors.append(key + '使用了旧版或未登记的来源：' + str(path))
        evidence = row.get('evidenceFiles')
        if not isinstance(evidence, list) or not evidence:
            errors.append(str(key) + '缺少证据文件')
        else:
            for file in evidence:
                verify_file(file, str(key) + '证据')
        if key in ('geometry', 'shape-review', 'plan-review') and str(row.get('geometrySHA256', '')).lower() != geometry_hash:
            errors.append(str(key) + '未绑定当前最终几何；相关改动后应重新回核')
        if key in ('annotation-review', 'reopen', 'drawing-review') and str(row.get('drawingSHA256', '')).lower() != drawing_hash:
            errors.append(str(key) + '未绑定当前DWG')
    return {'recordValid': not errors, 'complete': not errors and not pending,
            'mode': mode, 'views': views, 'errors': errors, 'pending': pending,
            'scope': '仅核对记录、文件指纹、顺序和覆盖；passed的专业判断仍须来自实际原生及图面核验'}

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('record', type=Path)
    p.add_argument('--allow-pending', action='store_true', help='仅用于已允许的阶段/协调稿，仍显示未通过项')
    args = p.parse_args(argv)
    try:
        result = check(json.loads(args.record.read_text(encoding='utf-8-sig')), args.record.resolve().parent)
    except (OSError, ValueError, TypeError, KeyError) as error:
        result = {'recordValid': False, 'complete': False, 'errors': [str(error)], 'pending': []}
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['complete'] or (args.allow_pending and result['recordValid']) else 2

if __name__ == '__main__':
    raise SystemExit(main())
