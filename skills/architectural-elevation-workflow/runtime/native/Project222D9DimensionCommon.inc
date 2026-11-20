// One-off copy of N8a public helpers; original sources frozen in receipt.
// N8a isolated native probe. RED deliberately leaves the real TCH seed unadapted.
#include "NativeEvidence.h"
#include "DimensionTopology.h"
#include "RejectionAcceptance.h"
#include <aced.h>
#include <adscodes.h>
#include <acutmem.h>
#include <acutads.h>
#include <acdocman.h>
#include <dbapserv.h>
#include <dbdim.h>
#include <dbents.h>
#include <dbidmap.h>
#include <dbsymtb.h>
#include <dbsymutl.h>
#include <dbmtext.h>
#include <rxregsvc.h>
#include <algorithm>
#include <cmath>
#include <exception>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <locale>
#include <memory>
#include <map>
#include <set>
#include <sstream>

namespace {
using namespace bce::native;
HMODULE moduleHandle{}; // Borrowed module identity; never FreeLibrary.
constexpr double tolerance = 1e-6;
void check(Acad::ErrorStatus status, const char* name) {
    require(status == Acad::eOk, std::string(name) + " status=" + std::to_string(status));
}
std::string number(double value) {
    require(std::isfinite(value), "nonfinite observed number");
    std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17) << value; return out.str();
}
std::string point(const AcGePoint3d& value) { return array({number(value.x), number(value.y), number(value.z)}); }
std::string text(const wchar_t* value) { return value ? quote(std::wstring(value)) : "null"; }
struct Close { void operator()(AcDbObject* value) const noexcept { if (value) value->close(); } };
template<class T> using Opened = std::unique_ptr<T, Close>;
template<class T> Opened<T> opened(AcDbObjectId id, AcDb::OpenMode mode = AcDb::kForRead) {
    T* value{}; const auto status = acdbOpenObject(value, id, mode); Opened<T> result(value);
    check(status, "open database object"); require(value != nullptr, "null opened object"); return result;
}
class Working final {
public:
    explicit Working(AcDbDatabase* value) : previous_(acdbHostApplicationServices()->workingDatabase()) {
        acdbHostApplicationServices()->setWorkingDatabase(value);
    }
    ~Working() noexcept { acdbHostApplicationServices()->setWorkingDatabase(previous_); }
    Working(const Working&) = delete; Working& operator=(const Working&) = delete;
private: AcDbDatabase* previous_;
};
struct Exploded final {
    AcDbVoidPtrArray values;
    std::vector<std::unique_ptr<AcDbObject>> owned;
    std::vector<AcDbEntity*> accepted;
    Exploded() = default; Exploded(const Exploded&) = delete; Exploded& operator=(const Exploded&) = delete;
};
struct ObservationLog { const Config& config; Evidence& evidence; unsigned sequence{}; };
std::string handle(AcDbObjectId id) {
    if (id.isNull()) return "null";
    wchar_t value[32]{}; id.handle().getIntoAsciiBuffer(value); return text(value);
}
bool capture_returns(Exploded& exploded, const AcDbEntity* source, ObservationLog& log) {
    bool safe = true; std::set<const void*> seen; std::vector<std::string> records;
    for (int i = 0; i < exploded.values.length(); ++i) {
        auto* raw = exploded.values[i];
        const bool duplicate = raw && !seen.insert(raw).second;
        const bool self = raw == source;
        auto* object = raw && !self && !duplicate ? AcDbObject::cast(static_cast<AcRxObject*>(raw)) : nullptr;
        auto* entity = object ? AcDbEntity::cast(object) : nullptr;
        // Actual 7d6a8f4a: exact AcDbAlignedDimension returns have all three AcDb
        // ownership fields null while isPersistent is true. SDK2020
        // dbobjptr.h:564-580 and N1 use AcDb objectId/ownerId/database for lifetime.
        // Retain isPersistent as raw evidence; it does not veto this proven
        // detached AcDbObject class. Unknown types and foreign objects stay borrowed.
        const bool detached = object && object->objectId().isNull() && object->ownerId().isNull() && object->database() == nullptr;
        const bool persistent = object && object->isPersistent();
        const bool expectedType = entity && entity->isA() == AcDbAlignedDimension::desc();
        const bool owned = detached && expectedType;
        records.push_back(object ? bce::native::object({{"index", std::to_string(i)}, {"acDbObjectCast", "true"},
            {"acDbEntityCast", entity ? "true" : "false"}, {"class", text(object->isA()->name())},
            {"objectId", handle(object->objectId())}, {"ownerId", handle(object->ownerId())},
            {"databaseNull", object->database() == nullptr ? "true" : "false"},
            {"databaseMatchesSource", object->database() == source->database() ? "true" : "false"},
            {"isPersistent", persistent ? "true" : "false"},
            {"detachedByAcDbOwnership", detached ? "true" : "false"}, {"exactExpectedSegmentType", expectedType ? "true" : "false"},
            {"deleteOwned", owned ? "true" : "false"}})
            : bce::native::object({{"index", std::to_string(i)}, {"null", raw ? "false" : "true"},
                {"self", self ? "true" : "false"}, {"duplicate", duplicate ? "true" : "false"},
                {"acDbObjectCast", "false"}, {"deleteOwned", "false"}}));
        if (owned) exploded.owned.emplace_back(object);
        if (owned && entity && !self && !duplicate) exploded.accepted.push_back(entity); else safe = false;
    }
    ++log.sequence;
    const auto path = log.config.output + L"\\explode-ownership-" + std::to_wstring(log.sequence) + L".json";
    auto file = create_new(path);
    write_flushed(file.get(), object({{"contract", quote(std::string("bce.dimension-explode-ownership.v1"))},
        {"sourceClass", text(source->isA()->name())}, {"sourceId", handle(source->objectId())},
        {"returns", array(records)}, {"ownershipSafe", safe ? "true" : "false"},
        {"ownedDetachedCount", std::to_string(exploded.owned.size())}}));
    log.evidence.step("explodeOwnership" + std::to_string(log.sequence), safe ? "passed" : "failed", utf8(path));
    return safe;
}
std::string dimension_resources(const AcDbDimension& dimension);
std::string public_dimension_text(const AcDbDimension& dimension);
std::string textstyle_shape(AcDbObjectId id);
struct Segment { AcGePoint3d first, second, baseline; double measurement{}, factor{}, textHeight{}; std::wstring formatted, overrideText; AcDbObjectId layerId,dimStyleId,textStyleId; std::string json, comparisonJson,textStyleShape; };
struct Snapshot { std::vector<Segment> segments; std::vector<AcGePoint3d> nodes, grips; AcDbObjectId rootLayerId; std::string json, comparisonJson; };
bool points_close(const AcGePoint3d& first, const AcGePoint3d& second) { return first.distanceTo(second) <= tolerance; }
Snapshot observe(AcDbObjectId id, ObservationLog& log) {
    auto root = opened<AcDbEntity>(id);
    require(!root->isAProxy() && std::wstring(root->isA()->name()) == L"TDbDimension2", "root must be real TDbDimension2");
    require(root->ownerId() == acdbSymUtil()->blockModelSpaceId(root->database()), "root must be directly in model space");
    AcGePoint3dArray grips; AcDbIntArray osnap, geometry;
    const auto gripStatus = root->getGripPoints(grips, osnap, geometry); check(gripStatus, "getGripPoints");
    std::vector<std::string> gripJson;
    for (int i = 0; i < grips.length(); ++i) gripJson.push_back(point(grips[i]));
    Exploded exploded;
    log.evidence.checkpoint("before-public-dimension-explode-" + std::to_string(log.sequence + 1));
    Acad::ErrorStatus explodeStatus = Acad::eNotApplicable;
    try { explodeStatus = root->explode(exploded.values); }
    catch (...) { capture_returns(exploded, root.get(), log); throw; }
    require(capture_returns(exploded, root.get(), log), "explode returned unresolved/duplicate/self/non-detached object; foreign objects not deleted");
    check(explodeStatus, "explode real dimension");
    Snapshot result; result.rootLayerId = root->layerId();
    for (int i=0;i<grips.length();++i) result.grips.push_back(grips[i]);
    std::vector<std::string> segmentJson, comparisonSegments;
    for (auto* entity : exploded.accepted) {
        auto* dim = AcDbAlignedDimension::cast(entity); require(dim != nullptr, "explode segment not AcDbAlignedDimension");
        Segment segment; segment.first = dim->xLine1Point(); segment.second = dim->xLine2Point(); segment.baseline = dim->dimLinePoint();
        const auto measurementStatus = dim->measurement(segment.measurement); check(measurementStatus, "measurement");
        AcString overrideText, formatted, layer;
        check(dim->layer(layer), "segment layer");
        const auto textStatus = dim->dimensionText(overrideText); check(textStatus, "dimensionText");
        const auto formatStatus = dim->formatMeasurement(formatted, segment.measurement, overrideText.kACharPtr()); check(formatStatus, "formatMeasurement");
        segment.formatted = formatted.kACharPtr(); segment.overrideText = overrideText.kACharPtr();
        segment.factor = dim->dimlfac(); segment.textHeight = dim->dimtxt();
        segment.textStyleShape=textstyle_shape(dim->dimtxsty());
        segment.layerId = dim->layerId();
        segment.dimStyleId=dim->dimensionStyle();segment.textStyleId=dim->dimtxsty();
        for(const auto reference:{segment.layerId,segment.dimStyleId,segment.textStyleId})require(!reference.isNull()&&reference.database()==id.database(),"segment_public_resource_reference_foreign_or_null");
        Fields fields{{"first", point(segment.first)}, {"second", point(segment.second)}, {"baseline", point(segment.baseline)},
            {"measurement", number(segment.measurement)}, {"measurementStatus", std::to_string(measurementStatus)},
            {"formatMeasurement", text(formatted.kACharPtr())}, {"formatStatus", std::to_string(formatStatus)},
            {"dimensionText", text(overrideText.kACharPtr())}, {"dimensionTextStatus", std::to_string(textStatus)},
            {"dimlfac", number(dim->dimlfac())}, {"dimdec", std::to_string(dim->dimdec())}, {"dimscale", number(dim->dimscale())},
            {"dimtxt", number(dim->dimtxt())}, {"dimasz", number(dim->dimasz())}, {"dimexo", number(dim->dimexo())},
            {"dimexe", number(dim->dimexe())}, {"dimgap", number(dim->dimgap())}, {"dimtad", std::to_string(dim->dimtad())},
            {"dimjust", std::to_string(dim->dimjust())},{"paperTextStyle",segment.textStyleShape}};
        segment.comparisonJson = object(fields); comparisonSegments.push_back(segment.comparisonJson);
        fields.emplace_back("layer", text(layer.kACharPtr()));
        fields.emplace_back("readResources", dimension_resources(*dim));
        fields.emplace_back("publicReadableText", public_dimension_text(*dim)); segment.json = object(fields);
        result.segments.push_back(segment); segmentJson.push_back(segment.json);
        for (const auto& candidate : {segment.first, segment.second}) {
            if (std::none_of(result.nodes.begin(), result.nodes.end(), [&](const AcGePoint3d& existing) { return points_close(candidate, existing); })) result.nodes.push_back(candidate);
        }
    }
    require(!result.segments.empty() && result.nodes.size() == result.segments.size()+1, "segments must form a full node path");
    std::sort(result.nodes.begin(), result.nodes.end(), [](const AcGePoint3d& a, const AcGePoint3d& b) { return a.x==b.x ? a.y<b.y : a.x<b.x; });
    std::vector<std::string> nodes;
    for (const auto& node : result.nodes) nodes.push_back(point(node));
    result.comparisonJson = object({{"grips", array(gripJson)}, {"nodes", array(nodes)}, {"segments", array(comparisonSegments)}});
    AcGePoint3dArray stretch; const auto stretchStatus=root->getStretchPoints(stretch); std::vector<std::string> stretchJson;
    if(stretchStatus==Acad::eOk) for(int i=0;i<stretch.length();++i)stretchJson.push_back(point(stretch[i]));
    result.json = object({{"rootClass", text(root->isA()->name())}, {"proxy", "false"}, {"directModelRoot", "true"},
        {"publicAcDbDimensionCast",AcDbDimension::cast(root.get())?"true":"false"},
        {"rootLayerId", handle(root->layerId())}, {"rootColorIndex", std::to_string(root->colorIndex())},
        {"rootLinetypeId", handle(root->linetypeId())}, {"rootLinetypeScale", number(root->linetypeScale())},
        {"rootLineWeight", std::to_string(root->lineWeight())}, {"rootVisibility", std::to_string(root->visibility())},
        {"stretchStatus", std::to_string(stretchStatus)}, {"stretchPoints", array(stretchJson)},
        {"gripStatus", std::to_string(gripStatus)}, {"grips", array(gripJson)}, {"explodeStatus", std::to_string(explodeStatus)},
        {"nodes", array(nodes)}, {"segments", array(segmentJson)}, {"nativeGlyphObservation", "null"},
        {"nativeGlyphGap", quote(std::string("No host text request captured in this RED probe; formatted segment text is not a glyph observation"))}});
    return result;
}
struct LayerObservation { std::string name, definition; AcDbObjectId linetype; };
LayerObservation layer_observation(AcDbObjectId id) {
    require(!id.isNull() && id.database() != nullptr, "layer reference must resolve to persistent symbol");
    Working working(id.database()); auto layer = opened<AcDbLayerTableRecord>(id);
    AcString name; check(layer->getName(name), "layer getName");
    const auto color = layer->color();
    return {text(name.kACharPtr()), object({{"colorMethod", std::to_string(color.colorMethod())},
        {"colorIndex", std::to_string(color.colorIndex())}, {"red", std::to_string(color.red())},
        {"green", std::to_string(color.green())}, {"blue", std::to_string(color.blue())},
        {"lineWeight", std::to_string(layer->lineWeight())}, {"off", layer->isOff() ? "true" : "false"},
        {"frozen", layer->isFrozen() ? "true" : "false"}, {"locked", layer->isLocked() ? "true" : "false"},
        {"plottable", layer->isPlottable() ? "true" : "false"}}), layer->linetypeObjectId()};
}
std::string validate_layer_map(AcDbObjectId source, AcDbObjectId target, AcDbIdMapping& mapping) {
    AcDbIdPair pair; pair.setKey(source);
    require(mapping.compute(pair) && pair.value() == target, "actual clone layer mapping mismatch");
    const auto before = layer_observation(source), after = layer_observation(target);
    require(before.definition == after.definition, "mapped layer public definition mismatch");
    AcDbIdPair linetype; linetype.setKey(before.linetype);
    require(mapping.compute(linetype) && linetype.value() == after.linetype, "mapped layer linetype dependency mismatch");
    return object({{"sourceId", handle(source)}, {"targetId", handle(target)}, {"sourceName", before.name}, {"targetName", after.name},
        {"isCloned", pair.isCloned() ? "true" : "false"}, {"isOwnerTranslated", pair.isOwnerXlated() ? "true" : "false"},
        {"sourceDefinition", before.definition}, {"targetDefinition", after.definition},
        {"sourceLinetypeId", handle(before.linetype)}, {"targetLinetypeId", handle(after.linetype)}, {"linetypeMappingVerified", "true"}});
}
// Included only by this isolated native probe, inside its private namespace.
constexpr bool mappingDiagnosticOnly = false;
std::string truth(bool value) { return value ? "true" : "false"; }
n8a::Point plain(const AcGePoint3d& p) { return {p.x,p.y,p.z}; }
n8a::Point plain(const AcGeVector3d& p) { return {p.x,p.y,p.z}; }
AcGePoint3d native_point(n8a::Point p) { return {p.x,p.y,p.z}; }
std::string textstyle_shape(AcDbObjectId id) {
    if(id.isNull())return "null";auto style=opened<AcDbTextStyleTableRecord>(id);AcString font,bigfont;
    check(style->fileName(font),"effective style font");check(style->bigFontFileName(bigfont),"effective style bigfont");
    return object({{"font",text(font.kACharPtr())},{"bigfont",text(bigfont.kACharPtr())},{"widthFactor",number(style->xScale())},{"oblique",number(style->obliquingAngle())},{"vertical",truth(style->isVertical()!=0)},{"textSize",number(style->textSize())},{"statusGetters", "0"}});
}
std::string style_record(AcDbObjectId id) {
    if(id.isNull())return "null";
    auto style=opened<AcDbTextStyleTableRecord>(id);AcString name,font,bigfont;
    check(style->getName(name),"style name");check(style->fileName(font),"style font");check(style->bigFontFileName(bigfont),"style bigfont");
    return object({{"id",handle(id)},{"name",text(name.kACharPtr())},{"font",text(font.kACharPtr())},{"bigfont",text(bigfont.kACharPtr())},
        {"textSize",number(style->textSize())},{"widthFactor",number(style->xScale())},{"oblique",number(style->obliquingAngle())},
        {"vertical",truth(style->isVertical()!=0)},{"shapeFile",truth(style->isShapeFile()!=0)}});
}
template<class Dim> std::string dim_values(const Dim& dim) {
    return object({{"dimlfac",number(dim.dimlfac())},{"dimdec",std::to_string(dim.dimdec())},{"dimscale",number(dim.dimscale())},
        {"dimtxt",number(dim.dimtxt())},{"dimasz",number(dim.dimasz())},{"dimexo",number(dim.dimexo())},{"dimexe",number(dim.dimexe())},
        {"dimgap",number(dim.dimgap())},{"dimtad",std::to_string(dim.dimtad())},{"dimjust",std::to_string(dim.dimjust())},{"dimtxsty",handle(dim.dimtxsty())}});
}
std::string dimstyle_record(AcDbObjectId id) {
    if(id.isNull())return "null";auto style=opened<AcDbDimStyleTableRecord>(id);AcString name;check(style->getName(name),"dimstyle name");
    return object({{"id",handle(id)},{"name",text(name.kACharPtr())},{"declaredPublicValues",dim_values(*style)},
        {"textStyle",style_record(style->dimtxsty())}});
}
std::string dimension_resources(const AcDbDimension& dimension) {
    auto base=opened<AcDbDimStyleTableRecord>(dimension.dimensionStyle());
    return object({{"baseDimStyle",dimstyle_record(dimension.dimensionStyle())},{"effectiveTextStyle",style_record(dimension.dimtxsty())},
        {"effectiveValues",dim_values(dimension)},{"effectiveDiffersFromBase",truth(dim_values(dimension)!=dim_values(*base))},
        {"coverage",quote(std::string("Only the explicitly listed public variables and resources; not all TCH fields or all DimStyle variables"))}});
}
std::string public_dimension_text(const AcDbDimension& dimension) {
    std::vector<std::string> texts;const auto blockId=dimension.dimBlockId();
    if(!blockId.isNull()) {
        auto block=opened<AcDbBlockTableRecord>(blockId);AcDbBlockTableRecordIterator* raw{};check(block->newIterator(raw),"public dim block iterator");
        std::unique_ptr<AcDbBlockTableRecordIterator> it(raw);
        for(;!it->done();it->step()) {AcDbObjectId id;check(it->getEntityId(id),"public dim block child");auto child=opened<AcDbEntity>(id);
            if(const auto* t=AcDbText::cast(child.get())){AcString content;check(t->textString(content),"public AcDbText string");texts.push_back(object({{"class",text(t->isA()->name())},{"text",text(content.kACharPtr())},{"height",number(t->height())},{"widthFactor",number(t->widthFactor())},{"position",point(t->position())},{"style",style_record(t->textStyle())}}));}
            else if(const auto* multi=AcDbMText::cast(child.get())){AcString content;check(multi->contents(content),"public MText contents");texts.push_back(object({{"class",text(multi->isA()->name())},{"contents",text(content.kACharPtr())},{"height",number(multi->textHeight())},{"position",point(multi->location())},{"style",style_record(multi->textStyle())}}));}
        }
    }
    return object({{"dimBlockId",handle(blockId)},{"texts",array(texts)},{"status",quote(std::string(texts.empty()?"no_public_text_host_request_missing":"public_dimblock_text_observed_host_request_still_missing"))}});
}
std::string layer_record(AcDbObjectId id) {const auto layer=layer_observation(id);return object({{"id",handle(id)},{"name",layer.name},{"definition",layer.definition},{"linetypeId",handle(layer.linetype)}});}
std::string linetype_record(AcDbObjectId id) {
    auto type=opened<AcDbLinetypeTableRecord>(id);AcString name;check(type->getName(name),"linetype name");std::vector<std::string> dashes;
    for(int i=0;i<type->numDashes();++i)dashes.push_back(number(type->dashLengthAt(i)));
    return object({{"id",handle(id)},{"name",text(name.kACharPtr())},{"patternLength",number(type->patternLength())},{"dashLengths",array(dashes)}});
}
template<class F> std::string table_records(AcDbObjectId id,F f) {
    auto table=opened<AcDbSymbolTable>(id);AcDbSymbolTableIterator* raw{};check(table->newIterator(raw),"symbol table iterator");std::unique_ptr<AcDbSymbolTableIterator> it(raw);std::vector<std::string> rows;
    for(;!it->done();it->step()){AcDbObjectId record;check(it->getRecordId(record),"symbol record id");rows.push_back(f(record));}return array(rows);
}
std::vector<AcDbObjectId> model_roots(AcDbDatabase& db) {
    auto model=opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(&db));AcDbBlockTableRecordIterator* raw{};check(model->newIterator(raw),"model iterator");std::unique_ptr<AcDbBlockTableRecordIterator> it(raw);std::vector<AcDbObjectId> ids;
    for(;!it->done();it->step()){AcDbObjectId id;check(it->getEntityId(id),"model root id");ids.push_back(id);}return ids;
}
std::string symbol_identity(AcDbObjectId id){auto value=opened<AcDbSymbolTableRecord>(id);AcString name;check(value->getName(name),"protected symbol key");return object({{"id",handle(id)},{"ownerId",handle(value->ownerId())},{"class",text(value->isA()->name())},{"key",text(name.kACharPtr())}});}
std::string database_state(AcDbDatabase& db,bool protectedIdentityOnly=false) {
    Working working(&db);std::vector<std::string> roots;
    for(const auto id:model_roots(db)){auto root=opened<AcDbEntity>(id);Fields fields{{"id",handle(id)},{"class",text(root->isA()->name())},{"owner",handle(root->ownerId())},{"layer",handle(root->layerId())},{"linetype",handle(root->linetypeId())},{"colorIndex",std::to_string(root->colorIndex())},{"lineWeight",std::to_string(root->lineWeight())},{"visible",std::to_string(root->visibility())}};
        if(!protectedIdentityOnly){AcGePoint3dArray grips;AcDbIntArray osnap,geometry;const auto status=root->getGripPoints(grips,osnap,geometry);std::vector<std::string> points;if(status==Acad::eOk)for(int i=0;i<grips.length();++i)points.push_back(point(grips[i]));fields.emplace_back("gripStatus",std::to_string(status));fields.emplace_back("grips",array(points));}
        if(const auto* t=AcDbText::cast(root.get())){AcString content;check(t->textString(content),"state text");fields.emplace_back("text",text(content.kACharPtr()));fields.emplace_back("position",point(t->position()));fields.emplace_back("alignment",point(t->alignmentPoint()));fields.emplace_back("height",number(t->height()));fields.emplace_back("widthFactor",number(t->widthFactor()));fields.emplace_back("rotation",number(t->rotation()));fields.emplace_back("style",protectedIdentityOnly?symbol_identity(t->textStyle()):style_record(t->textStyle()));}
        roots.push_back(object(fields));}
    return object({{"readPolicy",quote(std::string(protectedIdentityOnly?"protected_common_identity_and_symbol_owner_keys_no_grips_explode_fonts":"owned_full_declared_public_observations"))},{"modelRoots",array(roots)},{"insunits",std::to_string(db.insunits())},
        {"layers",table_records(db.layerTableId(),protectedIdentityOnly?symbol_identity:layer_record)},
        {"textStyles",table_records(db.textStyleTableId(),protectedIdentityOnly?symbol_identity:style_record)},
        {"linetypes",table_records(db.linetypeTableId(),protectedIdentityOnly?symbol_identity:linetype_record)},
        {"dimStyles",table_records(db.dimStyleTableId(),protectedIdentityOnly?symbol_identity:dimstyle_record)}});
}
struct WriteMonitor final:AcDbDatabaseReactor {
    AcDbDatabase& db;unsigned opened{},modified{},appended{},erased{},unappended{},reappended{},header{};
    explicit WriteMonitor(AcDbDatabase& value):db(value){db.addReactor(this);}~WriteMonitor() noexcept {db.removeReactor(this);}
    void objectOpenedForModify(const AcDbDatabase*,const AcDbObject*)override{++opened;}
    void objectModified(const AcDbDatabase*,const AcDbObject*)override{++modified;}
    void objectAppended(const AcDbDatabase*,const AcDbObject*)override{++appended;}
    void objectErased(const AcDbDatabase*,const AcDbObject*,Adesk::Boolean)override{++erased;}
    void objectUnAppended(const AcDbDatabase*,const AcDbObject*)override{++unappended;}
    void objectReAppended(const AcDbDatabase*,const AcDbObject*)override{++reappended;}
    void headerSysVarWillChange(const AcDbDatabase*,const ACHAR*)override{++header;}
    bool clean()const{return opened+modified+appended+erased+unappended+reappended+header==0;}
    std::string json()const{return object({{"openedForModify",std::to_string(opened)},{"modified",std::to_string(modified)},{"appended",std::to_string(appended)},{"erased",std::to_string(erased)},{"unappended",std::to_string(unappended)},{"reappended",std::to_string(reappended)},{"headerWillChange",std::to_string(header)}});}
};
void report_file(const Config& config,const std::wstring& name,const std::string& json){auto file=create_new(config.output+L"\\"+name);write_flushed(file.get(),json);}
struct Fixture {
    std::string name;std::wstring seed;int segmentCount{};AcGePoint3d origin;AcGeVector3d direction;
    std::vector<double> stations;double lateral{},scaleDenominator{100},unitToMm{1};std::string unit{"mm"},unitSource{"explicit project fixture"},placementSource{"explicit per-chain fixture"};bool noncollinear{};
    std::vector<AcGePoint3d> anchors()const{std::vector<AcGePoint3d> points;for(auto coordinate:stations)points.push_back(origin+direction*coordinate);if(noncollinear&&points.size()>2)points[1]+=AcGeVector3d(-direction.y,direction.x,0)*37.25;return points;}
    std::string json()const{std::vector<std::string> coordinates,points,projected;for(auto coordinate:stations){coordinates.push_back(number(coordinate));projected.push_back(point(origin+direction*coordinate));}for(const auto& p:anchors())points.push_back(point(p));return object({{"name",quote(name)},{"seed",quote(seed)},{"segmentCount",std::to_string(segmentCount)},{"origin",point(origin)},{"direction",array({number(direction.x),number(direction.y),number(direction.z)})},{"coordinateInput",array(coordinates)},{"projectedPoints",array(projected)},{"sourcePoints",array(points)},{"lateralOffset",number(lateral)},{"baselinePoint",point(origin+AcGeVector3d(-direction.y,direction.x,0)*lateral)},{"unit",quote(unit)},{"unitSource",quote(unitSource)},{"scaleDenominator",number(scaleDenominator)},{"placementSource",quote(placementSource)},{"actualAnchorPolicy",quote(std::string("explicit fixture same-station source anchors; no projection replacement"))}});}
};
void validate_fixture(const Fixture& fixture) {
    require(!fixture.seed.empty(),"request_missing_seed");require(!fixture.placementSource.empty(),"request_missing_placement");require(!fixture.unitSource.empty(),"request_missing_unit_source");
    require(fixture.segmentCount>0&&fixture.stations.size()==static_cast<std::size_t>(fixture.segmentCount+1),"request_wrong_segment_count");
    require(std::abs(fixture.direction.length()-1)<=tolerance&&std::abs(fixture.direction.z)<=tolerance,"request_invalid_direction");
    require(fixture.unitToMm>0&&fixture.scaleDenominator>0,"request_invalid_unit_scale");
    const auto anchors=fixture.anchors();std::vector<double> dummy;
    for(std::size_t i=0;i<fixture.stations.size();++i){require(std::isfinite(fixture.stations[i]),"request_nonfinite_station");if(i)require(fixture.stations[i]>fixture.stations[i-1]+tolerance,"request_duplicate_or_descending_station");
        const auto projected=fixture.origin+fixture.direction*fixture.stations[i];const auto delta=anchors[i]-projected;require(std::abs(delta.dotProduct(fixture.direction))<=tolerance,"actual_anchor_station_projection_mismatch");dummy.push_back(fixture.stations[i]);}
}
std::vector<Fixture> fixtures(){return {
    {"two_mm100_horizontal",L"10837",2,{12000.375,-3400.625,0},{1,0,0},{0,512.375,3001.125},800.5},
    {"one_mm100_vertical_large",L"ACB0",1,{1000000.375,2000000.625,0},{0,1,0},{0,1876.625},-700.125},
    {"six_mm100_horizontal",L"AC83",6,{-45000.375,16000.625,0},{1,0,0},{0,135.375,630.125,1250.875,1999.25,3201.5,4450.625},900.375},
    {"twelve_mm100_vertical_large",L"CAF3",12,{2356789.125,-1456789.375,0},{0,1,0},{0,120.375,555.625,1110.875,1875.125,2720.375,3650.625,4700.875,5861.125,7130.375,8510.625,10000.875,12020.375},-1100.25},
    {"two_mm50_horizontal",L"10837",2,{14200.125,17200.375,0},{1,0,0},{0,710.625,3245.875},600.25,50},
    {"two_m50_horizontal",L"10837",2,{12.000375,-3.400625,0},{1,0,0},{0,.512375,3.001125},.40025,50,1000,"m"},
    {"two_mm100_noncollinear",L"10837",2,{22100.375,6400.625,0},{1,0,0},{0,512.375,3001.125},800.5,100,1,"mm","explicit project fixture","explicit per-chain fixture",true}
};}
n8a::Path topology(const Snapshot& snapshot,const AcGeVector3d& direction) {
    std::vector<n8a::Edge> edges;for(const auto& segment:snapshot.segments)edges.push_back({plain(segment.first),plain(segment.second)});
    auto result=n8a::path(edges,plain(direction));const AcGeVector3d normal(-direction.y,direction.x,0);
    const auto baseline=snapshot.segments.front().baseline;
    for(const auto& segment:snapshot.segments)require(std::abs((segment.baseline-baseline).dotProduct(normal))<=tolerance&&std::abs(segment.baseline.z-baseline.z)<=tolerance,"segment baselines are not one chain line");
    for(const auto node:result.nodes)require(std::abs(node.z-result.nodes.front().z)<=tolerance,"nonplanar node path");return result;
}
AcGeVector3d source_direction(const Snapshot& snapshot){require(snapshot.nodes.size()>=2,"source nodes missing");double greatest{};AcGeVector3d direction;
    for(const auto& a:snapshot.nodes)for(const auto& b:snapshot.nodes)if(a.distanceTo(b)>greatest){greatest=a.distanceTo(b);direction=b-a;}
    require(greatest>tolerance,"zero path span");direction.normalize();if(direction.x<-tolerance||(std::abs(direction.x)<=tolerance&&direction.y<0))direction.negate();return direction;}
