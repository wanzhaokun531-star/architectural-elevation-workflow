"""The installed skill must own its code and all executable rule dependencies."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class BundleLayoutTests(unittest.TestCase):
    def test_runtime_uses_its_own_skill_and_policy(self):
        rt = load('bundle_runtime', ROOT/'elevation_runtime.py')
        self.assertEqual(rt.SKILL, ROOT.parent)
        self.assertEqual(rt.POLICY, ROOT/'config/立面标注Skill执行规则_20261106.json')
        self.assertTrue((rt.SKILL/'scripts/read_elevation_rules.py').is_file())

    def test_sealed_rules_stay_inside_the_single_skill(self):
        seal = load('bundle_seal', ROOT/'tools/seal_release.py')
        _, records = seal.inventory()
        self.assertTrue(records)
        for relative in records:
            target = (ROOT/relative).resolve()
            self.assertTrue(target.is_relative_to(ROOT.parent), relative)
        for name in ('common.md', 'elevation.md'):
            target=ROOT.parent/'references/annotation-rules'/name
            self.assertTrue(target.is_file())
            self.assertIn(target.resolve(), [(ROOT/r).resolve() for r in records])


if __name__ == '__main__':
    unittest.main()
