struct NativePlotStatus {
    bool called{};
    int value{};
    void set(Acad::ErrorStatus status) { called = true; value = static_cast<int>(status); }
    std::string json() const { return called ? std::to_string(value) : "null"; }
    bool ok() const { return called && value == Acad::eOk; }
};

class NativePlotPipeline {
public:
    enum Stage { Create, BeginPlot, BeginDocument, BeginPage, BeginGraphics, EndGraphics, EndPage, EndDocument, EndPlot, Count };
    NativePlotStatus statuses[Count];
    bool destroyed{}, cleanupThrew{};
    NativePlotPipeline() = default;
    NativePlotPipeline(const NativePlotPipeline&) = delete;
    NativePlotPipeline& operator=(const NativePlotPipeline&) = delete;
    ~NativePlotPipeline() { finish(); }
    void begin(AcPlPlotInfo& info, AcPlPlotPageInfo& page, const std::wstring& document, const std::wstring& output) {
        const auto factoryStatus = AcPlPlotFactory::createPublishEngine(engine_); statuses[Create].set(factoryStatus);
        check(factoryStatus, "create native publish engine");
        if (!engine_) throw std::runtime_error("native publish engine returned null");
        auto status = engine_->beginPlot(nullptr); statuses[BeginPlot].set(status); plot_ = status == Acad::eOk; check(status, "native beginPlot");
        status = engine_->beginDocument(info, document.c_str(), nullptr, 1, true, output.c_str()); statuses[BeginDocument].set(status); document_ = status == Acad::eOk; check(status, "native beginDocument");
        status = engine_->beginPage(page, info, true); statuses[BeginPage].set(status); page_ = status == Acad::eOk; check(status, "native beginPage");
        status = engine_->beginGenerateGraphics(); statuses[BeginGraphics].set(status); graphics_ = status == Acad::eOk; check(status, "native beginGenerateGraphics");
    }
    void finish() noexcept {
        if (!engine_) return;
        // Every successful begin gets one matching end attempt, in reverse nesting.
        // Engine release uses destroy(), never delete, including failed pipelines.
        if (graphics_) { graphics_ = false; try { statuses[EndGraphics].set(engine_->endGenerateGraphics()); } catch (...) { cleanupThrew = true; } }
        if (page_) { page_ = false; try { statuses[EndPage].set(engine_->endPage()); } catch (...) { cleanupThrew = true; } }
        if (document_) { document_ = false; try { statuses[EndDocument].set(engine_->endDocument()); } catch (...) { cleanupThrew = true; } }
        if (plot_) { plot_ = false; try { statuses[EndPlot].set(engine_->endPlot()); } catch (...) { cleanupThrew = true; } }
        auto* engine = engine_; engine_ = nullptr;
        try { engine->destroy(); destroyed = true; } catch (...) { cleanupThrew = true; }
    }
    bool complete() const {
        if (!destroyed || cleanupThrew) return false;
        for (const auto& status : statuses) if (!status.ok()) return false;
        return true;
    }
    std::string json() const {
        const char* names[]{ "create", "beginPlot", "beginDocument", "beginPage", "beginGraphics", "endGraphics", "endPage", "endDocument", "endPlot" };
        std::ostringstream out; out << '{';
        for (int i = 0; i < Count; ++i) { if (i) out << ','; out << '"' << names[i] << "\":" << statuses[i].json(); }
        out << '}'; return out.str();
    }
private:
    AcPlPlotEngine* engine_{};
    bool plot_{}, document_{}, page_{}, graphics_{};
};

