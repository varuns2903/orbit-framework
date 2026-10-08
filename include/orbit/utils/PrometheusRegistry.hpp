#pragma once
#include <orbit/legacy_namespaces.hpp>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>

namespace orbit::utils {

class PrometheusRegistry {
public:
    static PrometheusRegistry& get_instance() {
        static PrometheusRegistry instance;
        return instance;
    }

    // Metric Types
    void inc_counter(const std::string& name, const std::string& labels = "", double value = 1.0);
    void set_gauge(const std::string& name, const std::string& labels, double value);
    void inc_gauge(const std::string& name, const std::string& labels = "", double value = 1.0);
    void dec_gauge(const std::string& name, const std::string& labels = "", double value = 1.0);
    void observe_histogram(const std::string& name, const std::string& labels, double value);

    // Expose metrics in Prometheus text format
    std::string expose() const;

private:
    PrometheusRegistry() = default;

    struct Metric {
        double value{0.0};
        
        // For histogram
        uint64_t count{0};
        double sum{0.0};
        std::vector<uint64_t> buckets; // Simplistic buckets
    };

    mutable std::mutex mutex_;
    
    // name -> (labels -> Metric)
    std::unordered_map<std::string, std::unordered_map<std::string, Metric>> counters_;
    std::unordered_map<std::string, std::unordered_map<std::string, Metric>> gauges_;
    std::unordered_map<std::string, std::unordered_map<std::string, Metric>> histograms_;
    
    // Help and type strings
    std::unordered_map<std::string, std::string> help_text_;
    std::unordered_map<std::string, std::string> type_text_;
};

class MetricsTimer {
public:
    MetricsTimer(const std::string& name, const std::string& labels)
        : name_(name), labels_(labels), start_(std::chrono::high_resolution_clock::now()) {}

    ~MetricsTimer() {
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> duration = end - start_;
        PrometheusRegistry::get_instance().observe_histogram(name_, labels_, duration.count());
    }

private:
    std::string name_;
    std::string labels_;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_;
};

} // namespace utils
