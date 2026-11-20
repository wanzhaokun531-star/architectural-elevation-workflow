#include "ZhaoDimensions.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include "Project222D9DimensionCommon.inc"
#include "MFColors.inc"

const wchar_t* knownSeed(std::size_t segments) {
    switch (segments) {
    case 1: return L"ACB0";
    case 2: return L"AC84";
    case 3: return L"ACB1";
    case 4: return L"FF4E";
    case 5: return L"ACA0";
    case 6: return L"AC83";
    case 9: return L"BFCE";
    case 11: return L"ACAE";
    case 12: return L"CAF3";
    default: throw std::runtime_error("No verified reference chain for the requested number of stations");
    }
}

Fixture requestFixture(const zhao::DimensionRequest& request) {
    require(request.stations.size() >= 2, "Dimension requires two or more measured stations");
    require(std::isfinite(request.definition) && std::isfinite(request.baseline) &&
            std::abs(request.baseline - request.definition) > tolerance,
            "Dimension requires distinct finite definition and baseline coordinates");
    Fixture fixture;
    fixture.name = "zhao_" + request.name;
    fixture.seed = knownSeed(request.stations.size() - 1);
    fixture.segmentCount = static_cast<int>(request.stations.size() - 1);
    fixture.direction = request.vertical ? AcGeVector3d::kYAxis : AcGeVector3d::kXAxis;
    fixture.origin = request.vertical
        ? AcGePoint3d(request.definition, request.stations.front(), 0)
        : AcGePoint3d(request.stations.front(), request.definition, 0);
    for (const auto station : request.stations)
        fixture.stations.push_back(station - request.stations.front());
    fixture.lateral = request.vertical ? request.definition - request.baseline
                                      : request.baseline - request.definition;
    fixture.unitSource = "Zhao source elevation and aligned plan, model millimetres";
    fixture.placementSource = "Projected plan axis stations or measured elevation heights";
    validate_fixture(fixture);
    return fixture;
}

AcDbObjectId cloneDimension(AcDbDatabase& source, AcDbDatabase& target, const wchar_t* seed) {
    AcDbObjectId id;
    check(source.getAcDbObjectId(id, false, AcDbHandle(seed)), "Find genuine reference chain");
    AcDbObjectIdArray requested;
    requested.append(id);
    AcDbIdMapping mapping;
    check(source.wblockCloneObjects(requested, acdbSymUtil()->blockModelSpaceId(&target),
                                  mapping, AcDb::kDrcIgnore), "Clone genuine reference dimension");
    AcDbIdPair pair;
    pair.setKey(id);
    require(mapping.compute(pair) && pair.isCloned() && pair.value().database() == &target,
            "Reference dimension clone did not produce an owned target root");
    return pair.value();
}

void requireCaptionGripLayout(const Snapshot& snapshot, const Fixture& fixture) {
    const auto size = snapshot.segments.size();
    const auto path = topology(snapshot, fixture.direction);
    require(snapshot.grips.size() == 2 * size + 6 && path.nodes.size() == size + 1,
            "Native chain caption grips do not have the reference structure");
    const auto baseline = fixture.origin +
        AcGeVector3d(-fixture.direction.y, fixture.direction.x, 0) * fixture.lateral;
    const auto first = baseline + fixture.direction * fixture.stations.front();
    const auto last = baseline + fixture.direction * fixture.stations.back();
    require((points_close(snapshot.grips[0], first) && points_close(snapshot.grips[1], last)) ||
            (points_close(snapshot.grips[1], first) && points_close(snapshot.grips[0], last)),
            "Native chain baseline grip prefix differs");
    for (const auto& node : path.nodes) {
        require(std::count_if(snapshot.grips.begin() + 2, snapshot.grips.begin() + size + 3,
                             [&](const auto& grip) { return points_close(grip, native_point(node)); }) == 1,
                "Native chain measurement grip prefix differs");
    }
}

AcGePoint3d captionTarget(const Segment& segment, const Fixture& fixture) {
    auto midpoint = segment.first + (segment.second - segment.first) * .5;
    constexpr double textClearance = 275;
    if (std::abs(fixture.direction.y) > .5)
        return {segment.baseline.x - textClearance, midpoint.y, midpoint.z};
    if (segment.measurement < 250) {
        const auto along = (midpoint - fixture.origin).dotProduct(fixture.direction);
        midpoint.x += along < (fixture.stations.front() + fixture.stations.back()) * .5
                    ? -520. : 520.;
        return {midpoint.x, segment.baseline.y + 500., midpoint.z};
    }
    return {midpoint.x, segment.baseline.y + textClearance, midpoint.z};
}

