"""Regression tests for actual stale-code and stale-output failure mechanisms."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('elevation_runtime', ROOT/'elevation_runtime.py')
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)


class RuntimeTests(unittest.TestCase):
    def test_current_three_project_placements(self):
        if not all((ROOT/'projects'/f'{name}.json').is_file() for name in ('shaofu','yanfu','tianfu')):
            self.skipTest('Private project inputs are not included in the source repository')
        policy = rt.rules()
        for name in ('shaofu', 'yanfu', 'tianfu'):
            config = rt.read(ROOT/'projects'/f'{name}.json')
            base = rt.validate_project(config)
            local = rt.read(base/'local-levels.json')
            source = (base/'placement.tsv').read_text('ascii')
            rows = rt.placement_rows(rt.canonical_placement(source, local, policy))
            openings = [r for r in rows if r[0] == 'OPENING_LEVEL']
            self.assertEqual(len(openings), len([q for q in local if q.get('role') in ('openingBottom','openingTop')]))
            for row in rows:
                if row[0] == 'DIM':
                    self.assertEqual(row[6], 'PUB_DIM' if row[2].endswith('_detail') else 'AXIS')
            # Source measured stations must survive rule-driven layout changes.
            before = [r[7:] for r in rt.placement_rows(source) if r[0] == 'DIM']
            after = [r[7:] for r in rows if r[0] == 'DIM']
            self.assertEqual(before, after)
            original_axes = [r for r in rt.placement_rows(source) if r[0] == 'AXIS']
            actual_axes = [r for r in rows if r[0] == 'AXIS']
            views = {r[1]:r for r in rows if r[0] == 'VIEW'}
            for old, new in zip(original_axes, actual_axes):
                self.assertEqual(old[3:5], new[3:5])  # Actual axis station and horizontal offset.
                self.assertEqual(old[7:], new[7:])  # Keep the source-native axis handle.
                self.assertEqual(float(new[5]), float(views[new[1]][3])-4000.)
                self.assertEqual(float(new[6]), float(views[new[1]][3])-800.)
                self.assertGreater(float(new[6]), float(new[5])+400.)
            for row in rows:
                if row[0]=='CONTROL' and abs(float(row[2])-float(row[4]))<1.e-7:
                    self.assertEqual(float(row[3]),float(views[row[1]][3])-800.)

    def test_public_layout_fixture_preserves_stations_and_connects_guides(self):
        policy = rt.rules()
        source = '\n'.join([
            'VIEW front 0 -450 0 10000 5000 -6000',
            'DIM front bottom_overall 0 -800 -3200 AXIS 2 0 10000',
            'DIM front left_detail 1 -1500 -2400 PUB_DIM 3 -450 0 600',
            'AXIS front B 1000 1400 -4000 -800 A1',
            'LEVEL front left 0 -4600 0',
            'LEVEL front local-left 600 2000 600',
            'CONTROL front 0 -800 0 3000',
        ])
        local = [dict(view='front',role='openingBottom',point=[2000,600])]
        rows=rt.placement_rows(rt.canonical_placement(source,local,policy))
        by_type={r[0]:r for r in rows if r[0]!='DIM'}
        self.assertEqual(by_type['AXIS'][3:5],['1000','1400'])
        self.assertEqual(list(map(float,by_type['AXIS'][5:7])),[-4450.,-1250.])
        self.assertEqual(float(by_type['CONTROL'][3]),-1250.)
        self.assertEqual(by_type['OPENING_LEVEL'][3:],['600','2000','600'])
        dims=[r for r in rows if r[0]=='DIM']
        self.assertEqual(dims[0][7:],['2','0','10000'])
        self.assertEqual(dims[1][7:],['3','-450','0','600'])
        self.assertEqual(float(dims[0][5]),-3650.)
        self.assertEqual(float(dims[1][5]),-2400.)

    def test_missing_opening_measure_does_not_silently_skip(self):
        policy = rt.rules()
        source = 'VIEW front 0 -450 0 10000 5000 -5000\nLEVEL front local-left 600 100 600\n'
        local = [dict(view='front',role='openingBottom',point=[200,600])]
        with self.assertRaisesRegex(ValueError, 'no matching native mark'):
            rt.canonical_placement(source, local, policy)

    def test_changed_old_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)/'native.cpp';p.write_text('verified correction')
            inventory = {'native.cpp': rt.digest(p)}
            rt.check_files(tmp, inventory)
            p.write_text('old copied code')
            with self.assertRaisesRegex(ValueError, 'Version mismatch'):
                rt.check_files(tmp, inventory)

    def test_unregistered_project_cannot_select_another_execution_path(self):
        with patch.object(rt,'release',return_value=({'files':{}},'release')):
            with self.assertRaisesRegex(ValueError,'not part of the formal release'):
                rt.prepare('unregistered')

    def test_external_source_reference_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, 'escapes'):
                rt.checked_path(tmp, '../old-project/native.cpp')

    def test_changed_binary_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp);module=root/'module.arx';module.write_bytes(b'new binary')
            data=dict(releaseSHA256='r',nativeFiles={'native.cpp':'s'})
            rt.write(root/'build.json',dict(**data,module=str(module),moduleSHA256=rt.digest(module)))
            with patch.object(rt,'check_session',return_value=(root,data)):
                rt.check_build(root)
                module.write_bytes(b'old binary')
                with self.assertRaisesRegex(ValueError, 'replaced'):
                    rt.check_build(root)

    def test_incomplete_final_review_cannot_publish(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);review=root/'review.json'
            rt.write(review,dict(drawingSHA256='d',categories={'roof':{'status':'passed'}}))
            with patch.object(rt,'verify',return_value=dict(drawingSHA256='d')):
                with self.assertRaisesRegex(ValueError,'eight categories'):
                    rt.publish(root,review,root/'output.dwg')

    def test_output_changed_after_native_readback_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'case').mkdir()
            drawing=root/'case/construction.dwg';drawing.write_bytes(b'checked output')
            (root/'source.dwg').write_bytes(b'checked source')
            (root/'placement.tsv').write_text('checked placement')
            (root/'done.txt').write_text('passed')
            rt.write(root/'loaded-module.json',dict(release='r',moduleSHA='b',module=str(root/'module.arx'),
                     placementSHA=rt.digest(root/'placement.tsv')))
            rt.write(root/'launch.json',dict(module=str(root/'module.arx')))
            rt.write(root/'host-result.json',dict(exitCode=0))
            rt.write(root/'readback.json',dict(status='passed',openingLevelsWithoutLeader=1,
                     SHA256=rt.digest(drawing),sourceSHA256=rt.digest(root/'source.dwg')))
            data=dict(releaseSHA256='r',version='v',counts={'OPENING_LEVEL':1},pending=[])
            with patch.object(rt,'check_build',return_value=(root,data,{'moduleSHA256':'b'})):
                self.assertTrue(rt.verify(root)['runtimeVerified'])
                drawing.write_bytes(b'unchecked later output')
                with self.assertRaisesRegex(ValueError,'changed after native'):
                    rt.verify(root)


if __name__ == '__main__':
    unittest.main()
