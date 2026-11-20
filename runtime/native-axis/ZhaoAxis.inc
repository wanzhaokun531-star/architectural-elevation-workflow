// Included after MFCommon.inc and MFGrips.inc in the drawing builder's namespace.
namespace zhao_axis_detail {
constexpr double tol = 1e-4;
constexpr double radius = 400.0;
constexpr double stemHeight = 550.0;
constexpr double bendHeight = 650.0;

struct Seed {
    const wchar_t* caption;
    const wchar_t* handle;
    bool in222;
};

constexpr Seed seeds[] = {
    {L"1", L"8DF", false}, {L"2", L"8E1", false}, {L"3", L"8E3", false},
    {L"4", L"8E5", false}, {L"5", L"8E7", false}, {L"6", L"8E9", false},
    {L"A", L"942", false}, {L"B", L"940", false}, {L"C", L"93E", false},
    {L"D", L"10BDB", true}, {L"E", L"93C", false}, {L"F", L"93A", false}
};

bool same(const AcGePoint3d& a, const AcGePoint3d& b) {
    return a.distanceTo(b) < tol;
}

bool segment(const std::pair<AcGePoint3d, AcGePoint3d>& line,
             const AcGePoint3d& a, const AcGePoint3d& b) {
    return (same(line.first, a) && same(line.second, b)) ||
           (same(line.first, b) && same(line.second, a));
}

AcDbObjectId find(AcDbDatabase& z222, AcDbDatabase& mengfu, const std::wstring& caption) {
    for (const auto& seed : seeds) {
        if (caption != seed.caption) continue;
        auto& donor = seed.in222 ? z222 : mengfu;
        AcDbObjectId id;
        if (donor.getAcDbObjectId(id, false, AcDbHandle(seed.handle)) == Acad::eOk) {
            auto entity = opened<AcDbEntity>(id);
            const bool trueAxis = !entity->isAProxy() &&
                std::wstring(entity->isA()->name()) == L"TDbAxisLabelSet";
            entity.reset();
            if (trueAxis) {
                const auto scene = axis_scene(id);
                if (scene.texts.size() == 1 && scene.texts[0].text == caption &&
                    scene.circles.size() == 1 && scene.lines.size() == 1)
                    return id;
            }
        }
    }

    for (auto* donor : {&z222, &mengfu}) {
        Working scope(donor);
        for (const auto id : model_roots(*donor)) {
            auto entity = opened<AcDbEntity>(id);
            if (entity->isAProxy() ||
                std::wstring(entity->isA()->name()) != L"TDbAxisLabelSet")
                continue;
            entity.reset();
            const auto scene = axis_scene(id);
            if (scene.texts.size() == 1 && scene.texts[0].text == caption &&
                scene.circles.size() == 1 && scene.lines.size() == 1)
                return id;
        }
    }
    throw std::runtime_error("No genuine single-ended Tianzheng axis donor for " +
                             utf8(caption));
}

AcDbObjectId axisLayer(AcDbDatabase& db) {
    auto table = opened<AcDbLayerTable>(db.layerTableId());
    AcDbObjectId id;
    check(table->getAt(L"AXIS", id), "Find rule AXIS layer");
    const auto layer = opened<AcDbLayerTableRecord>(id);
    require(layer->color().colorIndex() == 3 &&
            layer->linetypeObjectId() == db.continuousLinetype() &&
            layer->lineWeight() == AcDb::kLnWtByLwDefault && layer->isPlottable() &&
            !layer->isLocked() && !layer->isOff() && !layer->isFrozen(),
            "AXIS layer must match the elevation rules");
    return id;
}

void checkRoot(AcDbObjectId id, const std::wstring& caption,
               const AcGePoint3d& center, const AcGePoint3d& tip) {
    const auto scene = axis_scene(id);
    const AcGePoint3d ringTop(center.x, center.y + radius, center.z);
    require(scene.visible && scene.circles.size() == 1 && scene.texts.size() == 1 &&
            scene.lines.size() == 1 && scene.texts[0].text == caption &&
            !scene.texts[0].mirroredX && !scene.texts[0].mirroredY &&
            std::abs(std::remainder(scene.texts[0].rotation, 2 * pi)) < tol &&
            same(scene.circles[0].first, center) &&
            std::abs(scene.circles[0].second - radius) < tol &&
            segment(scene.lines[0], ringTop, tip),
            "Native axis label, circle or lead failed readback");
}

void erase(AcDbObjectId id) {
    auto entity = opened<AcDbEntity>(id, AcDb::kForWrite);
    check(entity->erase(), "Discard an unselected native axis clone");
}

AcDbObjectId changeEndpoint(AcDbDatabase& db, AcDbObjectId source,
                            const AcGePoint3d& old, const AcGePoint3d& target,
                            const std::wstring& caption, const AcGePoint3d& center) {
    if (same(old, target)) return source;

    const auto scene = axis_scene(source);
    const auto geometry = d4grips::readGeometry(source);
    auto expected = geometry;
    std::size_t endpointIndex = 0;
    unsigned gripMatches = 0;
    unsigned stretchMatches = 0;
    for (std::size_t i = 0; i < expected.grips.size(); ++i) {
        if (!same(expected.grips[i], old)) continue;
        expected.grips[i] = target;
        endpointIndex = i;
        ++gripMatches;
    }
    for (auto& p : expected.stretch) {
        if (!same(p, old)) continue;
        p = target;
        ++stretchMatches;
    }
    require(gripMatches == 1 && stretchMatches == 1 && endpointIndex % 4 == 0,
            "Axis donor lacks a unique editable native endpoint");

    struct Attempt {
        AcDbObjectId accepted;
        d4grips::Geometry actual;
        int gripCount;
    };
    const auto view = d4grips::view();
    const auto attempt = [&](int grip, int second, const AcGeVector3d& correction) {
        auto scratch = std::make_unique<AcDbDatabase>(true, true);
        const auto id = clone_material(db, *scratch, source);
        Working scope(scratch.get());
        auto current = geometry;
        int gripCount = 1;
        bool valid = false;
        {
            auto entity = opened<AcDbEntity>(id, AcDb::kForWrite);
            const auto requested = target + correction;
            const auto moved = d4grips::move(*entity, grip, AcGeVector3d(0, 0, 0),
                                            &requested, view);
            gripCount = moved.count;
            entity.reset();
            current = d4grips::readGeometry(id);
            if (moved.status == 0 && second >= 0 && current.grips.size() == expected.grips.size() &&
                same(current.grips[endpointIndex], target) && endpointIndex + 1 < current.grips.size() &&
                !d4grips::same(current, expected)) {
                const auto restore = geometry.grips[endpointIndex + 1] -
                                     current.grips[endpointIndex + 1];
                entity = opened<AcDbEntity>(id, AcDb::kForWrite);
                const auto next = d4grips::move(*entity, second, restore, nullptr, view);
                entity.reset();
                if (next.status == 0) current = d4grips::readGeometry(id);
            }
            if (moved.status == 0 && d4grips::same(current, expected) &&
                d4grips::scene_matches(scene, axis_scene(id), old, target)) {
                checkRoot(id, caption, center, target);
                valid = true;
            }
        }
        const auto accepted = valid ? clone_material(*scratch, db, id) : AcDbObjectId();
        return Attempt{accepted, std::move(current), gripCount};
    };

    int gripCount = 1;
    for (int grip = 0; grip < gripCount; ++grip) {
        const auto first = attempt(grip, -1, AcGeVector3d(0, 0, 0));
        gripCount = first.gripCount;
        if (!first.accepted.isNull()) {
            erase(source);
            return first.accepted;
        }
        if (first.actual.grips.size() != geometry.grips.size() ||
            same(first.actual.grips[endpointIndex], geometry.grips[endpointIndex]))
            continue;
        const auto correction = target - first.actual.grips[endpointIndex];
        for (int second = -1; second < gripCount; ++second) {
            const auto candidate = attempt(grip, second, correction);
            if (!candidate.accepted.isNull()) {
                erase(source);
                return candidate.accepted;
            }
        }
    }
    throw std::runtime_error("No native axis grip operation preserved the circle and caption");
}

void addGuide(AcDbDatabase& db, AcDbObjectId layer,
              const AcGePoint3d& from, const AcGePoint3d& to) {
    if (same(from, to)) return;
    auto line = std::make_unique<AcDbLine>(from, to);
    line->setDatabaseDefaults(&db);
    check(line->setLayer(layer), "Set AXIS guide layer");
    check(line->setColorIndex(256), "Set AXIS guide ByLayer color");
    auto model = opened<AcDbBlockTableRecord>(acdbSymUtil()->blockModelSpaceId(&db),
                                               AcDb::kForWrite);
    AcDbObjectId id;
    check(model->appendAcDbEntity(id, line.get()), "Append independent axis guide");
    check(line.release()->close(), "Close independent axis guide");
}

void findGuide(AcDbDatabase& db, AcDbObjectId layer,
               const AcGePoint3d& from, const AcGePoint3d& to) {
    if (same(from, to)) return;
    unsigned matches = 0;
    for (const auto id : model_roots(db)) {
        auto entity = opened<AcDbEntity>(id);
        const auto* line = AcDbLine::cast(entity.get());
        if (line && line->layerId() == layer && line->colorIndex() == 256 &&
            segment({line->startPoint(), line->endPoint()}, from, to))
            ++matches;
    }
    require(matches == 1, "Expected exactly one independent AXIS guide");
}
} // namespace zhao_axis_detail

