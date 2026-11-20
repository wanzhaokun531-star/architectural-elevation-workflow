#include <acedCmdNF.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cstring>
#include <cstdint>
#include <Windows.h>
#include <acedads.h>
#include <dbgrip.h>
#include <actrans.h>
#include <dbdict.h>
#include <dbspfilt.h>
#include <dbpl.h>
#include <AcDbLMgr.h>
#include <AcPlPlotFactory.h>
#include <AcPlPlotEngine.h>
#include <AcPlPlotInfo.h>
#include <AcPlPlotInfoValidator.h>
#include <dbplotsettings.h>
#include <dbplotsetval.h>
#include <dblayout.h>
#include "MFInputBinding.h"
#include "Project222D9DimensionCommon.inc"
constexpr double drawingTolerance=1e-4;
constexpr double pi=3.14159265358979323846;
AcDbObjectId resolve(AcDbDatabase& db,const wchar_t* h){AcDbObjectId id;check(db.getAcDbObjectId(id,false,AcDbHandle(h)),"resolve bound 222 handle");return id;}
std::string symbol_name(AcDbObjectId id){if(id.isNull())return "null";auto value=opened<AcDbSymbolTableRecord>(id);AcString name;check(value->getName(name),"read resource name");return text(name.kACharPtr());}
struct Bounds{double x0{},y0{},x1{},y1{};bool valid{};void add(const AcGePoint3d& p){if(!valid){x0=x1=p.x;y0=y1=p.y;valid=true;}else{x0=(std::min)(x0,p.x);y0=(std::min)(y0,p.y);x1=(std::max)(x1,p.x);y1=(std::max)(y1,p.y);}}void add(const Bounds& b){if(b.valid){add(AcGePoint3d(b.x0,b.y0,0));add(AcGePoint3d(b.x1,b.y1,0));}}void entity(const AcDbEntity& e){AcDbExtents ex;check(e.getGeomExtents(ex),"actual output extent");add(ex.minPoint());add(ex.maxPoint());}Bounds padded(double p)const{auto b=*this;b.x0-=p;b.y0-=p;b.x1+=p;b.y1+=p;return b;}std::string json()const{return array({number(x0),number(y0),number(x1),number(y1)});}};
struct PlotWindow{std::wstring name;Bounds box;};
struct AxisText{std::wstring text;AcGePoint3d position;double rotation{},height{},widthFactor{};AcDbObjectId style;bool mirroredX{},mirroredY{};int actualColor{};AcGePoint3d actualTextCenter;};
struct AxisScene{std::vector<std::pair<AcGePoint3d,AcGePoint3d>> lines;std::vector<std::pair<AcGePoint3d,double>> circles;std::vector<AxisText> texts;std::vector<std::unique_ptr<AcDbEntity>> owned;std::set<const void*> seen;bool visible=true;};
void read_axis(const AcDbEntity& e,AxisScene& result,int depth=0){
 require(depth<8,"axis public nesting too deep");if(std::wstring(e.isA()->name())!=L"TDbMText"&&e.visibility()!=AcDb::kVisible)result.visible=false;
 if(const auto* poly=AcDbPolyline::cast(&e)){for(unsigned int i=1;i<poly->numVerts();++i){AcGePoint3d a,b;check(poly->getPointAt(i-1,a),"title poly start");check(poly->getPointAt(i,b),"title poly end");result.lines.push_back({a,b});}return;}
 if(const auto* line=AcDbLine::cast(&e)){result.lines.push_back({line->startPoint(),line->endPoint()});return;}
 if(const auto* circle=AcDbCircle::cast(&e)){result.circles.push_back({circle->center(),circle->radius()});return;}
 if(const auto* value=AcDbText::cast(&e)){AcString s;check(value->textString(s),"axis actual public text");AcDbExtents bounds;check(value->getGeomExtents(bounds),"actual text glyph extents");const AcGePoint3d center((bounds.minPoint().x+bounds.maxPoint().x)*.5,(bounds.minPoint().y+bounds.maxPoint().y)*.5,(bounds.minPoint().z+bounds.maxPoint().z)*.5);result.texts.push_back({s.kACharPtr(),value->position(),value->rotation(),value->height(),value->widthFactor(),value->textStyle(),value->isMirroredInX()!=0,value->isMirroredInY()!=0,value->colorIndex(),center});return;}
 require(std::wstring(e.isA()->name())==L"TDbAxisLabelSet"||std::wstring(e.isA()->name())==L"TDbSymbDrawingIndex"||std::wstring(e.isA()->name())==L"TDbSymbElevation"||std::wstring(e.isA()->name())==L"TDbMText"||std::wstring(e.isA()->name())==L"TDbDrawingName"||std::wstring(e.isA()->name())==L"TDbText","unexpected native axis/reference-symbol child");
 AcDbVoidPtrArray values;check(e.explode(values),"axis public const explode");std::vector<AcDbEntity*> accepted;
 for(int i=0;i<values.length();++i){auto* raw=values[i];require(raw&&raw!=&e&&result.seen.insert(raw).second,"null/self/repeated axis explode return; borrowed object not deleted");auto* entity=AcDbEntity::cast(static_cast<AcRxObject*>(raw));require(entity&&entity->objectId().isNull()&&entity->ownerId().isNull()&&entity->database()==nullptr,"uncertain axis explode ownership; borrowed object not deleted");result.owned.emplace_back(entity);accepted.push_back(entity);}
 for(const auto* entity:accepted)read_axis(*entity,result,depth+1);
}
AxisScene axis_scene(AcDbObjectId id){require(!id.isNull()&&id.database()!=nullptr,"D5 native axis read requires its actual persistent database");Working scope(id.database());auto e=opened<AcDbEntity>(id);require(std::wstring(e->isA()->name())==L"TDbAxisLabelSet"&&!e->isAProxy(),"axis root native class mismatch");AxisScene result;read_axis(*e,result);return result;}
std::string protected_state(AcDbDatabase& db,const std::vector<AcDbObjectId>& ids){Working work(&db);std::vector<std::string> rows;for(auto id:ids){auto e=opened<AcDbEntity>(id);Fields f{{"handle",handle(id)},{"class",text(e->isA()->name())},{"owner",handle(e->ownerId())},{"layer",handle(e->layerId())},{"lineType",handle(e->linetypeId())},{"color",std::to_string(e->colorIndex())},{"weight",std::to_string(e->lineWeight())},{"visible",std::to_string(e->visibility())}};AcDbExtents ex;const auto s=e->getGeomExtents(ex);f.emplace_back("extentStatus",std::to_string(s));if(s==Acad::eOk){f.emplace_back("min",point(ex.minPoint()));f.emplace_back("max",point(ex.maxPoint()));}AcGePoint3dArray gp;AcDbIntArray a,b;const auto gs=e->getGripPoints(gp,a,b);std::vector<std::string> points;if(gs==Acad::eOk)for(int i=0;i<gp.length();++i)points.push_back(point(gp[i]));f.emplace_back("gripStatus",std::to_string(gs));f.emplace_back("grips",array(points));
 if(const auto* line=AcDbLine::cast(e.get())){f.emplace_back("start",point(line->startPoint()));f.emplace_back("end",point(line->endPoint()));}
 if(const auto* ref=AcDbBlockReference::cast(e.get())){std::vector<std::string> values;for(int r=0;r<4;++r)for(int c=0;c<4;++c)values.push_back(number(ref->blockTransform()(r,c)));f.emplace_back("blockTransform",array(values));f.emplace_back("definition",handle(ref->blockTableRecord()));}
 if(const auto* value=AcDbText::cast(e.get())){AcString t;check(value->textString(t),"protected text");f.emplace_back("text",text(t.kACharPtr()));f.emplace_back("position",point(value->position()));f.emplace_back("rotation",number(value->rotation()));f.emplace_back("height",number(value->height()));f.emplace_back("textStyle",handle(value->textStyle()));}
 if(auto* dim=AcDbDimension::cast(e.get())){double m{};check(dim->measurement(m),"protected dimension");f.emplace_back("measurement",number(m));}
 rows.push_back(object(f));}return array(rows);}
