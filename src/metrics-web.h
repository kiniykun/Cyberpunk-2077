#pragma once
#include "abstractMetricsCapture.h"
namespace red { namespace memory {
class MetricsCaptureWeb final : public AbstractMetricsCapture {
public:
    void WritePlatfromIdentifier(Serializer&) override;
    void WriteLoadedModules(Serializer&) override;
};
}}