std::vector<n8a::Point> plain_grips(const Snapshot& snapshot){std::vector<n8a::Point> points;for(const auto& p:snapshot.grips)points.push_back(plain(p));return points;}
void validate_topology_measurements(const Snapshot& snapshot,const n8a::Path& path,const AcGeVector3d& direction){for(std::size_t i=0;i<path.segments.size();++i){const auto expected=(native_point(path.nodes[i+1])-native_point(path.nodes[i])).dotProduct(direction);require(std::abs(snapshot.segments[path.segments[i]].measurement-expected)<=tolerance,"measurement_does_not_match_station_projection");}}
struct SceneLog {const Config& config;std::string name;unsigned sequence{};std::vector<std::string> actions;
    void attempt(const char* operation,Acad::ErrorStatus status,const Fields& details={}){auto fields=details;fields.emplace_back("operation",quote(std::string(operation)));fields.emplace_back("status",std::to_string(status));actions.push_back(object(fields));}
    void capture(const char* operation,Acad::ErrorStatus status,const Snapshot& value){++sequence;const auto filename=std::wstring(name.begin(),name.end())+L"-observation-"+std::to_wstring(sequence)+L".json";report_file(config,filename,object({{"operation",quote(std::string(operation))},{"status",std::to_string(status)},{"snapshot",value.json}}));actions.push_back(object({{"operation",quote(std::string(operation))},{"status",std::to_string(status)},{"observation",quote(filename)}}));}};
