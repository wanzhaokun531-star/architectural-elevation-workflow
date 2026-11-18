from pathlib import Path
import sys,unittest,math
import ezdxf
SCRIPTS=Path(__file__).resolve().parents[1]/'scripts'
sys.path.insert(0,str(SCRIPTS))
from component_blocks import clean_segments,chain_paths,ComponentBlockExporter,module_dimension,world_segments

class Geometry(unittest.TestCase):
    def test_overlap_merges_but_real_gap_remains(self):
        a=clean_segments([((0,0),(10,0),'L'),((5,0),(20,0),'L'),((22,0),(25,0),'L'),((20,0),(0,0),'L')])
        self.assertEqual(len(a),2)
        self.assertEqual(sorted(round(math.dist(x,y),6) for x,y,l in a),[3,20])
    def test_layers_and_real_fraction_preserved(self):
        a=clean_segments([((73.5,25.688),(100,25.688),'A'),((73.5,25.688),(100,25.688),'B')])
        self.assertEqual(len(a),2)
        self.assertTrue(all(p[1]==25.688 for x,y,l in a for p in [x,y]))
    def test_closed_outline_becomes_one_path(self):
        a=[((0,0),(10,0),'L'),((10,0),(10,5),'L'),((10,5),(0,5),'L'),((0,5),(0,0),'L')]
        p=chain_paths(clean_segments(a));self.assertEqual(len(p),1);self.assertTrue(p[0]['closed'])
    def test_added_dimensions_only_use_explicit_module(self):
        self.assertEqual(module_dimension(158.4),160);self.assertEqual(module_dimension(118.8),120)

class Blocks(unittest.TestCase):
    def test_same_geometry_reuses_definition_and_changes_propagate(self):
        d=ezdxf.new('R2018');d.layers.new('A-WINDOW')
        e=ComponentBlockExporter(d)
        lines=[((0,0),(10,0),'A-WINDOW'),((10,0),(10,5),'A-WINDOW')]
        r=e.add('W1','window','S',(0,0),lines,{'width':10,'height':5},'SRC1')
        r2=e.add('W2','window','S',(100,0),[((a[0]+100,a[1]),(b[0]+100,b[1]),l) for a,b,l in lines],{'width':10,'height':5},'SRC2')
        self.assertEqual(r.dxf.name,r2.dxf.name)
        self.assertEqual(len(d.modelspace().query('INSERT')),2)
        before=world_segments(d)
        block=d.blocks[r.dxf.name]
        block.add_line((3,1),(4,1),dxfattribs={'layer':'0'})
        after=world_segments(d)
        self.assertEqual(len(after)-len(before),2)
    def test_visibility_variants_not_shared(self):
        d=ezdxf.new('R2018');d.layers.new('A-COLUMN');e=ComponentBlockExporter(d)
        r=e.add('C1','column','S',(0,0),[((0,0),(0,10),'A-COLUMN')],{'width':600,'height':10},'X')
        t=e.add('C2','column','S',(100,0),[((100,5),(100,10),'A-COLUMN')],{'width':600,'height':10},'Y')
        self.assertNotEqual(r.dxf.name,t.dxf.name)
    def test_different_design_sizes_do_not_share_partial_outline(self):
        d=ezdxf.new('R2018');d.layers.new('A-COLUMN');e=ComponentBlockExporter(d)
        lines=[((0,0),(0,10),'A-COLUMN')]
        a=e.add('A','column','S',(0,0),lines,{'width':600,'height':10},'A')
        b=e.add('B','column','S',(0,0),lines,{'width':720,'height':10},'B')
        self.assertNotEqual(a.dxf.name,b.dxf.name)
    def test_short_attached_corner_not_deleted(self):
        a=[((0,0),(100,0),'L'),((100,0),(100,5),'L'),((100,5),(0,5),'L')]
        p=chain_paths(clean_segments(a));self.assertEqual(sum(len(t['points'])-1+int(t['closed']) for t in p),3)
    def test_mixed_moulding_layers_survive_block_inheritance(self):
        from collections import Counter
        d=ezdxf.new('R2018');e=ComponentBlockExporter(d)
        lines=[((0,0),(10,0),'A-EAVE'),((10,0),(10,5),'A-EAVE'),((10,5),(0,5),'A-TRIM')]
        e.add('M1','moulding','S',(0,0),lines,{'height':5},'M')
        self.assertEqual(Counter(l for a,b,l in world_segments(d)),Counter({'A-EAVE':2,'A-TRIM':1}))

if __name__=='__main__':unittest.main()