void zhao_verify_axis(AcDbObjectId id, const wchar_t* caption, double station,
                      double centerX, double centerY, double lineTopY) {
    using namespace zhao_axis_detail;
    require(!id.isNull() && id.database(), "Native axis root has no drawing");
    auto& output = *id.database();
    Working scope(&output);
    const auto layer = axisLayer(output);
    const auto entity = opened<AcDbEntity>(id);
    require(entity->layerId() == layer && entity->colorIndex() == 256,
            "Native axis label is not ByLayer on AXIS");
    const AcGePoint3d center(centerX, centerY, 0.0);
    const AcGePoint3d top(station, lineTopY, 0.0);
    if (std::abs(station - centerX) < tol) {
        checkRoot(id, caption, center, top);
    } else {
        const AcGePoint3d stem(centerX, centerY + stemHeight, 0.0);
        const AcGePoint3d bend(station, centerY + bendHeight, 0.0);
        checkRoot(id, caption, center, stem);
        findGuide(output, layer, stem, bend);
        findGuide(output, layer, bend, top);
    }
}

AcDbObjectId zhao_axis_from_seed(AcDbObjectId seed, AcDbDatabase& output,
                                const wchar_t* caption, double station, double centerX,
                                double centerY, double lineTopY) {
    using namespace zhao_axis_detail;
    require(caption && *caption && std::isfinite(station) && std::isfinite(centerX) &&
            std::isfinite(centerY) && std::isfinite(lineTopY) && lineTopY > centerY + 1600,
            "Invalid axis caption or guide geometry");
    const auto prototype = axis_scene(seed);
    require(prototype.circles.size() == 1 && prototype.texts.size() == 1 &&
            prototype.texts[0].text == caption, "Axis donor caption or side count differs");
    const auto prototypeCenter = prototype.circles[0].first;
    const AcGePoint3d center(centerX, centerY, 0.0);
    Working scope(&output);
    auto id = clone_material(*seed.database(), output, seed);
    const auto layer = axisLayer(output);
    {
        auto entity = opened<AcDbEntity>(id, AcDb::kForWrite);
        check(entity->transformBy(AcGeMatrix3d::translation(center - prototypeCenter)),
              "Place native axis circle");
        check(entity->setLayer(layer), "Set native axis layer");
        check(entity->setColorIndex(256), "Set native axis ByLayer color");
    }
    const auto scene = axis_scene(id);
    const auto ringTop = AcGePoint3d(centerX, centerY + radius, 0.0);
    require(scene.lines.size() == 1 &&
            (same(scene.lines[0].first, ringTop) || same(scene.lines[0].second, ringTop)),
            "Donor must have one native vertical stem above its circle");
    const auto old = same(scene.lines[0].first, ringTop) ? scene.lines[0].second :
                                                        scene.lines[0].first;
    require(std::abs(old.x - centerX) < tol && old.y > ringTop.y,
            "Donor axis stem must be vertical and above its circle");

    const AcGePoint3d top(station, lineTopY, 0.0);
    if (std::abs(station - centerX) < tol) {
        id = changeEndpoint(output, id, old, top, caption, center);
    } else {
        const AcGePoint3d stem(centerX, centerY + stemHeight, 0.0);
        const AcGePoint3d bend(station, centerY + bendHeight, 0.0);
        id = changeEndpoint(output, id, old, stem, caption, center);
        addGuide(output, layer, stem, bend);
        addGuide(output, layer, bend, top);
    }
    zhao_verify_axis(id, caption, station, centerX, centerY, lineTopY);
    return id;
}

AcDbObjectId zhao_axis(AcDbDatabase& z222, AcDbDatabase& mengfu, AcDbDatabase& output,
                      const wchar_t* caption, double station, double centerX,
                      double centerY, double lineTopY) {
    require(caption && *caption, "Missing axis caption");
    const auto seed = zhao_axis_detail::find(z222, mengfu, caption);
    return zhao_axis_from_seed(seed, output, caption, station, centerX, centerY, lineTopY);
}
