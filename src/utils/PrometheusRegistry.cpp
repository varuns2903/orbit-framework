#include <vector>
#include <orbit/utils/PrometheusRegistry.hpp>
#include <charconv>
#include <cmath>
#include <sstream>
#include <system_error>
#include <string>

namespace orbit::utils {

namespace {
// Default buckets, suited to request latencies in seconds (the Prometheus
// client libraries' defaults).
const std::vector<double> kHistogramBounds = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};

// A sample value in the exposition format: the shortest text that reads back
// as the same double. Streaming a double prints 6 significant digits, so a
// counter past a million (1234567 bytes) came out as 1.23457e+06.
std::string format_value(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    char buf[32];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v);
    return ec == std::errc() ? std::string(buf, end) : std::to_string(v);
}
} // namespace

void PrometheusRegistry::inc_counter(const std::string& name, const std::string& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[name][labels].value += value;
    if (type_text_.find(name) == type_text_.end()) {
        type_text_[name] = "counter";
    }
}

void PrometheusRegistry::set_gauge(const std::string& name, const std::string& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name][labels].value = value;
    if (type_text_.find(name) == type_text_.end()) {
        type_text_[name] = "gauge";
    }
}

void PrometheusRegistry::inc_gauge(const std::string& name, const std::string& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name][labels].value += value;
    if (type_text_.find(name) == type_text_.end()) {
        type_text_[name] = "gauge";
    }
}

void PrometheusRegistry::dec_gauge(const std::string& name, const std::string& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    gauges_[name][labels].value -= value;
}

void PrometheusRegistry::observe_histogram(const std::string& name, const std::string& labels, double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& m = histograms_[name][labels];
    if (m.buckets.empty()) m.buckets.assign(kHistogramBounds.size(), 0);
    m.count++;
    m.sum += value;
    // Per-bucket counts; exposition makes them cumulative.
    for (size_t i = 0; i < kHistogramBounds.size(); ++i) {
        if (value <= kHistogramBounds[i]) {
            m.buckets[i]++;
            break;
        }
    }
    if (type_text_.find(name) == type_text_.end()) {
        type_text_[name] = "histogram";
    }
}

std::string PrometheusRegistry::expose() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ostringstream oss;

    auto print_metric = [&oss, this](const auto& map, const std::string& name) {
        if (type_text_.count(name)) {
            oss << "# TYPE " << name << " " << type_text_.at(name) << "\n";
        }
        for (const auto& [labels, metric] : map.at(name)) {
            oss << name;
            if (!labels.empty()) oss << "{" << labels << "}";
            oss << " " << format_value(metric.value) << "\n";
        }
    };
    
    auto print_histogram = [&oss, this](const auto& map, const std::string& name) {
        if (type_text_.count(name)) {
            oss << "# TYPE " << name << " " << type_text_.at(name) << "\n";
        }
        for (const auto& [labels, metric] : map.at(name)) {
            std::string l_comma = labels.empty() ? "" : labels + ",";

            // Cumulative buckets (le = "less than or equal"), as Prometheus expects.
            uint64_t cumulative = 0;
            for (size_t i = 0; i < kHistogramBounds.size(); ++i) {
                cumulative += i < metric.buckets.size() ? metric.buckets[i] : 0;
                oss << name << "_bucket{" << l_comma << "le=\"" << kHistogramBounds[i] << "\"} " << cumulative << "\n";
            }
            oss << name << "_bucket{" << l_comma << "le=\"+Inf\"} " << metric.count << "\n";
            
            // Expose _sum and _count
            oss << name << "_sum";
            if (!labels.empty()) oss << "{" << labels << "}";
            oss << " " << format_value(metric.sum) << "\n";
            
            oss << name << "_count";
            if (!labels.empty()) oss << "{" << labels << "}";
            oss << " " << metric.count << "\n";
        }
    };

    for (const auto& [name, labels_map] : counters_) {
        print_metric(counters_, name);
    }
    for (const auto& [name, labels_map] : gauges_) {
        print_metric(gauges_, name);
    }
    for (const auto& [name, labels_map] : histograms_) {
        print_histogram(histograms_, name);
    }

    return oss.str();
}

} // namespace utils