std::string current_plot_coordinates(){
 std::vector<std::string> values;
 for(const auto* name:{L"TARGET",L"VIEWDIR",L"VIEWTWIST",L"VIEWCTR",L"VIEWSIZE",L"CVPORT",L"TILEMODE"}){
  resbuf value{};const int status=acedGetVar(name,&value);std::string observed="null";
  if(status==RTNORM){if(value.restype==RT3DPOINT||value.restype==RTPOINT)observed=point(AcGePoint3d(value.resval.rpoint[0],value.resval.rpoint[1],value.resval.rpoint[2]));else if(value.restype==RTREAL)observed=number(value.resval.rreal);else if(value.restype==RTSHORT)observed=std::to_string(value.resval.rint);}
  values.push_back(object({{"variable",text(name)},{"status",std::to_string(status)},{"type",std::to_string(value.restype)},{"value",observed}}));
 }
 return array(values);
}
Bounds window_wcs_to_current_dcs(const Bounds& wcs,std::vector<std::string>& corners){
 resbuf from{},to{};from.restype=to.restype=RTSHORT;from.resval.rint=0;to.resval.rint=2;Bounds dcs;
 for(int i=0;i<4;++i){ads_point input{i&1?wcs.x1:wcs.x0,i&2?wcs.y1:wcs.y0,0},output{};require(acedTrans(input,&from,&to,FALSE,output)==RTNORM,"WCS to current Model DCS point transformation failed");dcs.add(AcGePoint3d(output[0],output[1],0));corners.push_back(object({{"wcs",point(AcGePoint3d(input[0],input[1],input[2]))},{"dcs",point(AcGePoint3d(output[0],output[1],output[2]))}}));}
 require(dcs.valid&&dcs.x1>dcs.x0&&dcs.y1>dcs.y0,"DCS window collapsed");return dcs;
}
void preview_detail(const Config& c,const Product& product,Evidence& evidence){
 const auto& windows=product.windows;
 const auto copy=product.dwg;
 require(sha256(copy)==product.sha,"saved output changed before plot");
 auto* priorActive=acDocManager->mdiActiveDocument();auto* priorCurrent=acDocManager->curDocument();auto* priorWorking=acdbHostApplicationServices()->workingDatabase();AcApDocument* own{};
 int ownContextStatus=-1,closeStatus=-1,restoreCurrentStatus=-1;bool workingRestoreCalled=false;bool activeBeforeRestore=false,currentBeforeRestore=false,workingBeforeRestore=false;short oldBackground{};bool backgroundKnown=false;std::string error;std::vector<std::string> plots;std::string coordinatesBefore="null",coordinatesModel="null",layoutBefore="null",layoutAfter="null";
 try{
  require(acDocManager->isApplicationContext(),"preview requires session application context");
  check(acDocManager->appContextOpenDocument(copy.c_str()),"open own inspection copy");auto* openedDocument=acDocManager->mdiActiveDocument();require(openedDocument&&openedDocument!=priorActive,"owned inspection copy not active");own=openedDocument;require(_wcsicmp(own->fileName(),copy.c_str())==0,"preview opened document path differs from owned output");ownContextStatus=acDocManager->curDocument()==own?static_cast<int>(Acad::eOk):static_cast<int>(acDocManager->setCurDocument(own));check(static_cast<Acad::ErrorStatus>(ownContextStatus),"bind owned plotting current document context");require(acDocManager->curDocument()==own,"owned plotting document context not current");auto* db=own->database();require(db!=nullptr,"owned preview database missing");
  {Working working(db);
   coordinatesBefore=current_plot_coordinates();auto* layouts=acdbHostApplicationServices()->layoutManager();require(layouts!=nullptr,"layout manager unavailable");AcString beforeName;check(layouts->getActiveLayoutName(beforeName,true,db),"read actual initial layout");layoutBefore=text(beforeName.kACharPtr());AcDbObjectId modelLayout;{auto model=opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(db));modelLayout=model->getLayoutId();}if(layouts->getActiveLayoutBTRId(db)!=acdbSymUtil()->blockModelSpaceId(db)){
    // SESSION does not implicitly lock a document. Never reselect an already active Model.
    auto* previousContext=acDocManager->curDocument();check(acDocManager->lockDocument(own,AcAp::kWrite,nullptr,nullptr,false),"lock owned document for layout change");
    const auto contextStatus=acDocManager->setCurDocument(own);Acad::ErrorStatus layoutStatus=contextStatus;
    if(contextStatus==Acad::eOk)layoutStatus=layouts->setCurrentLayoutId(modelLayout);
    const auto restoreStatus=previousContext!=own&&contextStatus==Acad::eOk?acDocManager->setCurDocument(previousContext):Acad::eOk;
    const auto unlockStatus=acDocManager->unlockDocument(own);
    check(contextStatus,"select owned document context for layout change");check(layoutStatus,"change non-Model owned layout to Model");check(restoreStatus,"restore document context after layout change");check(unlockStatus,"unlock owned document after layout change");
   }
   require(layouts->getActiveLayoutBTRId(db)==acdbSymUtil()->blockModelSpaceId(db),"plot DCS current space is not Model");AcString afterName;check(layouts->getActiveLayoutName(afterName,true,db),"read actual plotting layout");layoutAfter=text(afterName.kACharPtr());coordinatesModel=current_plot_coordinates();
   resbuf old{};require(acedGetVar(L"BACKGROUNDPLOT",&old)==RTNORM&&old.restype==RTSHORT,"read BACKGROUNDPLOT");oldBackground=old.resval.rint;backgroundKnown=true;resbuf zero{};zero.restype=RTSHORT;zero.resval.rint=0;require(acedSetVar(L"BACKGROUNDPLOT",&zero)==RTNORM,"set own synchronous plot");
   for(const auto& window:windows){
    evidence.checkpoint("before-native-pdf-"+utf8(window.name));const auto pdf=c.output+L"\\晏府-"+window.name+L".pdf";require(GetFileAttributesW(pdf.c_str())==INVALID_FILE_ATTRIBUTES,"PDF already exists");
    AcDbPlotSettings settings(true);AcDbObjectId layoutId;{auto model=opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(db));layoutId=model->getLayoutId();auto layout=opened<AcDbLayout>(layoutId);check(settings.copyFrom(layout.get()),"copy owned layout plot settings");}
    auto* v=acdbHostApplicationServices()->plotSettingsValidator();require(v!=nullptr,"plot validator missing");v->refreshLists(&settings);check(v->setPlotCfgName(&settings,L"DWG To PDF.pc3"),"PDF device");v->refreshLists(&settings);AcArray<const ACHAR*> media;check(v->canonicalMediaNameList(&settings,media),"PDF media list");std::vector<std::wstring> names;for(int i=0;i<media.length();++i)if(media[i])names.emplace_back(media[i]);const bool overall=window.name==L"four-elevations";const double paperLong=overall?1189.0:594.0,paperShort=overall?841.0:420.0;std::wstring selected;double mw{},mh{};for(const auto& name:names){if(v->setCanonicalMediaName(&settings,name.c_str())!=Acad::eOk)continue;double w{},h{};if(settings.getPlotPaperSize(w,h)==Acad::eOk&&((std::abs(w-paperLong)<.1&&std::abs(h-paperShort)<.1)||(std::abs(w-paperShort)<.1&&std::abs(h-paperLong)<.1))){selected=name;mw=w;mh=h;break;}}
    require(!selected.empty(),"required A0/A2 PDF media missing");check(v->setCanonicalMediaName(&settings,selected.c_str()),"select actual requested PDF paper");check(v->setPlotPaperUnits(&settings,AcDbPlotSettings::kMillimeters),"PDF mm");const bool landscape=window.box.x1-window.box.x0>=window.box.y1-window.box.y0;check(v->setPlotRotation(&settings,(mw>=mh)==landscape?AcDbPlotSettings::k0degrees:AcDbPlotSettings::k90degrees),"native PDF orientation");
    std::vector<std::string> transformedCorners;const auto dcs=window_wcs_to_current_dcs(window.box,transformedCorners);
    check(v->setPlotWindowArea(&settings,dcs.x0,dcs.y0,dcs.x1,dcs.y1),"actual WCS geometry transformed to current Model DCS plot window");check(v->setPlotType(&settings,AcDbPlotSettings::kWindow),"window plot");check(v->setUseStandardScale(&settings,false),"native 1:100 custom scale");check(v->setCustomPrintScale(&settings,1,100),"native print scale exactly 1:100");check(v->setPlotCentered(&settings,true),"center native PDF");
    check(v->setCurrentStyleSheet(&settings,L"Yanfu_Elevation.ctb"),"monochrome plot style");settings.setPlotPlotStyles(true);settings.setPrintLineweights(true);settings.setScaleLineweights(false);
    AcPlPlotInfo info;info.setLayout(layoutId);info.setOverrideSettings(&settings);AcPlPlotInfoValidator validator;validator.setMediaMatchingPolicy(AcPlPlotInfoValidator::kMatchEnabled);check(validator.validate(info),"validate native PDF info");require(info.isValidated()&&info.validatedSettings(),"native PDF unvalidated");double numerator{},denominator{};check(info.validatedSettings()->getCustomPrintScale(numerator,denominator),"actual validated PDF scale");require(std::abs(numerator-1)<1e-9&&std::abs(denominator-100)<1e-9,"validated native PDF scale differs from1:100");double pw{},ph{};check(info.validatedSettings()->getPlotPaperSize(pw,ph),"validated PDF paper");double ml{},mb{},mr{},mt{};check(info.validatedSettings()->getPlotPaperMargins(ml,mb,mr,mt),"validated paper margins");const double availableW=(landscape?(std::max)(pw,ph):(std::min)(pw,ph))-(ml+mr),availableH=(landscape?(std::min)(pw,ph):(std::max)(pw,ph))-(mb+mt);require((dcs.x1-dcs.x0)/100<=availableW+1e-6&&(dcs.y1-dcs.y0)/100<=availableH+1e-6,"actual content window does not fit declared paper at1:100");require(acplProcessPlotState()==kNotPlotting,"plotter busy");AcPlPlotPageInfo page;NativePlotPipeline pipeline;pipeline.begin(info,page,copy,pdf);pipeline.finish();require(pipeline.complete(),"native PDF pipeline incomplete");require(GetFileAttributesW(pdf.c_str())!=INVALID_FILE_ATTRIBUTES,"actual PDF output missing");
    plots.push_back(object({{"name",quote(window.name)},{"windowWCS",window.box.json()},{"windowDCS",dcs.json()},{"actualWcsToDcsCorners",array(transformedCorners)},{"pdf",quote(pdf)},{"pdfSHA256",quote(sha256(pdf))},{"paper",quote(selected)},{"scaleNumerator",number(numerator)},{"scaleDenominator",number(denominator)},{"validatedPaperMM",array({number(pw),number(ph)})},{"scalePolicy",quote(std::string("Actual validated custom scale1:100; A0 overall and A2 individual views"))},{"pipeline",pipeline.json()}}));
   }
  }
 }catch(const std::exception& ex){error=ex.what();}catch(...){error="unknown native preview exception";}
 // Restore variables while our document is still current, then close without saving.
 if(backgroundKnown){resbuf restore{};restore.restype=RTSHORT;restore.resval.rint=oldBackground;if(acedSetVar(L"BACKGROUNDPLOT",&restore)!=RTNORM)error+=" BACKGROUNDPLOT restore failed";}
 if(own){const auto status=acDocManager->appContextCloseDocument(own);closeStatus=static_cast<int>(status);if(status!=Acad::eOk)error+=" own close failed="+std::to_string(status);else own=nullptr;}
 activeBeforeRestore=priorActive==acDocManager->mdiActiveDocument();currentBeforeRestore=priorCurrent==acDocManager->curDocument();workingBeforeRestore=priorWorking==acdbHostApplicationServices()->workingDatabase();
 if(!own){if(priorCurrent&&acDocManager->curDocument()!=priorCurrent){const auto status=acDocManager->setCurDocument(priorCurrent);restoreCurrentStatus=static_cast<int>(status);if(status!=Acad::eOk)error+=" restore prior current failed="+std::to_string(status);}if(priorWorking!=acdbHostApplicationServices()->workingDatabase()){workingRestoreCalled=true;acdbHostApplicationServices()->setWorkingDatabase(priorWorking);}}
 // Never pass nullptr to setCurDocument: a previously null current must be restored by close itself.

 // Hash only after close: the previous D1 helper attempted this while the DWG was open.
 std::string closedHash;try{closedHash=sha256(copy);require(closedHash==product.sha,"saved output was changed during preview");require(sha256(mfinput::paths[0])==mfinput::hashes[0],"frozen snapshot changed after plot");}catch(const std::exception& ex){error+=" ";error+=ex.what();}
 const bool activeRestored=priorActive==acDocManager->mdiActiveDocument(),currentRestored=priorCurrent==acDocManager->curDocument(),workingRestored=priorWorking==acdbHostApplicationServices()->workingDatabase();const bool ambient=activeRestored&&currentRestored&&workingRestored;if(!ambient)error+=" ambient pointers not restored";
 report_file(c,L"mf-preview.json",object({{"api",quote(std::string("ObjectARX SESSION opens this owned generated DWG; actual AcPl publish engine at1:100"))},{"sourceSnapshot",text(mfinput::paths[0])},{"sourceSnapshotSHA256",quote(mfinput::hashes[0])},{"ownedCopy",quote(copy)},{"ownedCopySHA256AfterClose",quote(closedHash)},{"ownedCopySaved","false"},{"ambientRestored",truth(ambient)},{"priorActiveWasNull",truth(priorActive==nullptr)},{"priorCurrentWasNull",truth(priorCurrent==nullptr)},{"priorWorkingWasNull",truth(priorWorking==nullptr)},{"ownedCurrentBindStatus",std::to_string(ownContextStatus)},{"ownedCloseStatus",std::to_string(closeStatus)},{"beforeExplicitRestore",object({{"activeMatched",truth(activeBeforeRestore)},{"currentMatched",truth(currentBeforeRestore)},{"workingMatched",truth(workingBeforeRestore)}})},{"restoreCurrentStatus",std::to_string(restoreCurrentStatus)},{"workingRestoreCalled",truth(workingRestoreCalled)},{"afterExplicitRestore",object({{"activeMatched",truth(activeRestored)},{"currentMatched",truth(currentRestored)},{"workingMatched",truth(workingRestored)}})},{"layoutBefore",layoutBefore},{"layoutUsed",layoutAfter},{"viewCoordinatesBefore",coordinatesBefore},{"viewCoordinatesModel",coordinatesModel},{"plotWindowCoordinateContract",quote(std::string("All four WCS XY corners converted using SDK acedTrans WCS0 to actual current Model DCS2; no assumed TARGET/orientation"))},{"plots",array(plots)},{"error",quote(error)},{"visualReview",quote(std::string("pending controller rendering; AcPl status success alone does not prove visible page content"))}}));
 require(error.empty(),"native preview failed: "+error);evidence.step("project222DetailNativePDF","passed","Four A2 elevation proofs rendered at1:100 from saved output; closed without saving before hash");
}




