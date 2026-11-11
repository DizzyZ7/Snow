#pragma once
#include "core.hpp"
#include <algorithm>
#include <cmath>

namespace snow {
// Small in-process timing policy. It measures application frame cost, not GPU
// utilization. Battery/system-load signals are supplied by the Windows host.
class PerformanceMonitor {
public:
    void recordFrame(double seconds) noexcept {
        if (!std::isfinite(seconds) || seconds < 0) return;
        average_ = average_ == 0 ? seconds : average_ * .9 + seconds * .1;
        const double budget = cap_ == 60 ? .016 : cap_ == 30 ? .033 : .067;
        if (average_ > budget * .8) {
            ++slow_; fast_ = 0;
            if (slow_ >= 30 && cap_ > 15) { cap_ = cap_ == 60 ? 30 : 15; slow_ = 0; }
        } else if (average_ < budget * .35) {
            slow_ = 0; ++fast_;
            if (fast_ >= 300 && cap_ < 60) { cap_ = cap_ == 15 ? 30 : 60; fast_ = 0; }
        } else slow_ = fast_ = 0;
    }
    [[nodiscard]] int interval(Profile profile, bool battery, bool saver, double systemLoad) const noexcept {
        int fps = std::min(cap_, profile == Profile::Eco ? 15 : profile == Profile::Quality ? 60 : 30);
        if (battery || saver) fps = 15;
        if (systemLoad >= .85) fps = std::min(fps, 30);
        return fps == 60 ? 16 : fps == 30 ? 33 : 67;
    }
    [[nodiscard]] double frameSeconds() const noexcept { return average_; }
private:
    int cap_{60};
    unsigned slow_{}, fast_{};
    double average_{};
};
}
