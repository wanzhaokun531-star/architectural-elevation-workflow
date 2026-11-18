"""Component-local line cleanup and real editable CAD block export.

Requires ezdxf. Accepts already-visible, orthographically projected segments;
does not infer instances from layer names and never rounds source dimensions
to a design module. Call module_dimension only for approved added decoration.
"""
from collections import defaultdict,Counter
from decimal import Decimal,ROUND_HALF_UP
from math import atan2,cos,sin,hypot,pi
import hashlib,json,re

def module_dimension(value,step=5):
    if step<=0:raise ValueError('Module must be positive')
    return float((Decimal(str(value))/Decimal(str(step))).quantize(Decimal('1'),rounding=ROUND_HALF_UP)*Decimal(str(step)))

def point(p):return tuple(0. if abs(float(v))<.0000005 else round(float(v),6) for v in tuple(p)[:2])

def clean_segments(lines,gap_tolerance=.001):
    """Merge only collinear same-layer intervals. Preserve real gaps/short edges."""
    groups=[];buckets=defaultdict(list)
    for a,b,layer in lines:
        a,b=point(a),point(b);dx,dy=b[0]-a[0],b[1]-a[1];length=hypot(dx,dy)
        if length<.000001:continue
        theta=atan2(dy,dx)%pi
        if pi-theta<1e-10:theta=0.
        ux,uy=cos(theta),sin(theta);offset=-uy*a[0]+ux*a[1]
        ka,ko=round(theta/1e-8),round(offset/1e-4);found=None
        for da in [-1,0,1]:
            for do in [-1,0,1]:
                for i in buckets[(layer,ka+da,ko+do)]:
                    x,y,o,_=groups[i]
                    if abs(ux*y-uy*x)<1e-9 and max(abs(-y*p[0]+x*p[1]-o) for p in [a,b])<.000002:
                        found=i;break
                if found is not None:break
            if found is not None:break
        if found is None:
            found=len(groups);groups.append([ux,uy,offset,[]]);buckets[(layer,ka,ko)].append(found)
        x,y,o,intervals=groups[found]
        intervals.append(tuple(sorted((x*a[0]+y*a[1],x*b[0]+y*b[1]))))
    output=[]
    # Recover the corresponding layer from the index map, avoiding cross-layer union.
    layer_for={i:key[0] for key,ids in buckets.items() for i in ids}
    for i,(x,y,o,intervals) in enumerate(groups):
        intervals.sort();merged=[]
        for a,b in intervals:
            if merged and a<=merged[-1][1]+gap_tolerance:merged[-1]=(merged[-1][0],max(b,merged[-1][1]))
            else:merged.append((a,b))
        for a,b in merged:
            output.append((point((x*a-y*o,y*a+x*o)),point((x*b-y*o,y*b+x*o)),layer_for[i]))
    return output

def chain_paths(lines):
    """Join degree-two nodes. Branches and real gaps remain independently editable."""
    output=[];by_layer=defaultdict(list)
    for a,b,l in lines:by_layer[l].append((point(a),point(b)))
    for layer,edges in by_layer.items():
        adj=defaultdict(list)
        for i,(a,b) in enumerate(edges):adj[a].append(i);adj[b].append(i)
        used=set()
        def walk(node,i):
            pts=[node]
            while i not in used:
                used.add(i);a,b=edges[i];node=b if node==a else a;pts.append(node)
                remain=[j for j in adj[node] if j not in used]
                if len(adj[node])!=2 or not remain:break
                i=remain[0]
            closed=len(pts)>2 and pts[0]==pts[-1]
            if closed:pts.pop()
            output.append({'layer':layer,'points':pts,'closed':closed})
        for node,ids in adj.items():
            if len(ids)!=2:
                for i in ids:
                    if i not in used:walk(node,i)
        for i,(a,b) in enumerate(edges):
            if i not in used:walk(a,i)
    return output

def add_paths(layout,paths,layer_override=None):
    for p in paths:
        attrs={'layer':layer_override or p['layer'],'color':0 if layer_override=='0' else 256}
        if layer_override=='0':attrs['lineweight']=-2
        if len(p['points'])==2 and not p['closed']:layout.add_line(*p['points'],dxfattribs=attrs)
        else:layout.add_lwpolyline(p['points'],close=p['closed'],dxfattribs=attrs)

WINDOW_FRAME_LAYER='A-立面门窗'
WINDOW_CASING_LAYER='J-LM-立面线脚1-0.18'

PRIMARY={'window':WINDOW_FRAME_LAYER,'door':'A-DOOR','column':'A-COLUMN','moulding':'A-TRIM','railing':'A-RAIL','steps':'A-STEP','window_trim':WINDOW_CASING_LAYER,'column_detail':'A-DETAIL','corbel':'A-DETAIL'}
PREFIX={'window':'WIN','door':'DOOR','column':'COL','moulding':'MOULD','railing':'RAIL','steps':'STEP','window_trim':'WTRIM','column_detail':'CRELIEF','corbel':'CORBEL'}

def inheritance_layer(kind,lines):
    if kind=='window_trim':return lines[0][2]
    if kind!='moulding':return PRIMARY[kind]
    counts=Counter(l for a,b,l in lines);maximum=max(counts.values())
    choices=sorted(l for l,n in counts.items() if n==maximum)
    return PRIMARY[kind] if PRIMARY[kind] in choices else choices[0]