std::vector<AcDbObjectId> resolve_saved(AcDbDatabase& db,const std::vector<AcDbObjectId>& ids){std::vector<AcDbObjectId> result;for(auto old:ids){AcDbObjectId id;check(db.getAcDbObjectId(id,false,old.handle()),"resolve saved identity");result.push_back(id);}return result;}
std::string resource_shape(AcDbObjectId id){
 auto o=opened<AcDbObject>(id);std::string raw;
 if(o->isA()==AcDbTextStyleTableRecord::desc())raw=style_record(id);
 else if(o->isA()==AcDbLayerTableRecord::desc())raw=layer_record(id);
 else if(o->isA()==AcDbDimStyleTableRecord::desc())raw=dimstyle_record(id);
 else if(o->isA()==AcDbLinetypeTableRecord::desc())raw=linetype_record(id);
 else return "null";
 Fields fields;for(const auto& f:flatten(raw))if(f.path!="$.id"&&f.path!="$.name"&&f.path.find(".id")==std::string::npos&&f.path.find("Id")==std::string::npos&&f.path.find("dimtxsty")==std::string::npos)fields.emplace_back(f.path,f.raw);return object(fields);
}
void validate_resource_map(AcDbIdMapping& map){for(AcDbIdMappingIter it(map);!it.done();it.next()){AcDbIdPair pair;require(it.getMap(pair),"D5 resource map read");if(pair.value().isNull())continue;auto from=opened<AcDbObject>(pair.key());auto to=opened<AcDbObject>(pair.value());if(!AcDbSymbolTableRecord::cast(from.get())||!AcDbSymbolTableRecord::cast(to.get()))continue;AcString name;check(AcDbSymbolTableRecord::cast(from.get())->getName(name),"D5 mapped source name");const std::wstring n=name.kACharPtr();if(n==L"AXIS"||n==L"PUB_DIM"||n==L"A-PUB_DIM"||n==L"DIM_ELEV"||n==L"A-DOTE"||n==L"DOTE"||n.rfind(L"_TCH_",0)==0){require(symbol_name(pair.key())==symbol_name(pair.value()),"D5 canonical name changed in clone");require(compare_fields(resource_shape(pair.key()),resource_shape(pair.value())).equal,"D5 same-name resource public definition mismatch");}}}
AcDbObjectId clone_material(AcDbDatabase& source,AcDbDatabase& output,AcDbObjectId seed){AcDbIdMapping map;AcDbObjectIdArray ids;ids.append(seed);check(source.wblockCloneObjects(ids,acdbSymUtil()->blockModelSpaceId(&output),map,AcDb::kDrcIgnore),"D5 local canonical clone Ignore");AcDbIdPair pair;pair.setKey(seed);require(map.compute(pair)&&pair.isCloned()&&pair.value().database()==&output,"D5 primary canonical mapping absent");validate_resource_map(map);return pair.value();}
std::string original_resources(AcDbDatabase& original,AcDbDatabase& target){
 std::vector<std::string> rows;
 for(int kind=0;kind<3;++kind){const auto tableId=kind==0?original.layerTableId():(kind==1?original.textStyleTableId():original.dimStyleTableId());auto table=opened<AcDbSymbolTable>(tableId);AcDbSymbolTableIterator* raw{};check(table->newIterator(raw),"original resource iterator");std::unique_ptr<AcDbSymbolTableIterator> it(raw);
  for(;!it->done();it->step()){AcDbObjectId source;check(it->getRecordId(source),"original resource identity");AcDbObjectId id;check(target.getAcDbObjectId(id,false,source.handle()),"original resource still present");Working work(&target);rows.push_back(kind==0?layer_record(id):(kind==1?style_record(id):dimstyle_record(id)));}
 }
 return array(rows);
}
