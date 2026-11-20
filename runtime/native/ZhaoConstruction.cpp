#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <fstream>
#include <filesystem>
#include <array>
#include <oaidl.h>
#include <axdb.h>
#include <axboiler.h>
#include "RuntimeVersion.h"
#include "NativeAnnotation.h"
#include "ZhaoDimensions.h"
class CPoint;
#include <core_rxmfcapi.h>
#include "MFCommon.inc"
#include "MFLevels.inc"
#include "MFGrips.inc"

struct Product {std::wstring dwg;std::string sha;std::vector<PlotWindow> windows;};
#include "MFPlot.inc"
#include "ZhaoAxis.inc"

struct View { std::string key; double zero{},bottom{},left{},right{},titleX{},titleY{}; };
struct Axis { std::string role,label,sourceHandle; double station{},x{},y{},top{}; };
struct LevelMark { std::string role,side; int height{}; double x{},y{}; bool opening{},below{}; };
struct Control { std::string role; double x1{},y1{},x2{},y2{}; };
struct Dim { std::string role,layer; zhao::DimensionRequest request; };
struct Placement {
 std::vector<View> views;
 std::vector<Axis> axes;
 std::vector<LevelMark> levels;
 std::vector<Control> controls;
 std::vector<Dim> dims;
};
struct TitlePair {AcDbHandle target,donor;std::wstring label;};
struct Job {
 std::wstring root,nonce,prototype;
 Config config;
 std::unique_ptr<Evidence> evidence;
 AcApDocument* control{};
 AcApDocument* openedDoc{};
 Placement plan;
 std::map<std::pair<int,bool>,AcDbHandle> seeds;
 AcDbHandle axisG;
 std::vector<TitlePair> titles;
 std::string commandError;
};
std::unique_ptr<Job> job;
std::wstring runtimeRoot,runtimeNonce;
bool idleHook{};
void onIdle() noexcept;

