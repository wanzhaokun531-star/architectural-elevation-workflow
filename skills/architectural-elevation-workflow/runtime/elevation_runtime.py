"""Controlled native elevation entry: prepare, build, run, verify, publish.

Input geometry is supplied by the source-review workflow. This entry owns the
native annotation build and load path; it does not infer an approved roof scheme.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

ROOT = Path(__file__).resolve().parent
RUNS = ROOT / 'runs'
SKILL = ROOT.parent
POLICY = ROOT / 'config/立面标注Skill执行规则_20261106.json'
CATEGORIES = ('sources', 'components', 'roof', 'windows', 'annotations',
              'visibleLines', 'layersAndBlocks', 'delivery')


class RuntimeErrorDetail(ValueError):
    pass


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.tmp')
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    temp.replace(path)


def require(ok, message):
    if not ok:
        raise RuntimeErrorDetail(message)


def checked_path(base, name):
    path = (Path(base) / name).resolve()
    require(path.is_relative_to(Path(base).resolve()), 'Path escapes controlled directory: ' + str(path))
    return path


def check_files(base, records):
    require(isinstance(records, dict) and records, 'Empty release/input file inventory')
    for name, sha in records.items():
        p = checked_path(base, name)
        require(p.is_file() and digest(p) == sha, 'Version mismatch: ' + str(p))


def release():
    info = read(ROOT / 'current.json')
    check_files(ROOT, info['files'])
    for path, sha in info['ruleFiles'].items():
        target = (ROOT / path).resolve()
        require(target.is_file() and digest(target) == sha, 'Rule version mismatch: ' + str(target))
    require(info['schema'] == 'elevation.runtime.release.v1', 'Unknown release schema')
    return info, digest(ROOT / 'current.json')


def rules():
    result = subprocess.run([sys.executable, '-B', '-X', 'utf8',
                             str(SKILL / 'scripts/read_elevation_rules.py'),
                             '--policy', str(POLICY),
                             '--scale', '100', '--mm-per-unit', '1'],
                            capture_output=True, text=True, encoding='utf-8', check=True,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return json.loads(result.stdout)


def placement_rows(text):
    rows = [line.split() for line in text.splitlines() if line.strip() and not line.startswith('#')]
    require(all(r[0] in ('VIEW', 'DIM', 'AXIS', 'LEVEL', 'OPENING_LEVEL', 'CONTROL') for r in rows),
            'Unknown placement record')
    return rows


def canonical_placement(text, locals_, policy):
    """Retain measured stations, set rule-driven lanes and semantic opening tags."""
    rows = placement_rows(text)
    views = {r[1]: r for r in rows if r[0] == 'VIEW'}
    require(len(views) == len([r for r in rows if r[0] == 'VIEW']), 'Duplicate view')
    require(views, 'No views')
    steps = read(policy['policyPath'])
    factor = policy['scaleDenominator'] / policy['mmPerDrawingUnit']
    # Relations are incremental, in the declared wall -> annotation order.
    side_positions, n = {}, 0.
    for r in steps['sideSpacing']['relations']:
        n += r['paperMM'] * factor
        side_positions[r['to']] = n
    bottom_positions, n = {}, 0.
    for r in steps['bottomSpacing']['relations']:
        n += r['paperMM'] * factor
        bottom_positions[r['to']] = n
    opening_keys = []
    for item in locals_:
        if item.get('role') in ('openingBottom', 'openingTop'):
            opening_keys.append((item['view'], *map(float, item['point'])))
    matched = set()
    for r in rows:
        v = views[r[1]]
        left, right, zero = float(v[4]), float(v[5]), float(v[2])
        if r[0] in ('LEVEL', 'OPENING_LEVEL'):
            x, y = float(r[4]), float(r[5])
            hit = [i for i, key in enumerate(opening_keys)
                   if key[0] == r[1] and abs(key[1]-x) < 1.e-4 and abs(key[2]-y) < 1.e-4]
            if hit:
                require(r[2].startswith('local-'), 'Opening mark incorrectly classified as main')
                r[0] = 'OPENING_LEVEL'
                matched.update(hit)
            if r[2] in ('left', 'right'):
                sign = -1 if r[2] == 'left' else 1
                edge = left if sign < 0 else right
                r[4] = str(edge + sign * side_positions['mainLevelTriangleTip'])
        if r[0] == 'DIM':
            if r[2].startswith(('left_', 'right_')):
                sign = -1 if r[2].startswith('left_') else 1
                edge = left if sign < 0 else right
                role = r[2].split('_', 1)[1]
                target = {'overall': 'firstOutermostBaseline', 'levels': 'secondBaseline',
                          'detail': 'thirdBaseline'}[role]
                r[4] = str(edge + sign * side_positions['visibleExtensionStart'])
                r[5] = str(edge + sign * side_positions[target])
                r[6] = 'PUB_DIM' if role == 'detail' else 'AXIS'
            elif r[2].startswith('bottom_'):
                floor = float(v[3])
                r[4] = str(floor - bottom_positions['visibleExtensionStart'])
                target = 'firstOutermostBaseline' if r[2] == 'bottom_overall' else 'secondBaseline'
                r[5] = str(floor - bottom_positions[target])
                r[6] = 'AXIS'
        if r[0] == 'AXIS':
            r[5] = str(float(v[3]) - bottom_positions['axisCircleCenter'])
            r[6] = str(float(v[3]) - bottom_positions['visibleExtensionStart'])
        if r[0] == 'CONTROL' and abs(float(r[2])-float(r[4])) < 1.e-7:
            r[3] = str(float(v[3]) - bottom_positions['visibleExtensionStart'])
    require(len(matched) == len(opening_keys), 'Local opening measure has no matching native mark')
    require(all(r[0] != 'OPENING_LEVEL' or r[2].startswith('local-') for r in rows),
            'Invalid opening mark side')
    return '\n'.join(' '.join(r) for r in rows) + '\n'


def project_file(name):
    require(name.replace('-', '').replace('_', '').isalnum(), 'Invalid project name')
    return ROOT / 'projects' / (name + '.json')


def validate_project(config):
    base = Path(config['inputDirectory']).resolve()
    check_files(base, config['inputs'])
    for item in config['sources']:
        require(digest(item['path']) == item['sha256'], 'Source changed: ' + item['path'])
    require(digest(config['planDwg']) == config['planDwgSHA256'], 'Frozen native plan changed')
    require(config.get('roofSelection'), 'Missing roof source selection')
    require(config.get('datum'), 'Missing datum status')
    return base


def prepare(name):
    info, release_sha = release()
    config_path = project_file(name)
    require(config_path.relative_to(ROOT).as_posix() in info['files'],
            'Project configuration is not part of the formal release: ' + name)
    config = read(config_path)
    base = validate_project(config)
    nonce = uuid.uuid4().hex
    session = RUNS / ('elevation-runtime-' + name + '-' + nonce[:12])
    session.mkdir(parents=True)
    for folder in ('case', 'native', 'native-axis', 'native-dim'):
        (session / folder).mkdir()
    policy = rules()
    write(session / 'resolved-rules.json', policy)
    for folder in ('native', 'native-axis', 'native-dim', 'shared'):
        for src in (ROOT / folder).rglob('*'):
            if src.is_file():
                target = session / src.relative_to(ROOT)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, target)
    geometry = Path(config['geometryDwg'])
    require(digest(geometry) == config['geometryDwgSHA256'], 'Geometry DWG version mismatch')
    for n in ('source.dwg', 'control.dwg'):
        shutil.copy2(geometry, session / n)
    shutil.copy2(config['planDwg'], session / 'source-plan.dwg')
    for n, fp in config['donors'].items():
        require(digest(fp['path']) == fp['sha256'], 'Native prototype changed: ' + n)
        shutil.copy2(fp['path'], session / n)
    local = read(base / 'local-levels.json')
    text = canonical_placement((base / 'placement.tsv').read_text('ascii'), local, policy)
    (session / 'placement.tsv').write_text(text, encoding='ascii')
    write(session / 'local-levels.json', local)
    rows = placement_rows(text)
    counts = Counter(r[0] for r in rows)
    values = dict(views=counts['VIEW'], dimensions=counts['DIM'], axes=counts['AXIS'],
                  levels=counts['LEVEL']+counts['OPENING_LEVEL'], roots=config['geometryRoots'])
    header = ['#pragma once', 'namespace eruntime {',
              f'inline constexpr const char* releaseSHA="{release_sha}";',
              f'inline constexpr const char* releaseVersion="{info["version"]}";']
    header += [f'inline constexpr std::size_t {k}={v};' for k, v in values.items()]
    header += ['}', '']
    (session / 'native/RuntimeVersion.h').write_text('\n'.join(header), encoding='utf-8')
    (session / 'native/MFInputBinding.h').write_text(
        '#pragma once\nnamespace mfinput {inline constexpr const wchar_t* paths[]{LR"cad('
        + str(session / 'source.dwg') + ')cad"}; inline constexpr const char* hashes[]{"'
        + digest(session / 'source.dwg').upper() + '"};}\n', encoding='utf-8')
    plot = session / 'native/MFPlot.inc'
    text = plot.read_text('utf-8-sig').replace('晏府-', '立面-').replace('Yanfu_Elevation.ctb', config['plotStyleName'])
    plot.write_text(text, encoding='utf-8-sig')
    native_files = {str(p.relative_to(session)).replace('\\', '/'): digest(p)
                    for p in session.rglob('*') if p.is_file() and p.suffix in ('.cpp', '.h', '.inc', '.vcxproj', '.cmd')}
    runtime_inputs = {str(p.relative_to(session)).replace('\\', '/'): digest(p)
                      for p in session.iterdir() if p.is_file()}
    record = dict(schema='elevation.runtime.session.v1', project=name, nonce=nonce,
                  releaseSHA256=release_sha, version=info['version'],
                  projectFile=str(config_path), projectSHA256=digest(config_path),
                  nativeFiles=native_files, inputFiles=runtime_inputs, counts=dict(counts),
                  pending=config.get('pending', []), geometryRebuilt=False)
    write(session / 'session.json', record)
    return session


def check_session(session):
    session = Path(session).resolve()
    require(session.parent == RUNS.resolve() and session.name.startswith('elevation-runtime-'),
            'Session is outside formal runs')
    _, sha = release()
    data = read(session / 'session.json')
    require(data['releaseSHA256'] == sha, 'Session uses a retired release')
    require(digest(data['projectFile']) == data['projectSHA256'], 'Project config changed')
    validate_project(read(data['projectFile']))
    check_files(session, data['nativeFiles'])
    check_files(session, data['inputFiles'])
    return session, data


def build(session):
    session, data = check_session(session)
    with (session / 'build.log').open('wb') as log:
        result = subprocess.run(['cmd.exe', '/d', '/c', 'build.cmd'], cwd=session / 'native',
                                stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    require(result.returncode == 0, 'Native build failed: ' + str(session / 'build.log'))
    check_session(session)
    module = session / 'native/out/ZhaoConstruction.arx'
    require(module.is_file(), 'Build produced no ARX')
    write(session / 'build.json', dict(releaseSHA256=data['releaseSHA256'],
                                     module=str(module), moduleSHA256=digest(module),
                                     nativeFiles=data['nativeFiles']))
    return module


def check_build(session):
    session, data = check_session(session)
    built = read(session / 'build.json')
    require(built['releaseSHA256'] == data['releaseSHA256'] and built['nativeFiles'] == data['nativeFiles'],
            'Build receipt is from different source')
    require(digest(built['module']) == built['moduleSHA256'], 'Built module was replaced')
    return session, data, built


def run_native(session):
    session, data, built = check_build(session)
    require(not (session / 'loaded-module.json').exists(), 'Session already executed; prepare a fresh run')
    config = read(data['projectFile'])
    style = Path(config['plotStyleSource'])
    require(digest(style) == config['plotStyleSHA256'], 'Plot style changed')
    require(digest(config['installedPlotStyle']) == config['plotStyleSHA256'],
            'Installed plot style differs from the configured source')
    module = Path(config['trustedModuleDirectory']) / ('ElevationRuntime_' + data['nonce'] + '.arx')
    require(not module.exists(), 'Unique runtime module already exists')
    shutil.copy2(built['module'], module)
    require(digest(module) == built['moduleSHA256'], 'Copied module differs from built module')
    ini = module.with_suffix('.ini')
    ini.write_text('[job]\nroot=' + str(session) + '\nnonce=' + data['nonce']
                   + '\nrelease=' + data['releaseSHA256'] + '\nmodule=' + str(module)
                   + '\nmoduleSHA=' + built['moduleSHA256'].upper()
                   + '\nplacementSHA=' + digest(session / 'placement.tsv').upper() + '\n', encoding='utf-16')
    script = session / 'boot.scr'
    script.write_bytes(('\n'.join(['(setvar "FILEDIA" 0)', '_.OPEN',
                       '"'+(session/'control.dwg').as_posix()+'"', '_.ARX', '_L',
                       '"'+module.as_posix()+'"', ''])).encode('gb18030'))
    write(session / 'launch.json', dict(module=str(module), moduleSHA256=built['moduleSHA256'],
                                        releaseSHA256=data['releaseSHA256']))
    cmd = ['powershell.exe', '-NoProfile', '-File', config['isolatedHost'],
           '-BootScriptPath', str(script), '-RunDirectory', str(session), '-Nonce', data['nonce'],
           '-Template', config['template'], '-TimeoutSeconds', '600', '-CloseOwnStartupTips']
    if config.get('acadExecutable'):
        cmd += ['-AcadPath', config['acadExecutable']]
    if config.get('cadProfile'):
        cmd += ['-Profile', config['cadProfile']]
    with (session / 'host.log').open('wb') as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    write(session / 'host-result.json', dict(exitCode=result.returncode))
    require(result.returncode == 0, 'Isolated CAD host failed: ' + str(session / 'host.log'))
    return verify(session)


def verify(session):
    session, data, built = check_build(session)
    loaded = read(session / 'loaded-module.json')
    launch = read(session / 'launch.json')
    require(loaded['release'] == data['releaseSHA256'], 'CAD loaded another release')
    require(loaded['moduleSHA'].lower() == built['moduleSHA256'], 'CAD loaded another binary')
    require(Path(loaded['module']).resolve() == Path(launch['module']).resolve(), 'CAD load path differs')
    require(loaded['placementSHA'].lower() == digest(session/'placement.tsv'), 'CAD read stale placement')
    require(read(session/'host-result.json')['exitCode'] == 0, 'CAD host did not finish normally')
    require((session/'done.txt').read_text().strip() == 'passed', 'Native run did not pass')
    require(not (session/'failed.txt').exists(), 'Native failure is present')
    native = read(session/'readback.json')
    require(native.get('status') == 'passed', 'Native saved/reopen checks did not pass')
    require(native.get('openingLevelsWithoutLeader') == data['counts'].get('OPENING_LEVEL', 0),
            'Saved opening leader-free coverage differs')
    drawing = session/'case/construction.dwg'
    require(drawing.is_file(), 'Native output DWG is missing')
    require(digest(drawing).upper() == native['SHA256'].upper(),
            'Output DWG changed after native save/reopen; rerun native verification')
    require(digest(session/'source.dwg').upper() == native['sourceSHA256'].upper(),
            'Native readback belongs to another source DWG')
    result = dict(runtimeVerified=True, release=data['version'], drawing=str(drawing),
                  drawingSHA256=digest(drawing), native=native, pending=data['pending'],
                  constructionReviewComplete=False,
                  scope='Native code/build/load identity and native save/reopen; source/shape and final drawing review remain separate.')
    write(session/'verification.json', result)
    return result


def publish(session, review_file, target):
    result = verify(session)
    review = read(review_file)
    require(review.get('drawingSHA256') == result['drawingSHA256'], 'Final review is for another DWG')
    require(set(review.get('categories', {})) == set(CATEGORIES), 'Final review needs all eight categories')
    for key, row in review['categories'].items():
        require(row.get('status') in ('passed', 'notApplicable') and row.get('basis'), 'Unresolved final category: ' + key)
        evidence = Path(row.get('evidence', ''))
        require(evidence.is_file() and digest(evidence) == row.get('evidenceSHA256'), 'Missing/changed final evidence: '+key)
    require(not review.get('unresolved'), 'Final review still has unresolved items')
    target = Path(target)
    require(target.suffix.lower() == '.dwg' and not target.exists(), 'Use a new DWG output path')
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(result['drawing'], target)
    require(digest(target) == result['drawingSHA256'], 'Published DWG differs')
    write(Path(session)/'publication.json', dict(path=str(target), sha256=digest(target), reviewSHA256=digest(review_file)))
    return dict(published=str(target), sha256=digest(target))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inspect', 'prepare', 'build', 'run', 'verify', 'publish'))
    parser.add_argument('--project')
    parser.add_argument('--session', type=Path)
    parser.add_argument('--review', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    try:
        if args.action == 'inspect':
            info, sha = release()
            result = dict(version=info['version'], releaseSHA256=sha,
                          projects=[p.stem for p in (ROOT/'projects').glob('*.json')])
        elif args.action == 'prepare':
            require(args.project, 'prepare needs --project')
            result = dict(session=str(prepare(args.project)))
        else:
            require(args.session, 'This action needs --session')
            if args.action == 'build':
                result = dict(module=str(build(args.session)))
            elif args.action == 'run':
                result = run_native(args.session)
            elif args.action == 'verify':
                result = verify(args.session)
            else:
                require(args.review and args.output, 'publish needs --review and --output')
                result = publish(args.session, args.review, args.output)
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(json.dumps(dict(passed=False, error=str(error)), ensure_ascii=False, indent=2))
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
