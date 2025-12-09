#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace friar::time {

struct Measurement {
    std::string name;
    std::chrono::nanoseconds elapsed;
};

struct Timings {
    std::vector<Measurement> measurements;
    bool perform_measurements = true;

    template<class F>
    auto measure(std::string_view name, F &&f) -> decltype(f()) {
        if (!perform_measurements) {
            return f();
        }

        Measurement measurement;
        measurement.name = name;

        auto start = std::chrono::steady_clock::now();

        decltype(f()) result = f();
        auto end = std::chrono::steady_clock::now();

        measurement.elapsed = end - start;
        measurements.push_back(measurement);

        return result;
    }
};

} // namespace friar::time