std::wstring at(const wchar_t* name) { return runtimeRoot+L"\\"+name; }
void stage(const std::string& value) {
 std::ofstream out(at(L"progress.txt"),std::ios::app);
 out << value << '\n';
}
void mark(const wchar_t* name,const std::string& value) {
 auto out=create_new(at(name));
 write_flushed(out.get(),value);
}
std::wstring iniValue(const std::wstring& file,const wchar_t* field) {
 wchar_t buffer[4096]{};
 const auto n=GetPrivateProfileStringW(L"job",field,L"",buffer,static_cast<DWORD>(std::size(buffer)),file.c_str());
 require(n>0 && n<std::size(buffer)-1,"missing runtime binding");
 return std::wstring(buffer,n);
}
#include "RuntimeIdentity.inc"
std::wstring desktopName(HDESK desk) {
 require(desk!=nullptr,"no desktop");
 wchar_t name[256]{};
 require(GetUserObjectInformationW(desk,UOI_NAME,name,sizeof(name),nullptr)!=0,"cannot read desktop name");
 return name;
}
void verifyDesktop() {
 require(runtimeNonce.size()==32 &&
   std::all_of(runtimeNonce.begin(),runtimeNonce.end(),[](wchar_t c){return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}),
   "invalid run nonce");
 const auto expected=L"CodexS1_"+runtimeNonce;
 require(desktopName(GetThreadDesktop(GetCurrentThreadId()))==expected,"not owned private desktop");
 const auto input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
 require(input!=nullptr,"input desktop missing");
 std::wstring active;
 try {active=desktopName(input);} catch(...) {CloseDesktop(input);throw;}
 CloseDesktop(input);
 require(active!=expected,"private desktop is input desktop");
}
std::unique_ptr<AcDbDatabase> drawing(const wchar_t* name) {
 auto db=std::make_unique<AcDbDatabase>(false,true);
 Working work(db.get());
 check(db->readDwgFile(at(name).c_str(),AcDbDatabase::kForReadAndAllShare,false),"read frozen drawing");
 check(db->closeInput(true),"close drawing input");
 return db;
}
AcDbObjectId readHandle(AcDbDatabase& db,const AcDbHandle& handleValue) {
 AcDbObjectId result;
 check(db.getAcDbObjectId(result,false,handleValue),"resolve owned handle");
 return result;
}
std::vector<AcDbObjectId> roots(AcDbDatabase& db) {
 auto model=opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(&db));
 AcDbBlockTableRecordIterator* raw{};
 check(model->newIterator(raw),"model iterator");
 std::unique_ptr<AcDbBlockTableRecordIterator> it(raw);
 std::vector<AcDbObjectId> out;
 for(;!it->done();it->step()){AcDbObjectId id;check(it->getEntityId(id),"model root");out.push_back(id);}
 return out;
}
AcDbObjectId append(AcDbDatabase& db,std::unique_ptr<AcDbEntity> entity) {
 auto model=opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(&db),AcDb::kForWrite);
 AcDbObjectId id;
 check(model->appendAcDbEntity(id,entity.get()),"append owned annotation");
 check(entity.release()->close(),"close owned annotation");
 return id;
}
AcDbObjectId copyEntity(AcDbDatabase& src,AcDbDatabase& dst,AcDbObjectId id) {
 AcDbObjectIdArray ids;ids.append(id);
 AcDbIdMapping map;
 check(src.wblockCloneObjects(ids,acdbSymUtil()->blockModelSpaceId(&dst),map,AcDb::kDrcIgnore),"clone real annotation");
 AcDbIdPair pair;pair.setKey(id);
 require(map.compute(pair)&&pair.isCloned()&&pair.value().database()==&dst,"missing native clone");
 return pair.value();
}
void moveEntity(AcDbObjectId id,const AcGeVector3d& shift) {
 auto e=opened<AcDbEntity>(id,AcDb::kForWrite);
 check(e->transformBy(AcGeMatrix3d::translation(shift)),"position native annotation");
}
AcDbObjectId layer(AcDbDatabase& db,const wchar_t* name,int aci) {
 auto table=opened<AcDbLayerTable>(db.layerTableId());
 AcDbObjectId id;
 if(!table->has(name)) {
  check(table->upgradeOpen(),"write layer table");
  auto row=std::make_unique<AcDbLayerTableRecord>();
  check(row->setName(name),"name annotation layer");
  check(table->add(id,row.get()),"add layer");
  check(row.release()->close(),"close new layer");
 }
 else check(table->getAt(name,id),"get annotation layer");
 auto row=opened<AcDbLayerTableRecord>(id,AcDb::kForWrite);
 AcCmColor col;col.setColorIndex(static_cast<Adesk::UInt16>(aci));
 row->setColor(col);row->setLinetypeObjectId(db.continuousLinetype());
 row->setLineWeight(AcDb::kLnWtByLwDefault);
 return id;
}
void appearance(AcDbObjectId id,AcDbObjectId layerId) {
 auto e=opened<AcDbEntity>(id,AcDb::kForWrite);
 check(e->setLayer(layerId),"set root annotation layer");
 check(e->setColorIndex(256),"set root ByLayer");
}
AcDbObjectId controlStyle(AcDbDatabase& db) {
 auto table=opened<AcDbLinetypeTable>(db.linetypeTableId());
 AcDbObjectId id;
 if(table->has(L"DOTE")) {check(table->getAt(L"DOTE",id),"read DOTE style");return id;}
 check(table->upgradeOpen(),"write linetype table");
 auto l=std::make_unique<AcDbLinetypeTableRecord>();
 check(l->setName(L"DOTE"),"name control line pattern");
 check(l->setNumDashes(4),"four DOTE pattern segments");
 check(l->setPatternLength(600),"DOTE effective period");
 int n=0;for(double v:{350.,-100.,50.,-100.}) check(l->setDashLengthAt(n++,v),"DOTE pattern segment");
 check(table->add(id,l.get()),"add DOTE linetype");
 check(l.release()->close(),"close DOTE style");
 return id;
}
std::wstring caption(const std::string& key) {
 if(key=="front")return L"正立面图";
 if(key=="back")return L"背立面图";
 if(key=="right")return L"右立面图";
 require(key=="left","unknown view");
 return L"左立面图";
}
Placement readPlan() {
 std::ifstream input(at(L"placement.tsv"));
 require(input.good(),"missing placement");
 Placement plan;
 std::string raw;
 while(std::getline(input,raw)) {
  if(raw.empty()||raw[0]=='#') continue;
  std::istringstream row(raw);
  row.imbue(std::locale::classic());
  std::string kind;row>>kind;
  if(kind=="VIEW"){View v;row>>v.key>>v.zero>>v.bottom>>v.left>>v.right>>v.titleX>>v.titleY;plan.views.push_back(v);}
  else if(kind=="DIM"){
   Dim d;std::string name;int n{};row>>d.role>>name>>d.request.vertical>>d.request.definition>>d.request.baseline>>d.layer>>n;
   require(n>=2 && n<=64,"invalid dimension station count");
   d.request.name=d.role+"-"+name;
   d.request.stations.resize(n);
   for(auto& x:d.request.stations)row>>x;
   plan.dims.push_back(d);
  }
  else if(kind=="AXIS"){Axis a;row>>a.role>>a.label>>a.station>>a.x>>a.y>>a.top>>a.sourceHandle;plan.axes.push_back(a);}
  else if(kind=="LEVEL"||kind=="OPENING_LEVEL"){LevelMark l;row>>l.role>>l.side>>l.height>>l.x>>l.y;l.opening=kind=="OPENING_LEVEL";plan.levels.push_back(l);}
  else if(kind=="CONTROL"){Control c;row>>c.role>>c.x1>>c.y1>>c.x2>>c.y2;plan.controls.push_back(c);}
  else throw std::runtime_error("unknown placement row "+kind);
  require(!row.fail(),"incomplete placement row "+raw);
 }
 require(plan.views.size()==eruntime::views&&plan.dims.size()==eruntime::dimensions&&plan.axes.size()==eruntime::axes&&plan.levels.size()==eruntime::levels,"incomplete native placement cohort");
 return plan;
}
AxisScene titleScene(AcDbObjectId id) {
 Working work(id.database());
 auto e=opened<AcDbEntity>(id);
 require(std::wstring(e->isA()->name())==L"TDbDrawingName"&&!e->isAProxy(),"title is not native");
 AxisScene out;read_axis(*e,out);
 return out;
}
AcDbExtents extents(AcDbObjectId id) {
 auto e=opened<AcDbEntity>(id);
 AcDbExtents out;check(e->getGeomExtents(out),"actual entity extents");
 return out;
}
AcGePoint3d center(const AcDbExtents& e) {
 return e.minPoint()+(e.maxPoint()-e.minPoint())*.5;
}
void titleCheck(AcDbObjectId id,const std::wstring& label) {
 const auto actual=titleScene(id);
 require(actual.circles.empty()&&actual.texts.size()==2,"title native children");
 bool name{},scale{};
 for(const auto& t:actual.texts){
  require(!t.mirroredX&&!t.mirroredY&&std::abs(t.rotation)<1e-6&&t.actualColor==7,"title text direction/color");
  name=name||t.text==label;scale=scale||t.text==L"1:100";
 }
 require(name&&scale,"title literal caption/scale did not match");
}
void seedLibrary() {
 stage("read level donor DWG");
 auto z222=drawing(L"levels.dwg");
 stage("read title donor DWG");
 auto ref=drawing(L"titles.dwg");
 auto own=std::make_unique<AcDbDatabase>(true,true);
 Working work(own.get());
 const auto dimElev=layer(*own,L"DIM_ELEV",3);
 const auto dimSym=layer(*own,L"DIM_SYMB",3);
 stage("load native level prototypes");
 const auto normal=d9_read_level(resolve(*z222,L"10BBA"));
 double low=normal.apex.y,high=normal.apex.y;
 for(const auto& line:normal.scene.lines){low=(std::min)(low,(std::min)(line.first.y,line.second.y));high=(std::max)(high,(std::max)(line.first.y,line.second.y));}
 for(const auto& label:normal.scene.texts){low=(std::min)(low,label.actualTextCenter.y-label.height*.5);high=(std::max)(high,label.actualTextCenter.y+label.height*.5);}
 const double symbolHeight=high-low;
 require(symbolHeight>0.,"native level symbol has no measurable vertical extent");
 const auto anchor=copyEntity(*z222,*own,resolve(*z222,L"10BBA"));
 moveEntity(anchor,AcGePoint3d(0,0,0)-bce::trial::readLevel(anchor).tip);
 appearance(anchor,dimElev);
 job->seeds.emplace(std::make_pair(0,false),anchor.handle());
 for(auto& mark:job->plan.levels) {
  mark.below=mark.height<0;
  if(mark.side=="left"||mark.side=="right") {
   for(const auto& higher:job->plan.levels)
    if(higher.role==mark.role&&higher.side==mark.side&&higher.height>mark.height&&
       higher.height-mark.height<symbolHeight)mark.below=true;
  }
  const auto key=std::make_pair(mark.height,mark.below);
  if(job->seeds.count(key))continue;
  const auto donor=resolve(*z222,mark.below?L"10BB6":L"10BBA");
  const auto id=copyEntity(*z222,*own,donor);
  const auto old=bce::trial::readLevel(id);
  moveEntity(id,AcGePoint3d(4000.,mark.height,0)-old.tip);
  appearance(id,dimElev);
  job->seeds.emplace(key,id.handle());
 }
 stage("load native drawing title prototype");
 auto styles=opened<AcDbTextStyleTable>(own->textStyleTableId(),AcDb::kForWrite);
 auto donorStyle=std::make_unique<AcDbTextStyleTableRecord>();
 check(donorStyle->setName(L"ZHAO_COMMAND_DONOR"),"stage Chinese donor style");
 check(donorStyle->setFileName(L"simhei.ttf"),"set actual Chinese font");
 AcDbObjectId donorStyleId;
 check(styles->add(donorStyleId,donorStyle.get()),"append Chinese donor style");
 check(donorStyle.release()->close(),"close Chinese donor style");
 styles.reset();
 for(const auto& v:job->plan.views){
  const auto target=copyEntity(*ref,*own,resolve(*ref,L"98E"));
  const AcGePoint3d stagePoint(100000.+30000.*job->titles.size(),50000.,0.);
  moveEntity(target,stagePoint-center(extents(target)));
  appearance(target,dimSym);
  auto text=std::make_unique<AcDbText>(stagePoint+AcGeVector3d(0,6000,0),caption(v.key).c_str(),donorStyleId,300);
  const auto donor=append(*own,std::move(text));
  job->titles.push_back({target.handle(),donor.handle(),caption(v.key)});
 }
 stage("load native single-ended axis prototype");
 const auto g=copyEntity(*z222,*own,resolve(*z222,L"10BDF"));
 const auto gScene=axis_scene(g);
 require(gScene.texts.size()==1&&gScene.texts[0].text==L"F"&&gScene.circles.size()==1,"F single-ended prototype differs");
 moveEntity(g,AcGePoint3d(30000,0,0)-gScene.circles[0].first);
 job->axisG=g.handle();
 job->prototype=at(L"prototypes.dwg");
 check(own->saveAs(job->prototype.c_str(),false,AcDb::kDHL_CURRENT),"save owned prototypes");
}