Acad::ErrorStatus move_node(AcDbObjectId root,std::size_t index,const AcGeVector3d& delta){auto object=opened<AcDbEntity>(root,AcDb::kForWrite);AcDbIntArray indices;indices.append(static_cast<int>(index));return object->moveGripPointsAt(indices,delta);}
Snapshot adapt(AcDbObjectId root,const Fixture& fixture,ObservationLog& observations,SceneLog& log) {
    auto current=observe(root,observations);const auto sourceDirection=source_direction(current);const auto originalPath=topology(current,sourceDirection);validate_topology_measurements(current,originalPath,sourceDirection);
    const double uniformScale=fixture.scaleDenominator/(100.0*fixture.unitToMm);
    const auto sourceOrigin=native_point(originalPath.nodes.front());const auto angle=std::atan2(fixture.direction.y,fixture.direction.x)-std::atan2(sourceDirection.y,sourceDirection.x);
    const auto matrix=AcGeMatrix3d::translation(fixture.origin.asVector())*AcGeMatrix3d::rotation(angle,AcGeVector3d::kZAxis)*AcGeMatrix3d::scaling(uniformScale)*AcGeMatrix3d::translation(-sourceOrigin.asVector());
    Acad::ErrorStatus status;{auto entity=opened<AcDbEntity>(root,AcDb::kForWrite);status=entity->transformBy(matrix);}log.attempt("transformBy",status,{{"uniformPaperScale",number(uniformScale)},{"rotationRadians",number(angle)}});check(status,"uniform seed paper/orientation transformBy");
    current=observe(root,observations);log.capture("uniformSeedPaperAndRigidPlacement",status,current);auto path=topology(current,fixture.direction);validate_topology_measurements(current,path,fixture.direction);
    require(path.nodes.size()==fixture.stations.size(),"transformed source/target node count mismatch");
    for(std::size_t i=0;i<path.nodes.size();++i)require(points_close(native_point(path.nodes[i]),matrix*native_point(originalPath.nodes[i])),"transform eOk but actual node differs");
    std::vector<double> sourceCoordinates;for(const auto p:path.nodes)sourceCoordinates.push_back((native_point(p)-fixture.origin).dotProduct(fixture.direction));
    const auto order=n8a::move_order(sourceCoordinates,fixture.stations);std::vector<AcGePoint3d> expected;for(const auto p:path.nodes)expected.push_back(native_point(p));const auto targets=fixture.anchors();
    // Include pure lateral changes even when the station coordinate itself is unchanged.
    auto completeOrder=order;for(std::size_t i=0;i<targets.size();++i)if(std::find(completeOrder.begin(),completeOrder.end(),i)==completeOrder.end()&&!points_close(expected[i],targets[i]))completeOrder.push_back(i);
    for(const auto node:completeOrder){current=observe(root,observations);path=topology(current,fixture.direction);const auto index=n8a::unique_grip(path.nodes[node],plain_grips(current));const auto delta=targets[node]-native_point(path.nodes[node]);const auto moveStatus=move_node(root,index,delta);log.attempt("moveGripPointsAtMeasurement",moveStatus,{{"pathNode",std::to_string(node)},{"actualMatchedGripIndex",std::to_string(index)},{"targetActualAnchor",point(targets[node])},{"offset",array({number(delta.x),number(delta.y),number(delta.z)})}});current=observe(root,observations);log.capture("moveMatchedMeasurementNode",moveStatus,current);check(moveStatus,"moveGripPointsAt measurement");const auto changed=topology(current,fixture.direction);expected[node]=targets[node];
        for(std::size_t i=0;i<expected.size();++i)require(points_close(native_point(changed.nodes[i]),expected[i]),"node move changed another actual measurement anchor");
        validate_topology_measurements(current,changed,fixture.direction);
    }
    current=observe(root,observations);path=topology(current,fixture.direction);const AcGeVector3d normal(-fixture.direction.y,fixture.direction.x,0);const auto targetBaseline=fixture.origin+normal*fixture.lateral;
    const double delta=(targetBaseline-current.segments.front().baseline).dotProduct(normal);
    std::vector<std::size_t> candidates;
    for(std::size_t i=0;i<current.grips.size();++i){const auto& p=current.grips[i];const bool measured=std::any_of(path.nodes.begin(),path.nodes.end(),[&](n8a::Point node){return points_close(p,native_point(node));});const double coordinate=(p-fixture.origin).dotProduct(fixture.direction);
        if(!measured&&std::abs((p-current.segments.front().baseline).dotProduct(normal))<=tolerance&&std::abs(p.z-targetBaseline.z)<=tolerance&&(std::abs(coordinate-fixture.stations.front())<=tolerance||std::abs(coordinate-fixture.stations.back())<=tolerance))candidates.push_back(i);}
    require(candidates.size()==2,"baseline semantics require two distinct endpoint grips on actual segment baseline");
    require(!points_close(current.grips[candidates[0]],current.grips[candidates[1]]),"ambiguous coincident baseline grips");
    const auto baselineStatus=move_node(root,candidates.front(),normal*delta);log.attempt("moveGripPointsAtBaseline",baselineStatus,{{"actualMatchedGripIndex",std::to_string(candidates.front())},{"normalOffset",number(delta)}});const auto baselineMoved=observe(root,observations);log.capture("moveMatchedBaselineOnly",baselineStatus,baselineMoved);check(baselineStatus,"moveGripPointsAt baseline");const auto finalPath=topology(baselineMoved,fixture.direction);
    for(std::size_t i=0;i<targets.size();++i)require(points_close(native_point(finalPath.nodes[i]),targets[i]),"baseline move changed actual source anchor");
    for(const auto& segment:baselineMoved.segments)require(std::abs((segment.baseline-targetBaseline).dotProduct(normal))<=tolerance,"baseline did not reach explicit placement");
    return baselineMoved;
}
struct FlatField {std::string path,raw;bool numeric{};double value{};};
// The scanner compares our own strictly generated JSON, preserving field paths.
// It never reads TCH serialization or treats private data as a business schema.
std::vector<FlatField> flatten(const std::string& json) {
    std::size_t at{};std::vector<FlatField> fields;
    const auto string_token=[&](){const auto begin=at;require(at<json.size()&&json[at++]=='"',"JSON string expected");bool escape=false;while(at<json.size()){const auto c=json[at++];if(c=='"'&&!escape)return json.substr(begin,at-begin);if(c=='\\'&&!escape)escape=true;else escape=false;}throw std::runtime_error("unterminated generated JSON string");};
    std::function<void(const std::string&)> parse;
    parse=[&](const std::string& path){require(at<json.size(),"truncated generated JSON");if(json[at]=='{'){++at;if(json[at]=='}'){++at;fields.push_back({path,"{}",false,0});return;}for(;;){auto key=string_token();require(json[at++]==':',"JSON colon expected");parse(path+"."+key.substr(1,key.size()-2));if(json[at]=='}'){++at;return;}require(json[at++]==',',"JSON object separator expected");}}
        else if(json[at]=='['){++at;if(json[at]==']'){++at;fields.push_back({path,"[]",false,0});return;}std::size_t index{};for(;;){parse(path+"["+std::to_string(index++)+"]");if(json[at]==']'){++at;return;}require(json[at++]==',',"JSON array separator expected");}}
        else if(json[at]=='"')fields.push_back({path,string_token(),false,0});
        else {const auto begin=at;while(at<json.size()&&json[at]!=','&&json[at]!=']'&&json[at]!='}')++at;const auto raw=json.substr(begin,at-begin);const bool numeric=!raw.empty()&&(raw[0]=='-'||(raw[0]>='0'&&raw[0]<='9'));char* end{};const double value=numeric?std::strtod(raw.c_str(),&end):0;require(!numeric||(end==raw.c_str()+raw.size()&&std::isfinite(value)),"invalid generated JSON number");fields.push_back({path,raw,numeric,value});}};
    parse("$");require(at==json.size(),"generated JSON trailing data");return fields;
}
struct FieldComparison {bool equal{};std::string json;};
FieldComparison compare_fields(const std::string& before,const std::string& after) {
    const auto first=flatten(before),second=flatten(after);bool equal=first.size()==second.size();std::vector<std::string> rows;
    const auto count=(std::max)(first.size(),second.size());
    for(std::size_t i=0;i<count;++i){const bool present=i<first.size()&&i<second.size();const bool same=present&&first[i].path==second[i].path&&first[i].numeric==second[i].numeric&&(first[i].numeric?std::abs(first[i].value-second[i].value)<=tolerance:first[i].raw==second[i].raw);equal=equal&&same;
        rows.push_back(object({{"beforePath",i<first.size()?quote(first[i].path):"null"},{"afterPath",i<second.size()?quote(second[i].path):"null"},{"before",i<first.size()?first[i].raw:"null"},{"after",i<second.size()?second[i].raw:"null"},{"equal",truth(same)}}));}
    return {equal,object({{"allDeclaredFieldsEqual",truth(equal)},{"numericToleranceModelUnits",number(tolerance)},{"beforeFieldCount",std::to_string(first.size())},{"afterFieldCount",std::to_string(second.size())},{"fields",array(rows)}})};
}
double formatted_number(const std::wstring& formatted) {const auto start=formatted.find_last_of(L';');const auto value=formatted.substr(start==std::wstring::npos?0:start+1);wchar_t* end{};const double parsed=std::wcstod(value.c_str(),&end);require(end==value.c_str()+value.size()&&end!=value.c_str()&&std::isfinite(parsed),"native_format_not_plain_numeric_after_controls");return parsed;}
void validate_measurements(const Snapshot& actual,const Fixture& fixture,const n8a::Path& path) {
    for(std::size_t i=0;i<path.segments.size();++i){const auto& part=actual.segments[path.segments[i]];const double expected=fixture.stations[i+1]-fixture.stations[i];require(std::abs(part.measurement-expected)<=tolerance,"native_measurement_mismatch");}
}
void validate_formats(const Snapshot& actual,const Fixture& fixture,const n8a::Path& path,double faultDelta=0) {
    for(std::size_t i=0;i<path.segments.size();++i){const auto& part=actual.segments[path.segments[i]];require(part.overrideText.empty(),"native_unexpected_dimension_text_override");require(std::abs(part.factor-fixture.unitToMm)<=tolerance,"native_display_factor_unsupported_no_public_root_dimlfac_method");const double expected=std::floor((fixture.stations[i+1]-fixture.stations[i])*fixture.unitToMm+.5)+faultDelta;require(std::abs(formatted_number(part.formatted)-expected)<=tolerance,"native_format_mismatch");}
}
std::string expect_reject(const std::string& name,const std::string& stage,const std::function<void()>& operation,const std::string& expected) {
    const auto message=n8a::expected_rejection(name,operation,expected);return object({{"name",quote(name)},{"reachedStage",quote(stage)},{"rejected", "true"},{"diagnostic",quote(message)},{"candidateReturned","false"},{"candidateSaved","false"}});
}
std::string formal_source_sha_rejection(const Config& config,Evidence& evidence);
AcDbObjectId clone_seed(AcDbDatabase& source,AcDbDatabase& candidate,AcDbObjectId seed,AcDbIdMapping& mapping) {
    require(&source!=&candidate && (!acDocManager->mdiActiveDocument() || &candidate!=acDocManager->mdiActiveDocument()->database()),"candidate must be independent owned database");AcDbObjectIdArray ids;ids.append(seed);
    check(source.wblockCloneObjects(ids,acdbSymUtil()->blockModelSpaceId(&candidate),mapping,AcDb::kDrcMangleName),"clone owned seed");AcDbIdPair pair;pair.setKey(seed);require(mapping.compute(pair)&&pair.isCloned()&&!pair.value().isNull()&&pair.value().database()==&candidate,"primary mapping invalid");return pair.value();
}
std::string reference_description(AcDbObjectId id,AcDbDatabase* expected) {
    if(id.isNull())return object({{"id","null"},{"class","null"},{"declaredDefinition","null"}});
    Working scope(id.database());auto value=opened<AcDbObject>(id);std::string definition="null";
    if(value->isA()==AcDbDimStyleTableRecord::desc())definition=dimstyle_record(id);
    else if(value->isA()==AcDbTextStyleTableRecord::desc())definition=style_record(id);
    else if(value->isA()==AcDbLayerTableRecord::desc())definition=layer_record(id);
    else if(value->isA()==AcDbLinetypeTableRecord::desc())definition=linetype_record(id);
    return object({{"id",handle(id)},{"class",text(value->isA()->name())},{"ownerId",handle(value->ownerId())},
        {"databaseNull",truth(value->database()==nullptr)},{"databaseMatchesExpected",truth(value->database()==expected)},
        {"declaredDefinition",definition}});
}
std::string sdk_map_records(AcDbIdMapping& mapping) {
    std::vector<std::string> pairs;AcDbIdMappingIter it(mapping);
    for(;!it.done();it.next()){AcDbIdPair pair;require(it.getMap(pair),"SDK map iterator failed");pairs.push_back(object({{"sourceId",handle(pair.key())},{"actualTargetId",handle(pair.value())},{"isCloned",truth(pair.isCloned())},{"isPrimary",truth(pair.isPrimary())},{"ownerTranslated",truth(pair.isOwnerXlated())}}));}
    return array(pairs);
}
struct ReferenceMapping {bool matches{};std::string json;};
struct SymbolProof {std::wstring key;std::string json;};
SymbolProof symbol_proof(AcDbObjectId id,AcDbDatabase& expected) {
    require(!id.isNull()&&id.database()==&expected,"derived_symbol_wrong_database_direction");Working scope(&expected);auto value=opened<AcDbSymbolTableRecord>(id);AcDbObjectId expectedOwner;
    if(value->isA()==AcDbDimStyleTableRecord::desc())expectedOwner=expected.dimStyleTableId();
    else if(value->isA()==AcDbTextStyleTableRecord::desc())expectedOwner=expected.textStyleTableId();
    else throw std::runtime_error("derived_symbol_unexpected_exact_class");
    require(value->ownerId()==expectedOwner,"derived_symbol_wrong_table_owner");AcString key;check(value->getName(key),"derived symbol key");auto table=opened<AcDbSymbolTable>(expectedOwner);AcDbObjectId found;const auto status=table->getAt(key.kACharPtr(),found);
    require(status==Acad::eOk&&found==id,"derived_symbol_table_key_lookup_mismatch");return {key.kACharPtr(),object({{"recordId",handle(id)},{"class",text(value->isA()->name())},{"ownerId",handle(value->ownerId())},{"expectedTableId",handle(expectedOwner)},{"tableClass",text(table->isA()->name())},{"key",text(key.kACharPtr())},{"getAtStatus",std::to_string(status)},{"getAtResult",handle(found)},{"correctDatabaseAndOwner", "true"}})};
}
template<class Dim> std::string dim_scalar_shape(const Dim& dim){return object({{"dimlfac",number(dim.dimlfac())},{"dimdec",std::to_string(dim.dimdec())},{"dimscale",number(dim.dimscale())},{"dimtxt",number(dim.dimtxt())},{"dimasz",number(dim.dimasz())},{"dimexo",number(dim.dimexo())},{"dimexe",number(dim.dimexe())},{"dimgap",number(dim.dimgap())},{"dimtad",std::to_string(dim.dimtad())},{"dimjust",std::to_string(dim.dimjust())}});}
std::string declared_symbol_shape(AcDbObjectId id){Working scope(id.database());auto value=opened<AcDbObject>(id);if(value->isA()==AcDbTextStyleTableRecord::desc()){auto* style=AcDbTextStyleTableRecord::cast(value.get());return object({{"textShape",textstyle_shape(id)},{"shapeFile",truth(style->isShapeFile()!=0)}});}if(value->isA()==AcDbDimStyleTableRecord::desc())return dim_scalar_shape(*AcDbDimStyleTableRecord::cast(value.get()));throw std::runtime_error("derived_symbol_unknown_definition");}
bool single_mangle_key(const std::wstring& source,const std::wstring& target){if(target.size()<4||target.front()!=L'$')return false;std::size_t at=1;while(at<target.size()&&target[at]>=L'0'&&target[at]<=L'9')++at;return at>1&&at<target.size()&&target[at]==L'$'&&target.substr(at+1)==source;}
struct DerivedBindings {
    AcDbDatabase& sourceDb;AcDbDatabase& targetDb;AcDbIdMapping& sdkMap;std::map<AcDbObjectId,AcDbObjectId> forward,reverse;
    DerivedBindings(AcDbDatabase& source,AcDbDatabase& target,AcDbIdMapping& mapping):sourceDb(source),targetDb(target),sdkMap(mapping){}
    ReferenceMapping bind(AcDbObjectId source,AcDbObjectId target,const std::string& role){
        AcDbIdPair pair;pair.setKey(source);const bool found=sdkMap.compute(pair),sdkMatches=found&&pair.value()==target;bool accepted=false;std::string diagnostic,sourceDefinition="null",targetDefinition="null",sourceBefore="null",targetBefore="null",sourceAfter="null",targetAfter="null",sourceShape="null",targetShape="null",nested="null";
        try{sourceDefinition=reference_description(source,&sourceDb);targetDefinition=reference_description(target,&targetDb);const auto first=symbol_proof(source,sourceDb),second=symbol_proof(target,targetDb);sourceBefore=first.json;targetBefore=second.json;auto sourceObject=opened<AcDbObject>(source);auto targetObject=opened<AcDbObject>(target);require(sourceObject->isA()==targetObject->isA(),"derived_symbol_exact_class_mismatch");
            if(found)require(sdkMatches,"derived_symbol_existing_sdk_pair_mismatch");else require(single_mangle_key(first.key,second.key),"derived_symbol_key_relation_mismatch");
            sourceShape=declared_symbol_shape(source);targetShape=declared_symbol_shape(target);require(sourceShape==targetShape,"derived_declared_definition_mismatch");
            const auto previous=forward.find(source);require(previous==forward.end()||previous->second==target,"derived_forward_binding_inconsistent");const auto inverse=reverse.find(target);require(inverse==reverse.end()||inverse->second==source,"derived_reverse_binding_inconsistent");
            if(auto* dim=AcDbDimStyleTableRecord::cast(sourceObject.get())){const auto child=bind(dim->dimtxsty(),AcDbDimStyleTableRecord::cast(targetObject.get())->dimtxsty(),role+".baseTextStyle");nested=child.json;require(child.matches,"derived_dimstyle_textstyle_binding_rejected");}
            sourceAfter=symbol_proof(source,sourceDb).json;targetAfter=symbol_proof(target,targetDb).json;require(sourceBefore==sourceAfter&&targetBefore==targetAfter,"derived_symbol_owner_key_changed_during_binding");forward[source]=target;reverse[target]=source;accepted=true;
        }catch(const std::exception& failure){diagnostic=failure.what();}
        return {accepted,object({{"role",quote(role)},{"bindingKind",quote(std::string("independent_public_explode_derived_role_binding_not_sdk_mapping"))},{"source",sourceDefinition},{"observedTarget",targetDefinition},{"sourceTableProofBefore",sourceBefore},{"targetTableProofBefore",targetBefore},{"sourceTableProofAfter",sourceAfter},{"targetTableProofAfter",targetAfter},{"sourceDeclaredShape",sourceShape},{"targetDeclaredShape",targetShape},{"nestedTextStyleBinding",nested},{"sdkComputeFound",truth(found)},{"sdkPairKey",handle(pair.key())},{"sdkPairValue",handle(pair.value())},{"sdkPairIsCloned",truth(pair.isCloned())},{"sdkPairIsPrimary",truth(pair.isPrimary())},{"sdkPairOwnerTranslated",truth(pair.isOwnerXlated())},{"observedTargetMatchesSdkPair",truth(sdkMatches)},{"singlePrefixRuleRequired",truth(!found)},{"acceptedDerivedBinding",truth(accepted)},{"diagnostic",quote(diagnostic)}})};
    }
};
