#pragma once

#include <dbid.h>
#include <memory>
#include <string>
#include <vector>

class AcDbDatabase;
namespace bce::native {
struct Config;
class Evidence;
}

namespace zhao {
struct DimensionRequest {
    std::string name;
    std::vector<double> stations;
    bool vertical{};
    double definition{};
    double baseline{};
    AcDbObjectId layer;
};

class DimensionBuilder {
public:
    DimensionBuilder(AcDbDatabase& donor, AcDbDatabase& target,
                     const bce::native::Config& config, bce::native::Evidence& evidence);
    ~DimensionBuilder();
    DimensionBuilder(const DimensionBuilder&) = delete;
    DimensionBuilder& operator=(const DimensionBuilder&) = delete;

    AcDbObjectId add(const DimensionRequest& request);
    void verify(AcDbObjectId saved, const DimensionRequest& request);

private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};
}