void configureCommandView(const AcDbExtents& a,const AcDbExtents& b) {
 const double x0=(std::min)(a.minPoint().x,b.minPoint().x)-1000;
 const double y0=(std::min)(a.minPoint().y,b.minPoint().y)-1000;
 const double x1=(std::max)(a.maxPoint().x,b.maxPoint().x)+1000;
 const double y1=(std::max)(a.maxPoint().y,b.maxPoint().y)+1000;
 ads_point lo{x0,y0,0},hi{x1,y1,0};
 require(acedCommandS(RTSTR,L"_.ZOOM",RTSTR,L"_W",RT3DPOINT,lo,RT3DPOINT,hi,RTNONE)==RTNORM,"zoom owned command region");
 require(acedCommandS(RTSTR,L"_.REGEN",RTNONE)==RTNORM,"regen owned command region");
}
void verifyPick(AcDbObjectId id,const AcGePoint3d& p) {
 ads_point pt{p.x,p.y,p.z};ads_name hit{};ads_matrix matrix{};resbuf* stack{};
 const auto status=acedNEntSelP(L"",hit,pt,1,matrix,&stack);
 if(stack) acutRelRb(stack);
 AcDbObjectId actual;
 require(status==RTNORM&&acdbGetObjectId(actual,hit)==Acad::eOk&&actual==id,"public command pick not unique at target");
}
struct Selection {
 ads_name ids{};
 Selection(){require(acedSSAdd(nullptr,nullptr,ids)==RTNORM,"create correction selection");}
 ~Selection(){acedSSFree(ids);}
 void add(AcDbObjectId id){ads_name name{};check(acdbGetAdsName(name,id),"actual elevation entity");require(acedSSAdd(name,ids,ids)==RTNORM,"add actual level target");}
};
struct Variables {
 std::array<short,5> old{};
 const wchar_t* names[5]{L"OSMODE",L"PICKSTYLE",L"CMDDIA",L"FILEDIA",L"PICKBOX"};
 Variables(){
  for(int i=0;i<5;++i){resbuf v{};require(acedGetVar(names[i],&v)==RTNORM&&v.restype==RTSHORT,"read local input variable");old[i]=v.resval.rint;}
  for(int i=0;i<5;++i){resbuf v{};v.restype=RTSHORT;v.resval.rint=i==4?50:0;require(acedSetVar(names[i],&v)==RTNORM,"set owned command input");}
 }
 ~Variables(){for(int i=0;i<5;++i){resbuf v{};v.restype=RTSHORT;v.resval.rint=old[i];acedSetVar(names[i],&v);}}
};
void correctPrototypes(AcDbDatabase& db) {
 Variables variables;
 const auto zero=readHandle(db,job->seeds.at(std::make_pair(0,false)));
 const auto z=d9_read_level(zero);
 const auto p=z.scene.lines[0].first+(z.scene.lines[0].second-z.scene.lines[0].first)*.5;
 configureCommandView(extents(zero),extents(zero));
 verifyPick(zero,p);
 Selection set;
 for(const auto& s:job->seeds)if(s.first!=std::make_pair(0,false))set.add(readHandle(db,s.second));
 if(!acedRegCmds->lookupGlobalCmd(L"TCheckElev"))
  acrxDynamicLinker->loadModule(L"C:/Tangent/TArchT20V8/sys23x64/tch_kerncmd.arx",false,false);
 require(acedRegCmds->lookupGlobalCmd(L"TCheckElev")!=nullptr,"TCheckElev unavailable");
 ads_point pt{p.x,p.y,p.z};
 require(acedCommandS(RTSTR,L"TCheckElev",RTPOINT,pt,RTPICKS,set.ids,RTSTR,L"",RTSTR,L"A",RTNONE)==RTNORM,"TCheckElev correction failed");
 for(const auto& s:job->seeds){
  const auto a=bce::trial::readLevel(readHandle(db,s.second));
  require(std::abs(a.metres*1000.-s.first.first)<.0001&&std::abs(a.tip.y-s.first.first)<.0001&&a.upright,"corrected level mismatched");
 }
 stage("native level prototypes verified");
 if(!acedRegCmds->lookupGlobalCmd(L"TTextReplace"))
  acrxDynamicLinker->loadModule(L"C:/Tangent/TArchT20V8/sys23x64/tch_TgTool.arx",false,false);
 require(acedRegCmds->lookupGlobalCmd(L"TTextReplace")!=nullptr,"TTextReplace unavailable");
 for(const auto& item:job->titles) {
  const auto source=readHandle(db,item.donor), target=readHandle(db,item.target);
  const auto sourceExt=extents(source),targetExt=extents(target);
  configureCommandView(sourceExt,targetExt);
  const auto parts=titleScene(target);
  const AxisText* body{};
  for(const auto& t:parts.texts)if(t.text.find(L"立面")!=std::wstring::npos)body=&t;
  require(body,"native title body not found");
  const auto a=center(sourceExt);
  const auto b=body->actualTextCenter;
  verifyPick(source,a);
  AcGePoint3d targetPick;
  bool found{};
  for(double x:{b.x,b.x-150.,b.x+150.,center(targetExt).x}) {
   for(double y:{b.y,b.y+100.,b.y-100.}) {
    try {verifyPick(target,{x,y,0});targetPick={x,y,0};found=true;break;}catch(const std::exception&){}
   }
   if(found)break;
  }
  require(found,"could not pick real title body");
  ads_point donor{a.x,a.y,a.z},label{targetPick.x,targetPick.y,targetPick.z};
  require(acedCommandS(RTSTR,L"TTextReplace",RT3DPOINT,donor,RTSTR,L"",RT3DPOINT,label,RTSTR,L"",RTNONE)==RTNORM,"TTextReplace correction failed");
  require(acedCommandS(RTSTR,L"_.REGEN",RTNONE)==RTNORM,"refresh native title");
  titleCheck(target,item.label);
  check(opened<AcDbEntity>(source,AcDb::kForWrite)->erase(),"erase isolated temporary text");
  stage("native title "+utf8(item.label)+" verified");
 }
 check(db.saveAs(job->prototype.c_str(),true,AcDb::kDHL_CURRENT),"save corrected prototype drawing");
}

