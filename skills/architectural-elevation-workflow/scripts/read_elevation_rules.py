"""Resolve current elevation output rules and drawing-scale parameters."""
import argparse
import json
from math import isfinite, isclose
from pathlib import Path

SKILL = Path(__file__).resolve().parents[1]
DEFAULT_POLICY = SKILL / 'references' / '立面补充执行参数.json'
RESOURCE_PREFIX = 'skill://architectural-elevation-workflow/'


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def skill_resource(uri):
    if not uri.startswith(RESOURCE_PREFIX):
        raise ValueError(f'Unsupported elevation resource: {uri}')
    path = (SKILL / uri.removeprefix(RESOURCE_PREFIX)).resolve()
    if not path.is_relative_to(SKILL) or not path.is_file():
        raise ValueError(f'Missing or invalid elevation resource: {uri}')
    return path


def resolve_rules(policy_path, scale, mm_per_unit, roof_mode=None):
    if not all(isfinite(n) and n > 0 for n in (scale, mm_per_unit)):
        raise ValueError('Scale denominator and millimetres per unit must be positive and finite.')
    policy = read_json(policy_path)
    if policy['schema'] != 'bce.elevation-annotation-skill-policy.v1':
        raise ValueError('Unsupported elevation policy schema.')
    main = policy['mainLevels']
    connection = main['baselineConnection']
    if connection['target'] != 'secondSideVerticalDimension' or not connection['sameHeight']:
        raise ValueError('Main level must join the second side dimension at the same height.')
    paper_to_model = scale / mm_per_unit
    spacing = policy['sideSpacing']['relations']
    lanes = {step['to']: step['paperMM'] for step in spacing}
    outer_gap = lanes['firstOutermostBaseline']
    tip_gap = lanes['mainLevelTriangleTip']
    if not (isclose(tip_gap, connection['maintainTipOutwardFromFirstPaperMM'])
            and isclose(outer_gap + tip_gap, connection['referenceTipToConnectionPaperMM'])):
        raise ValueError('Main level baseline rule conflicts with side dimension spacing.')
    roof = policy['roofFill']
    mode = roof_mode or roof['defaultMode']
    resolved_roof = {**roof, 'mode': mode}
    if mode == 'curvedRoofTile':
        pattern_path = skill_resource(roof['patternResource'])
        sample_path = skill_resource(roof['sampleResource'])
        pattern = read_json(pattern_path)
        if pattern['name'] != roof['pattern'] or not pattern['lines']:
            raise ValueError('Roof fill has no matching editable pattern.')
        resolved_roof.update(patternDefinition=pattern['lines'], angleDegrees=pattern['angleDegrees'],
                             modelPatternScale=pattern['scalePaperMM'] * paper_to_model,
                             samplePath=str(sample_path))
    elif mode != 'none':
        raise ValueError(f'Unsupported roof fill mode: {mode}')
    return {
        'effectiveDate': policy['effectiveDate'],
        'policyPath': str(policy_path.resolve()),
        'scaleDenominator': scale, 'mmPerDrawingUnit': mm_per_unit,
        'mainLevels': {**main, 'tipToFirstDimensionModel': tip_gap * paper_to_model,
                       'tipToSecondDimensionModel': (outer_gap + tip_gap) * paper_to_model},
        'controlLines': policy['controlLines'],
        'componentLayers': policy['componentLayers'],
        'roofFill': resolved_roof,
        'verification': policy['verification'],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--policy', type=Path, default=DEFAULT_POLICY)
    parser.add_argument('--scale', type=float, default=100)
    parser.add_argument('--mm-per-unit', type=float, default=1)
    parser.add_argument('--roof-fill', choices=('curvedRoofTile', 'none'))
    args = parser.parse_args()
    try:
        result = resolve_rules(args.policy, args.scale, args.mm_per_unit, args.roof_fill)
    except (ValueError, KeyError, OSError, TypeError) as error:
        parser.exit(2, f'Invalid elevation rules: {error}\n')
    print(json.dumps(result, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