class ComponentBlockExporter:
    def __init__(self,doc):
        self.doc=doc;self.definitions={};self.instances=[];self.expected_segments=[];self.stats={'input_segments':0,'clean_segments':0,'block_geometry_entities':0}
        for app in ['ELEV_SOURCE','ELEV_COMPONENT']:
            if app not in doc.appids:doc.appids.new(app)
        for layer in PRIMARY.values():
            if layer not in doc.layers:doc.layers.new(layer)

    def _definition(self,kind,lines,parameters):
        canonical=sorted((l,*sorted((point(a),point(b)))) for a,b,l in lines)
        shape_parameters={k:v for k,v in parameters.items() if k not in {'floor','source_name','source_id','source_handle','instance_id','view','level','z0','z1','opening_instance_id','native_path'}}
        signature=hashlib.sha256(json.dumps([kind,shape_parameters,canonical],sort_keys=True,separators=(',',':')).encode()).hexdigest()
        if signature in self.definitions:return self.definitions[signature]
        sizes=[]
        for k in ['width','height']:
            if k in parameters:sizes.append(re.sub(r'[^0-9pm]','',f'{float(parameters[k]):g}'.replace('-','m').replace('.','p')))
        stem='EFB_'+PREFIX.get(kind,kind.upper())+('_'+'x'.join(sizes) if sizes else '')+'_'+signature[:8]
        name=stem;i=1
        while name in self.doc.blocks:i+=1;name=stem+'_'+str(i)
        block=self.doc.blocks.new(name,base_point=(0,0,0))
        nested_layers={}
        if kind=='door':nested_layers={'A-TRIM':'window_trim'}
        elif kind=='column':nested_layers={'A-DETAIL':'column_detail'}
        main=[]
        for layer,nested_kind in nested_layers.items():
            sub=[t for t in lines if t[2]==layer]
            if sub:
                subname=self._definition(nested_kind,sub,parameters)
                block.add_blockref(subname,(0,0),dxfattribs={'layer':layer,'color':256})
        main=[t for t in lines if t[2] not in nested_layers]
        inherited=inheritance_layer(kind,lines)
        for path in chain_paths(main):
            add_paths(block,[path],layer_override='0' if path['layer']==inherited else None)
        self.definitions[signature]=name
        self.stats['block_geometry_entities']+=len(block)
        return name

    def add(self,instance_id,kind,view,origin,lines,parameters=None,source_id=''):
        if kind == 'window':
            frame, casing = [], []
            casing_layers = {'A-TRIM', PRIMARY['window_trim'], WINDOW_CASING_LAYER}
            for start, end, layer in lines:
                if layer in casing_layers:
                    casing.append((start, end, PRIMARY['window_trim']))
                else:
                    frame_layer = PRIMARY['window'] if layer == 'A-WINDOW' else layer
                    frame.append((start, end, frame_layer))
            lines = frame
            if casing:
                # Keep both model-space references on the original opening datum.
                params = {**(parameters or {}), 'opening_instance_id': instance_id}
                frame_ref = self._add_component(instance_id, kind, view, origin, frame, params, source_id)
                casing_ref = self._add_component(instance_id + '-CASING', 'window_trim', view,
                                                 origin, casing, params, source_id)
                return frame_ref if frame_ref is not None else casing_ref
        return self._add_component(instance_id, kind, view, origin, lines, parameters, source_id)

    def _add_component(self,instance_id,kind,view,origin,lines,parameters=None,source_id=''):
        if not lines:return None
        parameters=parameters or {};origin=point(origin)
        local=[((a[0]-origin[0],a[1]-origin[1]),(b[0]-origin[0],b[1]-origin[1]),layer) for a,b,layer in lines]
        clean=clean_segments(local)
        if not clean:return None
        self.expected_segments.extend((point((a[0]+origin[0],a[1]+origin[1])),point((b[0]+origin[0],b[1]+origin[1])),l) for a,b,l in clean)
        self.stats['input_segments']+=len(lines);self.stats['clean_segments']+=len(clean)
        name=self._definition(kind,clean,parameters)
        insert_layer=inheritance_layer(kind,clean)
        insert=self.doc.modelspace().add_blockref(name,origin,dxfattribs={'layer':insert_layer})
        insert.set_xdata('ELEV_SOURCE',[(1000,instance_id[:240]),(1000,view)])
        meta=json.dumps({'id':instance_id,'source':source_id,'kind':kind,'view':view,'parameters':parameters,'ordinary_block':True},ensure_ascii=True,separators=(',',':'))
        insert.set_xdata('ELEV_COMPONENT',[(1000,meta[i:i+220]) for i in range(0,len(meta),220)])
        self.instances.append({'instance_id':instance_id,'kind':kind,'view':view,'block_name':name,'insert':origin,'insert_handle':insert.dxf.handle,'source_id':source_id,'parameters':parameters,'source_segments':len(lines),'clean_segments':len(clean),'visibility_variant':'different visible local geometry uses a different definition'})
        return insert

def world_segments(doc,entities=None):
    """Resolve nested INSERTs and polyline edges, with inherited layer0, in WCS."""
    result=[]
    def visit(e,parent='0'):
        layer=e.dxf.layer if e.dxf.layer!='0' else parent
        if e.dxftype()=='INSERT':
            for item in e.virtual_entities():visit(item,layer)
        elif e.dxftype()=='LINE':result.append((point(e.dxf.start),point(e.dxf.end),layer))
        elif e.dxftype()=='LWPOLYLINE':
            if any(abs(p[0])>1e-12 for p in e.get_points('b')):raise NotImplementedError('Curved bulge geometry requires analytic verification')
            pts=[point(p) for p in e.get_points('xy')]
            for a,b in zip(pts,pts[1:]+(pts[:1] if e.closed else [])):result.append((a,b,layer))
        elif e.dxftype() in ['ARC','CIRCLE','ELLIPSE','SPLINE']:
            raise NotImplementedError('Analytic curves require their own parameter/shape verification')
    for e in (doc.modelspace() if entities is None else entities):visit(e)
    return result