struct Expected { AcDbHandle handle;std::size_t index; };
std::vector<Control> clearCaptionGuides(const Placement& plan) {
 std::vector<Control> result;
 for(const auto& c:plan.controls) {
  if(std::abs(c.y1-c.y2)>.001){result.push_back(c);continue;}
  std::vector<std::pair<double,double>> gaps;
  const auto left=(std::min)(c.x1,c.x2),right=(std::max)(c.x1,c.x2);
  for(const auto& d:plan.dims) {
   if(d.role!=c.role||!d.request.vertical)continue;
   const auto x=d.request.baseline-275.;
   if(x+225.<=left||x-225.>=right)continue;
   const auto& stations=d.request.stations;
   for(std::size_t i=1;i<stations.size();++i) {
    const auto textY=(stations[i]+stations[i-1])*.5;
    const auto digits=std::to_string(std::llround(stations[i]-stations[i-1])).size();
    if(std::abs(textY-c.y1)<75.+80.*digits)
     gaps.emplace_back((std::max)(left,x-225.),(std::min)(right,x+225.));
   }
  }
  for(const auto& mark:plan.levels) {
   if(mark.role==c.role && mark.side=="local-right" &&
      c.y1>=mark.y+280. && c.y1<=mark.y+710.) {
    const auto start=(std::max)(left,mark.x-365.);
    const auto end=(std::min)(right,mark.x+1230.);
    if(end>start)gaps.emplace_back(start,end);
   }
  }
  if(gaps.empty()){result.push_back(c);continue;}
  std::sort(gaps.begin(),gaps.end());
  const auto emit=[&](double a,double b) {
   if(b-a<125.)return;
   if(c.x1>c.x2)std::swap(a,b);
   result.push_back({c.role,a,c.y1,b,c.y2});
  };
  double start=left;
  for(const auto& gap:gaps){emit(start,gap.first);start=(std::max)(start,gap.second);}
  emit(start,right);
 }
 return result;
}
template<class T>struct ComRelease {void operator()(T* p)const noexcept{if(p)p->Release();}};
struct OwnedVariant {VARIANT value;OwnedVariant(){VariantInit(&value);}~OwnedVariant(){VariantClear(&value);}OwnedVariant(const OwnedVariant&)=delete;OwnedVariant& operator=(const OwnedVariant&)=delete;};
std::wstring nativeLevelProperty(AcDbObjectId id,const wchar_t* property,const wchar_t* setValue=nullptr) {
 Working work(id.database());IUnknown* raw{};
 require(SUCCEEDED(AcAxGetIUnknownOfObject(&raw,id,acedGetIDispatch(false))),"native level Automation wrapper");
 std::unique_ptr<IUnknown,ComRelease<IUnknown>> unknown(raw);IDispatch* rawDispatch{};
 require(SUCCEEDED(unknown->QueryInterface(IID_IDispatch,reinterpret_cast<void**>(&rawDispatch))),"native level IDispatch");
 std::unique_ptr<IDispatch,ComRelease<IDispatch>> dispatch(rawDispatch);
 LPOLESTR name=const_cast<LPOLESTR>(property);DISPID key{};
 require(SUCCEEDED(dispatch->GetIDsOfNames(IID_NULL,&name,1,LOCALE_USER_DEFAULT,&key)),"native level property name");
 if(setValue){OwnedVariant owned;owned.value.vt=VT_BSTR;owned.value.bstrVal=SysAllocString(setValue);require(owned.value.bstrVal!=nullptr,"allocate native property value");DISPID put=DISPID_PROPERTYPUT;DISPPARAMS args{&owned.value,&put,1,1};
  require(SUCCEEDED(dispatch->Invoke(key,IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_PROPERTYPUT,&args,nullptr,nullptr,nullptr)),"set actual native level property");}
 DISPPARAMS args{};OwnedVariant result;
 require(SUCCEEDED(dispatch->Invoke(key,IID_NULL,LOCALE_USER_DEFAULT,DISPATCH_PROPERTYGET,&args,&result.value,nullptr,nullptr)),"read actual native level property");
 require(result.value.vt==VT_BSTR&&result.value.bstrVal,"native level property must be a documented string");return std::wstring(result.value.bstrVal);
}
void removeOpeningLeader(AcDbObjectId id) {
 const auto before=d9_read_level(id);
 const auto beforeJson=d9_level_state(id);
 require(nativeLevelProperty(id,L"HasLeader",L"否")==L"否","opening leader flag did not turn off");
 require(nativeLevelProperty(id,L"HasBaseLine",L"否")==L"否","opening measurement guide flag did not turn off");
 const auto after=d9_read_level(id);
 require(after.apex.distanceTo(before.apex)<.0001&&after.caption==before.caption,"leader edit changed opening measurement");
 if(GetFileAttributesW(at(L"opening-leader-probe.json").c_str())==INVALID_FILE_ATTRIBUTES)mark(L"opening-leader-probe.json",object({{"before",beforeJson},{"after",d9_level_state(id)}}));
 require(after.scene.lines.size()==3||after.scene.lines.size()==4,"leader-free native level must retain its actual triangle and baseline representation");
}
void verifyLevel(AcDbObjectId id,const LevelMark& m) {
 const auto a=bce::trial::readLevel(id);
 require(std::abs(a.metres*1000.-m.height)<.0001&&a.upright&&
         a.tip.distanceTo({m.x,m.y,0})<.0001,"saved true level does not match expected matrix");
 const auto l=d9_read_level(id);
 const auto& t=l.scene.texts[0];
 require(std::abs(t.height-297.5)<.0001,"saved level font height changed");
 require(symbol_name(opened<AcDbEntity>(id)->layerId())==text(L"DIM_ELEV"),"saved level layer");
 if(m.opening){require(nativeLevelProperty(id,L"HasLeader")==L"否","reopened opening leader returned");require(nativeLevelProperty(id,L"HasBaseLine")==L"否","reopened opening guide returned");}
}
void verifyLevelConnection(AcDbObjectId id,const LevelMark& m,const Placement& plan) {
 if(m.side!="left"&&m.side!="right")return;
 const auto chain=std::find_if(plan.dims.begin(),plan.dims.end(),[&](const Dim& dim){
   return dim.role==m.role&&dim.request.name==m.role+"-"+m.side+"_levels";
 });
 require(chain!=plan.dims.end(),"missing second dimension chain");
 require(std::any_of(chain->request.stations.begin(),chain->request.stations.end(),
                     [&](double station){return std::abs(station-m.y)<.0001;}),
         "main level must connect at a measured station of the second dimension chain");
 const auto state=d9_read_level(id);
 const auto end=AcGePoint3d(chain->request.baseline,m.y,0);
 const auto datum=std::find_if(state.scene.lines.begin(),state.scene.lines.end(),
     [&](const auto& line) {
      return std::abs(line.first.y-m.y)<.0001&&std::abs(line.second.y-m.y)<.0001
        && (line.first.distanceTo(end)<.0001||line.second.distanceTo(end)<.0001);
     });
 require(datum!=state.scene.lines.end(),"native elevation datum did not meet second baseline");
 const auto& label=state.scene.texts.front();
 const bool below=m.below;
 require(below?label.actualTextCenter.y<m.y-300.:label.actualTextCenter.y>m.y+300.,
         "main elevation label orientation did not match close-pair layout");
 const auto hasExpectedTriangle=std::count_if(state.scene.lines.begin(),state.scene.lines.end(),
     [&](const auto& line) {
      if(std::abs(line.first.x-line.second.x)<.0001||std::abs(line.first.y-line.second.y)<.0001)
       return false;
      const auto& other=line.first.distanceTo(state.apex)<.0001?line.second:line.first;
      return std::abs(other.y-m.y-(below?-300.:300.))<.0001;
     });
 require(hasExpectedTriangle==2,"main level triangle orientation mismatch");
}