void placeCaptions(AcDbObjectId id, const Fixture& fixture, ObservationLog& log) {
    auto initial = observe(id, log);
    requireCaptionGripLayout(initial, fixture);
    const auto gripStart = initial.segments.size() + 3;
    for (std::size_t i = 0; i < initial.segments.size(); ++i) {
        auto current = observe(id, log);
        requireCaptionGripLayout(current, fixture);
        const auto& segment = current.segments[i];
        const auto& original = initial.segments[i];
        require(points_close(segment.first, original.first) &&
                points_close(segment.second, original.second) &&
                points_close(segment.baseline, original.baseline) &&
                segment.formatted == original.formatted,
                "Native caption edit altered chain segment order or measurement");
        const auto grip = gripStart + i;
        const auto target = captionTarget(segment, fixture);
        if (!points_close(current.grips[grip], target)) {
            check(move_node(id, grip, target - current.grips[grip]), "Place actual native dimension caption");
            current = observe(id, log);
            require(points_close(current.grips[grip], target), "Native caption grip did not reach its target");
        }
    }
}

void validateSaved(AcDbObjectId id, const Fixture& fixture, const std::wstring& expectedLayer,
                   ObservationLog& log) {
    Working scope(id.database());
    const auto actual = observe(id, log);
    const auto path = topology(actual, fixture.direction);
    const auto targets = fixture.anchors();
    require(path.nodes.size() == targets.size(), "Saved dimension station count differs");
    for (std::size_t i = 0; i < targets.size(); ++i)
        require(points_close(native_point(path.nodes[i]), targets[i]), "Saved dimension station differs");
    const AcGeVector3d normal(-fixture.direction.y, fixture.direction.x, 0);
    const auto baseline = fixture.origin + normal * fixture.lateral;
    for (const auto& segment : actual.segments) {
        require(std::abs((segment.baseline - baseline).dotProduct(normal)) <= tolerance,
                "Saved dimension baseline differs");
        require(std::abs(segment.textHeight - 297.5) <= tolerance,
                "Saved dimension text height differs from the reference");
        const auto fields = flatten(segment.json);
        const auto extension = std::find_if(fields.begin(), fields.end(), [](const FlatField& f) {
            return f.path == "$.dimexo";
        });
        require(extension != fields.end() && extension->numeric &&
                std::abs(extension->value - 300) <= tolerance,
                "Saved dimension visible extension offset differs from the reference");
    }
    requireCaptionGripLayout(actual, fixture);
    for (std::size_t i = 0; i < actual.segments.size(); ++i) {
        require(points_close(actual.grips[actual.segments.size() + 3 + i],
                             captionTarget(actual.segments[i], fixture)),
                "Saved native dimension caption drifted from its segment");
    }
    validate_measurements(actual, fixture, path);
    validate_formats(actual, fixture, path);
    const auto colors = d7_read_dimension_colors(id, log);
    d7_validate_dimension_colors(colors, expectedLayer);
    require((expectedLayer == L"AXIS" && colors.rootEffective == 3) ||
            (expectedLayer == L"PUB_DIM" && colors.rootEffective == 25),
            "Saved dimension layer color differs");
    for (const auto& segment : colors.segments)
        require(segment.textColor == 7, "Saved dimension text color differs from the reference");
}

std::wstring layerName(AcDbObjectId id) {
    auto record = opened<AcDbLayerTableRecord>(id);
    AcString name;
    check(record->getName(name), "Read target dimension layer");
    return name.kACharPtr();
}

} // Existing native dimension helpers are in an unnamed namespace.

namespace zhao {
struct DimensionBuilder::Impl {
    AcDbDatabase& donor;
    AcDbDatabase& target;
    ObservationLog log;

    Impl(AcDbDatabase& donor, AcDbDatabase& target, const bce::native::Config& config,
         bce::native::Evidence& evidence)
        : donor(donor), target(target), log{config, evidence} {}
};

DimensionBuilder::DimensionBuilder(AcDbDatabase& donor, AcDbDatabase& target,
                                   const bce::native::Config& config, bce::native::Evidence& evidence)
    : implementation_(std::make_unique<Impl>(donor, target, config, evidence)) {}

DimensionBuilder::~DimensionBuilder() = default;

AcDbObjectId DimensionBuilder::add(const DimensionRequest& request) {
    auto& state = *implementation_;
    Working scope(&state.target);
    const auto fixture = requestFixture(request);
    require(request.layer.database() == &state.target, "Dimension layer belongs to a different drawing");
    const auto expectedLayer = layerName(request.layer);
    const auto id = cloneDimension(state.donor, state.target, fixture.seed.c_str());
    try {
        SceneLog operations{state.log.config, fixture.name, 0, {}};
        adapt(id, fixture, state.log, operations);
        {
            auto entity = opened<AcDbEntity>(id, AcDb::kForWrite);
            check(entity->setLayer(request.layer), "Set native dimension layer");
            check(entity->setColorIndex(256), "Set native dimension ByLayer");
        }
        placeCaptions(id, fixture, state.log);
        validateSaved(id, fixture, expectedLayer, state.log);
    } catch (...) {
        auto entity = opened<AcDbEntity>(id, AcDb::kForWrite);
        check(entity->erase(), "Discard unsuccessful native dimension candidate");
        throw;
    }
    return id;
}

void DimensionBuilder::verify(AcDbObjectId saved, const DimensionRequest& request) {
    validateSaved(saved, requestFixture(request), layerName(request.layer), implementation_->log);
}
}

