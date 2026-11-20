#pragma once

#include <dbid.h>
#include <gepnt3d.h>

#include <string>
#include <vector>

namespace bce::trial {

struct Dimension {
    AcGePoint3d baseline;
    std::vector<AcGePoint3d> stations;
    double visibleGap{};
    bool vertical{};
};

Dimension readDimension(AcDbObjectId id);
void placeVerticalDimension(AcDbObjectId id, double definitionX, double baselineX);

struct Level {
    AcGePoint3d tip;
    std::wstring label;
    double metres{};
    bool upright{};
};

Level readLevel(AcDbObjectId id);
AcDbObjectId mirrorLevel(AcDbObjectId source, double targetTipX);

}