bool lowerLocalCaption(AcDbObjectId id,double horizontalShift=0.) {
 const auto before=d9_read_level(id);const auto position=before.scene.texts[0].position;
 const AcGeVector3d delta(horizontalShift,-100,0);
 const auto move=[&](AcDbObjectId target){
  const auto state=d9_read_level(target);AcDbIntArray selected;
  for(std::size_t i=0;i<state.grips.size();++i)if(state.grips[i].distanceTo(position)<.0001)selected.append(static_cast<int>(i));
  if(selected.length()!=1)return false;
  auto entity=opened<AcDbEntity>(target,AcDb::kForWrite);return entity->moveGripPointsAt(selected,delta)==Acad::eOk;
 };
 AcDbDatabase scratch(true,true);const auto candidate=copyEntity(*id.database(),scratch,id);
 if(!move(candidate))return false;const auto after=d9_read_level(candidate);
 if(after.apex.distanceTo(before.apex)>.0001||after.caption!=before.caption||after.scene.texts[0].position.distanceTo(position+delta)>.0001)return false;
 for(const auto& line:before.scene.lines)if(std::none_of(after.scene.lines.begin(),after.scene.lines.end(),[&](const auto& value){return d9_same_line(line,value);}))return false;
 require(move(id),"move verified local caption grip");
 require(d9_read_level(id).scene.texts[0].position.distanceTo(position+delta)<.0001,"local caption position differs");
 return true;
}

