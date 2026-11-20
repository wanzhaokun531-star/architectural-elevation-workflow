#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "NativeAnnotation.h"

#include <dbapserv.h>
#include <dbdim.h>
#include <dbents.h>
#include <dbpl.h>
#include <dbsymtb.h>
#include <dbsymutl.h>
#include <gemat3d.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <exception>
#include <locale>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bce::trial {
namespace {

constexpr double tolerance = 1e-6;
constexpr double fullRotation = 6.28318530717958647692;

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check(Acad::ErrorStatus status, const char* operation)
{
    if (status != Acad::eOk)
        throw std::runtime_error(std::string(operation) + " (ObjectARX status "
                                 + std::to_string(static_cast<int>(status)) + ")");
}

bool close(double a, double b)
{
    return std::abs(a - b) <= tolerance;
}

bool close(const AcGePoint3d& a, const AcGePoint3d& b)
{
    return a.distanceTo(b) <= tolerance;
}

bool finite(const AcGePoint3d& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

class WorkingDatabase {
public:
    explicit WorkingDatabase(AcDbObjectId id)
        : services_(acdbHostApplicationServices())
    {
        require(services_ && !id.isNull() && id.database(),
                "A persistent native model-space object is required");
        previous_ = services_->workingDatabase();
        services_->setWorkingDatabase(id.database());
    }

    ~WorkingDatabase() { services_->setWorkingDatabase(previous_); }
    WorkingDatabase(const WorkingDatabase&) = delete;
    WorkingDatabase& operator=(const WorkingDatabase&) = delete;

private:
    AcDbHostApplicationServices* services_;
    AcDbDatabase* previous_{};
};

struct CloseObject {
    void operator()(AcDbObject* object) const { object->close(); }
};

template<class T>
std::unique_ptr<T, CloseObject> opened(AcDbObjectId id, AcDb::OpenMode mode = AcDb::kForRead)
{
    T* object = nullptr;
    check(acdbOpenObject(object, id, mode), "Open native annotation");
    return std::unique_ptr<T, CloseObject>(object);
}

std::wstring_view className(const AcDbEntity& entity)
{
    return entity.isA()->name();
}

void requireRoot(const AcDbEntity& root, std::wstring_view expected)
{
    require(!root.isAProxy() && className(root) == expected,
            "Annotation must be a genuine native Tianzheng object of the requested type");
    require(root.database() && root.ownerId() == acdbSymUtil()->blockModelSpaceId(root.database()),
            "Native annotation must be directly in model space");
}

enum class SceneKind { Dimension, Level };

bool knownChild(const AcDbEntity& entity, SceneKind kind)
{
    if (kind == SceneKind::Dimension)
        return AcDbAlignedDimension::cast(&entity) != nullptr;
    const auto type = className(entity);
    return AcDbLine::cast(&entity) || AcDbText::cast(&entity)
        || AcDbPolyline::cast(&entity) || type == L"TDbMText" || type == L"TDbText";
}

class ExplodedScene {
public:
    std::vector<AcDbEntity*> children(const AcDbEntity& source, SceneKind kind)
    {
        AcDbVoidPtrArray raw;
        Acad::ErrorStatus status = Acad::eNotApplicable;
        std::exception_ptr failure;
        try {
            status = source.explode(raw);
        } catch (...) {
            failure = std::current_exception();
        }

        bool safe = true;
        std::vector<AcDbEntity*> result;
        for (int i = 0; i < raw.length(); ++i) {
            auto* item = raw[i];
            if (!item || item == &source || !seen_.insert(item).second) {
                safe = false;
                continue;
            }
            auto* entity = AcDbEntity::cast(static_cast<AcRxObject*>(item));
            if (!entity || entity->isAProxy() || !knownChild(*entity, kind)
                || !entity->objectId().isNull() || !entity->ownerId().isNull()
                || entity->database()) {
                safe = false;
                continue;
            }
            // Detached Tianzheng dimension children may still report isPersistent().
            std::unique_ptr<AcDbEntity> owner(entity);
            owned_.push_back(std::move(owner));
            result.push_back(entity);
        }
        require(safe, "Unexpected or borrowed native annotation explode result");
        if (failure)
            std::rethrow_exception(failure);
        check(status, "Explode native annotation for readback");
        return result;
    }

private:
    std::set<const void*> seen_;
    std::vector<std::unique_ptr<AcDbEntity>> owned_;
};

struct Segment {
    AcGePoint3d first;
    AcGePoint3d second;
    AcGePoint3d baseline;
    AcGePoint3d text;
    AcGeVector3d normal;
    double measurement{};
    double gap{};
    bool manualText{};
    AcDbObjectId layer;
    AcDbObjectId dimStyle;
    AcDbObjectId textStyle;
    std::array<double, 7> styleLengths{};
    std::array<int, 5> styleOptions{};
    std::wstring textOverride;
    std::wstring formatted;
};

struct DimensionSnapshot {
    Dimension dimension;
    std::vector<Segment> segments;
    AcDbObjectId layer;
    AcDbObjectId linetype;
    AcCmColor color;
    AcDb::LineWeight lineWeight{};
};

Segment observeSegment(AcDbAlignedDimension& dim)
{
    Segment segment;
    segment.first = dim.xLine1Point();
    segment.second = dim.xLine2Point();
    segment.baseline = dim.dimLinePoint();
    segment.text = dim.textPosition();
    segment.normal = dim.normal();
    segment.manualText = !dim.isUsingDefaultTextPosition();
    require(finite(segment.first) && finite(segment.second) && finite(segment.baseline)
            && (!segment.manualText || finite(segment.text)),
            "Native dimension contains a non-finite station, baseline, or text position");
    check(dim.measurement(segment.measurement), "Read native dimension measurement");
    segment.gap = dim.dimexo();
    require(std::isfinite(segment.gap) && segment.gap >= 0.0 && close(dim.dimscale(), 1.0),
            "Trial requires native dimensions with overall dimension scale 1");
    segment.layer = dim.layerId();
    segment.dimStyle = dim.dimensionStyle();
    segment.textStyle = dim.dimtxsty();
    segment.styleLengths = {dim.dimscale(), dim.dimlfac(), dim.dimtxt(), dim.dimasz(),
                            dim.dimexo(), dim.dimexe(), dim.dimgap()};
    segment.styleOptions = {dim.dimdec(), dim.dimtad(), dim.dimjust(),
                            dim.dimse1() ? 1 : 0, dim.dimse2() ? 1 : 0};
    AcString textOverride, formatted;
    check(dim.dimensionText(textOverride), "Read native dimension text override");
    check(dim.formatMeasurement(formatted, segment.measurement, textOverride.kACharPtr()),
          "Read native dimension formatted measurement");
    segment.textOverride = textOverride.kACharPtr();
    segment.formatted = formatted.kACharPtr();
    return segment;
}

double along(const AcGePoint3d& point, bool vertical)
{
    return vertical ? point.y : point.x;
}

double across(const AcGePoint3d& point, bool vertical)
{
    return vertical ? point.x : point.y;
}

DimensionSnapshot observeDimension(AcDbObjectId id)
{
    auto root = opened<AcDbEntity>(id);
    requireRoot(*root, L"TDbDimension2");
    DimensionSnapshot result;
    result.layer = root->layerId();
    result.linetype = root->linetypeId();
    result.color = root->color();
    result.lineWeight = root->lineWeight();
    ExplodedScene scene;
    for (auto* entity : scene.children(*root, SceneKind::Dimension))
        result.segments.push_back(observeSegment(*AcDbAlignedDimension::cast(entity)));
    require(!result.segments.empty(), "Native dimension has no measurable segments");

    auto& output = result.dimension;
    const auto& first = result.segments.front();
    output.vertical = close(first.first.x, first.second.x);
    const auto station = first.first;
    output.baseline = first.baseline;
    output.visibleGap = first.gap;
    for (auto& segment : result.segments) {
        for (const auto& endpoint : {segment.first, segment.second}) {
            require(close(across(endpoint, output.vertical), across(station, output.vertical))
                    && close(endpoint.z, station.z),
                    "Only horizontal or vertical native chains with collinear stations are supported");
        }
        require(close(across(segment.baseline, output.vertical),
                      across(output.baseline, output.vertical))
                && close(segment.baseline.z, station.z),
                "Native dimension segments do not share a single baseline");
        require(close(segment.gap, output.visibleGap),
                "Native dimension segments do not share a single visible extension offset");
        if (along(segment.first, output.vertical) > along(segment.second, output.vertical))
            std::swap(segment.first, segment.second);
        const auto span = along(segment.second, output.vertical)
                        - along(segment.first, output.vertical);
        require(span > tolerance && close(segment.measurement, span),
                "Native dimension measurement differs from its station interval");
    }

    std::sort(result.segments.begin(), result.segments.end(),
              [&](const Segment& a, const Segment& b) {
                  return along(a.first, output.vertical) < along(b.first, output.vertical);
              });
    output.stations.push_back(result.segments.front().first);
    for (const auto& segment : result.segments) {
        require(close(output.stations.back(), segment.first),
                "Native dimension chain contains a gap, overlap, or repeated interval");
        output.stations.push_back(segment.second);
    }
    return result;
}

AcGePoint3dArray gripPoints(AcDbObjectId id)
{
    auto root = opened<AcDbEntity>(id);
    AcGePoint3dArray points;
    AcDbIntArray osnaps, geometry;
    check(root->getGripPoints(points, osnaps, geometry), "Read native annotation grips");
    return points;
}

int uniqueGrip(const AcGePoint3dArray& grips, const AcGePoint3d& target)
{
    int match = -1;
    for (int index = 0; index < grips.length(); ++index) {
        if (close(grips[index], target)) {
            require(match < 0, "Native dimension grip position is ambiguous");
            match = index;
        }
    }
    require(match >= 0, "Native dimension has no matching editable grip");
    return match;
}

int baselineGrip(AcDbObjectId id, const Dimension& dimension)
{
    require(!close(dimension.stations.front().x, dimension.baseline.x),
            "Coincident native baseline and station grips cannot be moved safely");
    const auto grips = gripPoints(id);
    auto first = dimension.stations.front();
    auto last = dimension.stations.back();
    first.x = dimension.baseline.x;
    last.x = dimension.baseline.x;
    const int firstGrip = uniqueGrip(grips, first);
    uniqueGrip(grips, last);
    return firstGrip;
}

void moveGrip(AcDbObjectId id, int index, double deltaX)
{
    auto root = opened<AcDbEntity>(id, AcDb::kForWrite);
    AcDbIntArray indices;
    indices.append(index);
    check(root->moveGripPointsAt(indices, AcGeVector3d(deltaX, 0, 0)),
          "Move native dimension grip");
}

void moveX(AcDbObjectId id, double deltaX)
{
    auto root = opened<AcDbEntity>(id, AcDb::kForWrite);
    check(root->transformBy(AcGeMatrix3d::translation(AcGeVector3d(deltaX, 0, 0))),
          "Translate native annotation");
}

void verifyDimension(const DimensionSnapshot& before, const DimensionSnapshot& after,
                     double definitionX, double baselineX, bool verifyText)
{
    const auto& source = before.dimension;
    const auto& target = after.dimension;
    require(target.vertical && target.stations.size() == source.stations.size()
            && after.segments.size() == before.segments.size()
            && close(target.baseline.x, baselineX)
            && close(target.visibleGap, source.visibleGap),
            "Vertical native chain layout or extension offset failed readback");
    require(before.layer == after.layer && before.linetype == after.linetype
            && before.color == after.color && before.lineWeight == after.lineWeight,
            "Vertical native dimension entity appearance changed");
    for (std::size_t index = 0; index < source.stations.size(); ++index) {
        auto expected = source.stations[index];
        expected.x = definitionX;
        require(close(target.stations[index], expected),
                "Vertical native dimension measurement station moved off target");
    }
    for (std::size_t index = 0; index < before.segments.size(); ++index) {
        const auto& a = before.segments[index];
        const auto& b = after.segments[index];
        require(close(a.measurement, b.measurement) && a.manualText == b.manualText
                && a.layer == b.layer && a.dimStyle == b.dimStyle && a.textStyle == b.textStyle
                && a.styleLengths == b.styleLengths && a.styleOptions == b.styleOptions
                && a.textOverride == b.textOverride && a.formatted == b.formatted
                && (a.normal - b.normal).length() <= tolerance,
                "Vertical native dimension measurement, label, or style changed");
        if (verifyText && a.manualText) {
            auto expected = a.text;
            expected.x += baselineX - source.baseline.x;
            require(close(b.text, expected),
                    "Vertical native dimension lost its manual text offset");
        }
    }
}

using Stroke = std::pair<AcGePoint3d, AcGePoint3d>;

struct LevelText {
    std::wstring label;
    double height{};
    double width{};
    AcDbObjectId style;
    bool upright{};
};

struct LevelSnapshot {
    Level level;
    std::vector<Stroke> strokes;
    std::vector<LevelText> texts;
    AcDbObjectId layer;
};

void readLevelChildren(const AcDbEntity& entity, ExplodedScene& scene,
                       LevelSnapshot& snapshot, int depth = 0)
{
    require(depth < 8, "Native elevation symbol is nested too deeply");
    if (const auto* line = AcDbLine::cast(&entity)) {
        require(entity.visibility() == AcDb::kVisible, "Native level line is invisible");
        snapshot.strokes.emplace_back(line->startPoint(), line->endPoint());
        return;
    }
    if (const auto* polyline = AcDbPolyline::cast(&entity)) {
        require(entity.visibility() == AcDb::kVisible, "Native level polyline is invisible");
        const unsigned int count = polyline->numVerts();
        require(count >= 2, "Native level polyline is empty");
        const unsigned int edgeCount = polyline->isClosed() ? count : count - 1;
        for (unsigned int index = 0; index < edgeCount; ++index) {
            AcGePoint3d a, b;
            double bulge{};
            check(polyline->getBulgeAt(index, bulge), "Read native level polyline bulge");
            require(close(bulge, 0.0), "Native level symbol has a curved stroke");
            check(polyline->getPointAt(index, a), "Read native level stroke start");
            check(polyline->getPointAt((index + 1) % count, b), "Read native level stroke end");
            snapshot.strokes.emplace_back(a, b);
        }
        return;
    }
    if (const auto* text = AcDbText::cast(&entity)) {
        AcString label;
        check(text->textString(label), "Read actual Tianzheng elevation text");
        const bool upright = entity.visibility() == AcDb::kVisible
            && !text->isMirroredInX() && !text->isMirroredInY()
            && close(std::remainder(text->rotation(), fullRotation), 0.0);
        snapshot.texts.push_back({label.kACharPtr(), text->height(), text->widthFactor(),
                                  text->textStyle(), upright});
        return;
    }
    for (const auto* child : scene.children(entity, SceneKind::Level))
        readLevelChildren(*child, scene, snapshot, depth + 1);
}

double parseMetres(std::wstring_view label)
{
    while (!label.empty() && std::iswspace(label.front()))
        label.remove_prefix(1);
    while (!label.empty() && std::iswspace(label.back()))
        label.remove_suffix(1);
    std::wstring numeric(label);
    if (!numeric.empty() && numeric.front() == L'\u00B1')
        numeric.erase(0, 1);
    else if (numeric.compare(0, 3, L"%%P") == 0 || numeric.compare(0, 3, L"%%p") == 0)
        numeric.erase(0, 3);
    if (!numeric.empty() && numeric.front() == L'\u2212')
        numeric.front() = L'-';

    std::wistringstream stream(numeric);
    stream.imbue(std::locale::classic());
    double value{};
    require(static_cast<bool>(stream >> value) && stream.eof() && std::isfinite(value),
            "Native elevation text is not an unambiguous number in metres");
    return value;
}

LevelSnapshot observeLevel(AcDbObjectId id)
{
    auto root = opened<AcDbEntity>(id);
    requireRoot(*root, L"TDbSymbElevation");
    require(root->visibility() == AcDb::kVisible, "Native elevation symbol is invisible");
    LevelSnapshot snapshot;
    snapshot.layer = root->layerId();
    ExplodedScene scene;
    readLevelChildren(*root, scene, snapshot);
    require((snapshot.strokes.size() == 3 || snapshot.strokes.size() == 4) && snapshot.texts.size() == 1,
            "Native elevation must have its triangle, baseline, and one label");

    std::vector<Stroke> diagonals;
    for (const auto& stroke : snapshot.strokes) {
        require(finite(stroke.first) && finite(stroke.second)
                && close(stroke.first.z, stroke.second.z) && !close(stroke.first, stroke.second),
                "Invalid native elevation symbol stroke");
        if (!close(stroke.first.x, stroke.second.x) && !close(stroke.first.y, stroke.second.y))
            diagonals.push_back(stroke);
    }
    require(diagonals.size() == 2, "Native elevation triangle is ambiguous");
    unsigned int shared = 0;
    for (const auto& a : {diagonals[0].first, diagonals[0].second}) {
        for (const auto& b : {diagonals[1].first, diagonals[1].second}) {
            if (close(a, b)) {
                snapshot.level.tip = a;
                ++shared;
            }
        }
    }
    require(shared == 1, "Native elevation triangle has no unique tip");
    const auto& text = snapshot.texts.front();
    snapshot.level.label = text.label;
    snapshot.level.metres = parseMetres(text.label);
    snapshot.level.upright = text.upright;
    return snapshot;
}

} // namespace

Dimension readDimension(AcDbObjectId id)
{
    WorkingDatabase work(id);
    return observeDimension(id).dimension;
}

void placeVerticalDimension(AcDbObjectId id, double definitionX, double baselineX)
{
    require(std::isfinite(definitionX) && std::isfinite(baselineX) && !close(definitionX, baselineX),
            "Distinct finite native dimension station and baseline coordinates are required");
    WorkingDatabase work(id);
    const auto original = observeDimension(id);
    const auto& source = original.dimension;
    require(source.vertical, "Only a vertical native dimension chain can be placed here");

    const double definitionShift = definitionX - source.stations.front().x;
    const double baselineShift = baselineX - (source.baseline.x + definitionShift);
    if (!close(definitionShift, 0.0))
        moveX(id, definitionShift);

    auto current = observeDimension(id);
    verifyDimension(original, current, definitionX, source.baseline.x + definitionShift, false);
    if (!close(baselineShift, 0.0)) {
        moveGrip(id, baselineGrip(id, current.dimension), baselineShift);
        current = observeDimension(id);
        verifyDimension(original, current, definitionX, baselineX, false);
    }

    for (std::size_t index = 0; index < original.segments.size(); ++index) {
        const auto& old = original.segments[index];
        if (!old.manualText)
            continue;
        const auto point = current.segments[index].text;
        const double targetX = old.text.x + baselineX - source.baseline.x;
        require(close(point.y, old.text.y) && close(point.z, old.text.z),
                "Vertical native dimension manual text moved along its chain");
        if (!close(point.x, targetX)) {
            moveGrip(id, uniqueGrip(gripPoints(id), point), targetX - point.x);
            current = observeDimension(id);
            verifyDimension(original, current, definitionX, baselineX, false);
        }
    }
    verifyDimension(original, current, definitionX, baselineX, true);
}

Level readLevel(AcDbObjectId id)
{
    WorkingDatabase work(id);
    return observeLevel(id).level;
}

AcDbObjectId mirrorLevel(AcDbObjectId source, double targetTipX)
{
    require(std::isfinite(targetTipX), "Finite target elevation tip coordinate is required");
    WorkingDatabase work(source);
    const auto before = observeLevel(source);
    require(before.level.upright && !close(before.level.tip.x, targetTipX),
            "An upright native elevation and a distinct target position are required");

    std::unique_ptr<AcDbEntity> clone;
    {
        auto root = opened<AcDbEntity>(source);
        auto* raw = root->clone();
        auto* entity = raw && raw != root.get() ? AcDbEntity::cast(raw) : nullptr;
        require(entity && entity->objectId().isNull() && entity->ownerId().isNull()
                && !entity->database(), "Native elevation clone is not detached");
        clone.reset(entity);
        require(className(*clone) == L"TDbSymbElevation" && !clone->isAProxy(),
                "Native elevation clone lost its genuine Tianzheng class");
    }

    AcDbObjectId result;
    {
        auto model = opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(source.database()),
                                                 AcDb::kForWrite);
        check(model->appendAcDbEntity(result, clone.get()), "Append mirrored Tianzheng elevation");
        check(clone.release()->close(), "Close appended Tianzheng elevation");
    }
    {
        auto appended = opened<AcDbEntity>(result, AcDb::kForWrite);
        AcGeMatrix3d reflection;
        reflection.setToIdentity();
        reflection(0, 0) = -1.0;
        reflection(0, 3) = 2.0 * before.level.tip.x;
        check(appended->transformBy(reflection), "Reflect Tianzheng elevation horizontally");
        check(appended->transformBy(AcGeMatrix3d::translation(
                  AcGeVector3d(targetTipX - before.level.tip.x, 0, 0))),
              "Position reflected Tianzheng elevation");
    }

    const auto after = observeLevel(result);
    const auto& oldText = before.texts.front();
    const auto& newText = after.texts.front();
    auto targetTip = before.level.tip;
    targetTip.x = targetTipX;
    require(close(after.level.tip, targetTip) && before.level.label == after.level.label
            && close(before.level.metres, after.level.metres) && after.level.upright
            && before.layer == after.layer && oldText.style == newText.style
            && close(oldText.height, newText.height) && close(oldText.width, newText.width),
            "Reflected Tianzheng elevation tip, value, appearance, or text direction failed readback");

    const auto mirrored = [&](AcGePoint3d point) {
        point.x = targetTipX - (point.x - before.level.tip.x);
        return point;
    };
    for (const auto& [first, second] : before.strokes) {
        const auto a = mirrored(first);
        const auto b = mirrored(second);
        const auto matches = std::count_if(after.strokes.begin(), after.strokes.end(),
            [&](const Stroke& line) {
                return (close(line.first, a) && close(line.second, b))
                    || (close(line.first, b) && close(line.second, a));
            });
        require(matches == 1, "Reflected Tianzheng elevation triangle or datum line changed");
    }
    return result;
}

} // namespace bce::trial
