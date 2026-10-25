#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace by5 {
struct FictitiousHistoryConfig {
    int64_t batches = 57600;
    int64_t versionStep = 31250;
    int64_t minimumRealReadVersion = -1562500;
    uint64_t seed = 2;
};

class FictitiousHistory {
    FictitiousHistoryConfig config_;
    int64_t end_;
    int64_t offset_;
    int64_t completed_ = 0;
    bool ready_ = false;
    bool started_ = false;

    static int64_t checkedAdd(int64_t a, int64_t b) {
        if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
            (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
            throw std::overflow_error("fictitious history version overflow");
        return a + b;
    }

public:
    explicit FictitiousHistory(FictitiousHistoryConfig config) : config_(config) {
        if (config.batches <= 0 || config.versionStep <= 0 ||
            config.minimumRealReadVersion == std::numeric_limits<int64_t>::min())
            throw std::invalid_argument("invalid fictitious history configuration");
        if (config.batches > std::numeric_limits<int64_t>::max() / config.versionStep)
            throw std::overflow_error("fictitious history duration overflow");
        end_ = config.batches * config.versionStep;
        const int64_t guard = config.minimumRealReadVersion < 0 ? -config.minimumRealReadVersion : 0;
        offset_ = checkedAdd(end_, guard);
    }

    int64_t syntheticEnd() const { return end_; }
    int64_t realOffset() const { return offset_; }
    int64_t completedBatches() const { return completed_; }
    bool ready() const { return ready_; }
    const FictitiousHistoryConfig& config() const { return config_; }

    template<class Tree, class Source, class Observer>
    void preload(Tree& tree, Source&& source, Observer&& observe) {
        if (started_ || tree.validationMinRV() != 0 || tree.nodeCount() != 0)
            throw std::logic_error("preload requires a fresh empty tree at floor zero");
        started_ = true;
        for (int64_t batch = 0; batch < config_.batches; ++batch) {
            auto writes = source(batch, config_.seed);
            const int64_t cv = (batch + 1) * config_.versionStep;
            tree.addWrites(cv, writes.data(), writes.size());
            ++completed_;
            if (tree.validationMinRV() > cv)
                throw std::logic_error("preload floor exceeds synthetic time");
            observe(batch, cv, tree);
        }
        ready_ = true;
    }

    int64_t writeVersion(int64_t cv) const {
        if (!ready_ || cv <= 0)
            throw std::logic_error("real writes require completed preload and positive versions");
        return checkedAdd(offset_, cv);
    }

    int64_t readVersion(int64_t rv) const {
        if (!ready_ || rv < config_.minimumRealReadVersion)
            throw std::logic_error("read precedes configured real history domain");
        return checkedAdd(offset_, rv);
    }

    int64_t realFloor(int64_t treeFloor) const {
        return checkedAdd(treeFloor, -offset_);
    }
};
}