void apply() {
 auto target=drawing(L"source.dwg");
 auto dimDonor=drawing(L"dimension-donor.dwg");
 auto z222=drawing(L"levels.dwg");
 auto axisDonor=drawing(L"axis-donor.dwg");
 auto proto=drawing(L"prototypes.dwg");
 const auto& plan=job->plan;
 const auto clearControls=clearCaptionGuides(plan);
 std::vector<AcDbObjectId> preserved;
 std::string original;
 std::vector<Expected> dims,axes,marks,names,controls;
 const auto result=job->config.output+L"\\construction.dwg";
 std::vector<PlotWindow> windows;
 Bounds all;
 {
  Working work(target.get());
  for(auto id:roots(*target)) {
   auto e=opened<AcDbEntity>(id);
   if(symbol_name(e->layerId())==text(L"DIM_SYMB")) {
    require(e->isKindOf(AcDbBlockReference::desc()),"unexpected source annotation on title layer");
    e.reset();check(opened<AcDbEntity>(id,AcDb::kForWrite)->erase(),"replace old title block");
   } else preserved.push_back(id);
  }
  require(preserved.size()==eruntime::roots,"source geometry cohort differs");
  original=protected_state(*target,preserved);
  const auto axis=layer(*target,L"AXIS",3);
  const auto detail=layer(*target,L"PUB_DIM",25);
  const auto elevation=layer(*target,L"DIM_ELEV",3);
  const auto sym=layer(*target,L"DIM_SYMB",3);
  const auto control=layer(*target,L"DOTE",24);
  const auto lt=controlStyle(*target);
  zhao::DimensionBuilder builder(*dimDonor,*target,job->config,*job->evidence);
  for(std::size_t i=0;i<plan.dims.size();++i) {
   auto request=plan.dims[i].request;
   request.layer=plan.dims[i].layer=="AXIS"?axis:detail;
   const auto id=builder.add(request);
   dims.push_back({id.handle(),i});
   stage("dimension "+request.name);
  }
  for(std::size_t i=0;i<plan.axes.size();++i) {
   const auto& a=plan.axes[i];
   const auto sourceHandle=std::wstring(a.sourceHandle.begin(),a.sourceHandle.end());
   auto srcPlan=drawing(L"source-plan.dwg");
   auto id=copyEntity(*srcPlan,*target,resolve(*srcPlan,sourceHandle.c_str()));
   const auto pivot=center(extents(id));
   const double angle=a.label=="T"?3.141592653589793:a.label=="L"?1.5707963267948966:a.label=="R"?-1.5707963267948966:0.;
   if(angle!=0)check(opened<AcDbEntity>(id,AcDb::kForWrite)->transformBy(AcGeMatrix3d::rotation(angle,AcGeVector3d::kZAxis,pivot)),"orient original axis leader");
   const auto bounds=extents(id);
   const auto circle=AcGePoint3d(center(bounds).x,bounds.minPoint().y+400.,0);
   moveEntity(id,AcGePoint3d(a.x,a.y,0)-circle);
   appearance(id,axis);
   const auto axScene=axis_scene(id);
   require(axScene.texts.size()==1&&axScene.circles.size()==1&&axScene.lines.size()==1,"source axis is not a single-ended native axis");
   const auto ringTop=AcGePoint3d(a.x,a.y+400.,0.);
   const auto oldTip=axScene.lines[0].first.distanceTo(ringTop)<.001?axScene.lines[0].second:axScene.lines[0].first;
   if(std::abs(a.station-a.x)<.001)id=zhao_axis_detail::changeEndpoint(*target,id,oldTip,AcGePoint3d(a.station,a.top,0),axScene.texts[0].text,AcGePoint3d(a.x,a.y,0));
   else{const auto old=id;AcDbDatabase axisScratch(true,true);const auto seed=copyEntity(*target,axisScratch,old);id=zhao_axis_from_seed(seed,*target,axScene.texts[0].text.c_str(),a.station,a.x,a.y,a.top);zhao_axis_detail::erase(old);}
   axes.push_back({id.handle(),i});
   stage("axis "+a.role+" "+a.label);
  }
  for(std::size_t i=0;i<clearControls.size();++i) {
   const auto& c=clearControls[i];
   auto line=std::make_unique<AcDbLine>(AcGePoint3d(c.x1,c.y1,0),AcGePoint3d(c.x2,c.y2,0));
   const auto id=append(*target,std::move(line));
   appearance(id,control);
   check(opened<AcDbLine>(id,AcDb::kForWrite)->setLinetype(lt),"set DOTE actual line type");
   controls.push_back({id.handle(),i});
  }
  for(std::size_t i=0;i<plan.levels.size();++i) {
   const auto& m=plan.levels[i];
   const auto src=readHandle(*proto,job->seeds.at(std::make_pair(m.height,m.below)));
   const auto id=copyEntity(*proto,*target,src);
   const auto old=bce::trial::readLevel(id);
   if(m.side=="left"||m.side=="right") {
    const auto view=std::find_if(plan.views.begin(),plan.views.end(),
        [&](const View& v){return v.key==m.role;});
    require(view!=plan.views.end(),"missing main level view");
    const double axisX=m.side=="left"?view->left:view->right;
    const auto chain=std::find_if(plan.dims.begin(),plan.dims.end(),[&](const Dim& d){return d.role==m.role&&d.request.name==m.role+"-"+m.side+"_levels";});
    require(chain!=plan.dims.end(),"missing rule-driven second baseline");
    const double secondX=chain->request.baseline;
    const double distance=std::abs(secondX-m.x);
    const auto before=d9_read_level(id);
    bool adjusted=false;
    for(bool modern:{false,true}) {
     try {
      AcDbDatabase scratch(true,true);
      const auto candidate=copyEntity(*target,scratch,id);
      d9_move_datum_end(candidate,modern,distance);
      if(!d9_datum_post(before,d9_read_level(candidate),distance))continue;
     } catch(const std::exception&) {continue;}
     d9_move_datum_end(id,modern,distance);
     adjusted=d9_datum_post(before,d9_read_level(id),distance);
     break;
    }
    require(adjusted,"could not set native level datum to the second chain");
   }
   moveEntity(id,AcGePoint3d(m.x,m.y,0)-old.tip);
   AcDbObjectId actual=id;
   if(m.side=="right"||m.side=="local-right") {
    actual=bce::trial::mirrorLevel(id,m.x+10000.);
    moveEntity(actual,{-10000,0,0});
    check(opened<AcDbEntity>(id,AcDb::kForWrite)->erase(),"discard right intermediate clone");
   }
   appearance(actual,elevation);
   if(m.side=="local-right"||m.side=="local-left")lowerLocalCaption(actual,(m.role=="front"&&m.side=="local-left"&&m.x<4000.)?-700.:0.);
   if(m.opening)removeOpeningLeader(actual);
   verifyLevel(actual,m);
   verifyLevelConnection(actual,m,plan);
   marks.push_back({actual.handle(),i});
  }
  for(std::size_t i=0;i<plan.views.size();++i) {
   const auto& v=plan.views[i];
   const auto id=copyEntity(*proto,*target,readHandle(*proto,job->titles.at(i).target));
   const auto parts=titleScene(id);
   double y{};
   for(const auto& t:parts.texts)if(t.text==caption(v.key))y=t.position.y;
   moveEntity(id,AcGeVector3d(v.titleX-center(extents(id)).x,v.titleY-y,0));
   appearance(id,sym);
   titleCheck(id,caption(v.key));
   names.push_back({id.handle(),i});
  }
  require(original==protected_state(*target,preserved),"source geometry root state changed");
  check(target->saveAs(result.c_str(),false,AcDb::kDHL_CURRENT),"save construction DWG");
  stage("DWG saved");
  auto saved=drawing(L"case\\construction.dwg");
  Working reopened(saved.get());
  require(original==protected_state(*saved,resolve_saved(*saved,preserved)),"reopened source geometry root state changed");
  for(const auto& r:dims) {
   auto request=plan.dims[r.index].request;
   request.layer=plan.dims[r.index].layer=="AXIS"?axis:detail;
   builder.verify(readHandle(*saved,r.handle),request);
  }
  for(const auto& r:axes){
   const auto& a=plan.axes[r.index];
   const auto id=readHandle(*saved,r.handle);const auto bounds=extents(id);
   const auto scene=axis_scene(id);require(scene.texts.size()==1,"source caption missing");zhao_verify_axis(id,scene.texts[0].text.c_str(),a.station,a.x,a.y,a.top);
  }
  for(const auto& r:marks){
   const auto id=readHandle(*saved,r.handle);
   verifyLevel(id,plan.levels[r.index]);
   verifyLevelConnection(id,plan.levels[r.index],plan);
  }
  for(const auto& r:names)titleCheck(readHandle(*saved,r.handle),caption(plan.views[r.index].key));
  for(const auto& r:controls){
   auto line=opened<AcDbLine>(readHandle(*saved,r.handle));
   const auto& c=clearControls[r.index];
   require(line->startPoint().distanceTo({c.x1,c.y1,0})<.0001&&line->endPoint().distanceTo({c.x2,c.y2,0})<.0001&&
           symbol_name(line->layerId())==text(L"DOTE")&&symbol_name(line->linetypeId())==text(L"DOTE"),"reopened control line mismatch");
  }
  for(const auto& v:plan.views) {
   Bounds b;
   const auto mid=(v.left+v.right)*.5;
   for(const auto id:roots(*saved)) {
    const auto ex=extents(id);
    const auto p=center(ex);
    if(p.y>v.zero-9000 && p.y<v.zero+20000 && std::none_of(plan.views.begin(),plan.views.end(),[&](const View& other){return other.key!=v.key && std::abs(other.zero-v.zero)<1. && std::abs(p.x-(other.left+other.right)*.5)<std::abs(p.x-mid)-.001;})) {
     b.add(ex.minPoint());b.add(ex.maxPoint());
    }
   }
   require(b.valid,"empty drawing view");
   all.add(b);
   windows.push_back({std::wstring(v.key.begin(),v.key.end()),b.padded(1600)});
  }
  require(all.valid,"four elevations have no drawing extents");
 }
 auto report=object({{"status",quote(std::string("passed"))},
  {"dwg",quote(result)},{"SHA256",quote(sha256(result))},{"sourceSHA256",quote(sha256(at(L"source.dwg")))},
  {"preservedRoots",std::to_string(preserved.size())},{"dimensionChains",std::to_string(dims.size())},
  {"trueAxisLabels",std::to_string(axes.size())},{"mainLevels",std::to_string(std::count_if(plan.levels.begin(),plan.levels.end(),[](const LevelMark& m){return m.side=="left"||m.side=="right";}))},{"localLevels",std::to_string(std::count_if(plan.levels.begin(),plan.levels.end(),[](const LevelMark& m){return m.side!="left"&&m.side!="right";}))},{"openingLevelsWithoutLeader",std::to_string(std::count_if(plan.levels.begin(),plan.levels.end(),[](const LevelMark& m){return m.opening;}))},{"mainLevelsConnectedToSecondChain",std::to_string(std::count_if(plan.levels.begin(),plan.levels.end(),[](const LevelMark& m){return m.side=="left"||m.side=="right";}))},
  {"drawingNames",std::to_string(names.size())},{"controlLines",std::to_string(controls.size())}});
 mark(L"readback.json",report);
 stage("native DWG independently verified");
 Product product{result,sha256(result),windows};
 preview_detail(job->config,product,*job->evidence);
}
void fail(const std::string& why) {
 try { stage("FAIL "+why);mark(L"failed.txt",why); } catch(...) {}
 job.reset();
}
void resumeJob(void*) noexcept {
 try {
  require(job&&acDocManager->isApplicationContext(),"application continuation not active");
  require(job->openedDoc==acDocManager->curDocument(),"prototype document lost before closing");
  check(acDocManager->appContextCloseDocument(job->openedDoc),"close only owned prototypes");
  job->openedDoc=nullptr;
  require(job->control==acDocManager->curDocument(),"owned control not restored");
  require(job->commandError.empty(),job->commandError);
  apply();
  job->evidence->finish(job->config,"passed");
  job.reset();
  mark(L"done.txt","passed");
  auto* owned=acDocManager->curDocument();
  require(owned!=nullptr,"owned control missing before native quit");
  check(acDocManager->sendStringToExecute(owned,L"_.QUIT\n_N\n",false,false,false),"queue native quit");
  runtimeState(L"native.quit.done","nativeQuitQueued");
 }catch(const std::exception& e){fail(e.what());}catch(...){fail("unknown resume failure");}
}
void runCommands(void*) noexcept {
 try{
  require(job&&job->openedDoc==acDocManager->curDocument()&&
    !acDocManager->isApplicationContext(),"prototype is not the current command document");
  Working work(job->openedDoc->database());
  correctPrototypes(*job->openedDoc->database());
 }catch(const std::exception& e){job->commandError=e.what();}catch(...){job->commandError="unknown prototype command error";}
 if(acDocManager->beginExecuteInApplicationContext(resumeJob,nullptr)!=Acad::eOk)fail("could not queue continuation");
}
void startJob() noexcept {
 try {
  if(idleHook){require(acedRemoveOnIdleWinMsg(onIdle),"remove startup callback");idleHook=false;}
  verifyDesktop();
  require(acDocManager->isApplicationContext(),"requires application command");
  require(!job,"job already started");
  job=std::make_unique<Job>();
  job->control=acDocManager->curDocument();
  require(job->control&&_wcsicmp(job->control->fileName(),at(L"control.dwg").c_str())==0,"own control drawing mismatch");
  job->config.output=at(L"case");
  job->config.session=runtimeRoot;
  job->config.nonce=runtimeNonce;
  job->config.caseName=L"ElevationRuntime";
  job->evidence=std::make_unique<Evidence>(job->config);
  require(sha256(at(L"source.dwg"))==mfinput::hashes[0],"frozen source hash differs");
  if(GetFileAttributesW(at(L"preflight.txt").c_str())!=INVALID_FILE_ATTRIBUTES){
   if(!acrxClassDictionary->at(L"TDbDimension2"))require(acrxDynamicLinker->loadModule(L"C:/Tangent/TArchT20V8/sys23x64/tch_kernal.arx",false,false),"load native classes");
   auto src=drawing(L"source-plan.dwg");Working w(src.get());std::vector<std::string> records;for(auto id:roots(*src)){try{records.push_back(protected_state(*src,std::vector<AcDbObjectId>{id}));}catch(const std::exception&) {}}mark(L"plan-native-state.json",array(records));
   mark(L"done.txt","passed");job.reset();return;
  }
  job->plan=readPlan();
  stage("start with "+std::to_string(job->plan.dims.size())+" dimension chains");
  stage("verify live Tianzheng class registry");
  auto* classes=acrxClassDictionary;
  if(!classes->at(L"TDbDimension2")||!classes->at(L"TDbSymbElevation")){
   const std::wstring kernel=L"C:/Tangent/TArchT20V8/sys23x64/tch_kernal.arx";
   require(acrxDynamicLinker->loadModule(kernel.c_str(),false,false),"load actual TArch object module");
  }
  require(classes->at(L"TDbDimension2")&&classes->at(L"TDbSymbElevation"),"Tianzheng native classes unavailable");
  {auto sourcePlan=drawing(L"source-plan.dwg");Working w(sourcePlan.get());std::vector<std::string> records;
   for(auto id:roots(*sourcePlan)){auto e=opened<AcDbEntity>(id);if(std::wstring(e->isA()->name())!=L"TDbAxisLabelSet")continue;e.reset();const auto scene=axis_scene(id);std::vector<std::string> labels;for(const auto& t:scene.texts)labels.push_back(quote(t.text));records.push_back(object({{"handle",handle(id)},{"labels",array(labels)}}));}
   mark(L"source-axis-captions.json",array(records));}
  seedLibrary();
  stage("native prototype drawing prepared");
  check(acDocManager->appContextOpenDocument(job->prototype.c_str()),"open only owned prototype DWG");
  job->openedDoc=acDocManager->curDocument();
  require(job->openedDoc&&_wcsicmp(job->openedDoc->fileName(),job->prototype.c_str())==0,"not owned prototype DWG");
  check(acDocManager->beginExecuteInCommandContext(runCommands,nullptr),"queue prototype correction");
 }catch(const std::exception& e){fail(e.what());}catch(...){fail("unknown start failure");}
}
void onIdle() noexcept {
 try{
  if(job||GetFileAttributesW(at(L"dispatch.txt").c_str())!=INVALID_FILE_ATTRIBUTES)return;
  auto* doc=acDocManager->curDocument();
  if(!doc||doc!=acDocManager->mdiActiveDocument()||!doc->isQuiescent()||
     acDocManager->inputPending(doc)!=0||_wcsicmp(doc->fileName(),at(L"control.dwg").c_str())!=0)return;
  verifyDesktop();
  mark(L"dispatch.txt","owned control idle");
  check(acDocManager->sendStringToExecute(doc,L"ZHAO_CONSTRUCTION_RUN\n",false,false,false),"queue project job");
 }catch(const std::exception& e){fail(e.what());}catch(...){fail("unknown idle failure");}
}
}
BOOL APIENTRY DllMain(HMODULE instance,DWORD reason,LPVOID) {
 if(reason==DLL_PROCESS_ATTACH){moduleHandle=instance;DisableThreadLibraryCalls(instance);}
 return TRUE;
}
extern "C" __declspec(dllexport) AcRx::AppRetCode acrxEntryPoint(AcRx::AppMsgCode msg,void* id) noexcept {
 try{
  if(msg==AcRx::kInitAppMsg){
   acrxDynamicLinker->unlockApplication(id);
   acrxRegisterAppMDIAware(id);
   wchar_t file[4096]{};
   require(GetModuleFileNameW(moduleHandle,file,static_cast<DWORD>(std::size(file)))!=0,"module identity");
   auto ini=std::filesystem::path(file).replace_extension(L".ini").wstring();
   runtimeRoot=iniValue(ini,L"root");
   runtimeNonce=iniValue(ini,L"nonce");
   verifyRuntimeIdentity(ini,std::wstring(file));
   verifyDesktop();
   check(acedRegCmds->addCommand(L"ZHAO_CONSTRUCTION_12",L"ZHAO_CONSTRUCTION_RUN",L"ZHAO_CONSTRUCTION_RUN",ACRX_CMD_SESSION,startJob),"register isolated production case");
   require(acedRegisterOnIdleWinMsg(onIdle),"register owned startup callback");
   idleHook=true;
   mark(L"registered.txt","owned desktop verified");
   runtimeState(L"quiet-ready.json","verifiedModuleReady");
  }else if(msg==AcRx::kUnloadAppMsg){
   if(idleHook&&!acedRemoveOnIdleWinMsg(onIdle))return AcRx::kRetError;
   idleHook=false;
   check(acedRegCmds->removeGroup(L"ZHAO_CONSTRUCTION_12"),"remove isolated case command");
  }
  return AcRx::kRetOK;
 }catch(const std::exception& e){if(!runtimeRoot.empty())fail(e.what());return AcRx::kRetError;}catch(...){return AcRx::kRetError;}
}
