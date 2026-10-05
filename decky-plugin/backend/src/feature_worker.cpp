#include "feature_worker.h"

#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

#include "bt_link.h"

FeatureWorker::FeatureWorker(BtLink &link)
    : link_(link), efd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)), thread_([this] { run(); }) {}

FeatureWorker::~FeatureWorker() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
    if (efd_ >= 0) ::close(efd_);
}

void FeatureWorker::post(Job job) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

std::vector<FeatureWorker::Result> FeatureWorker::drain() {
    uint64_t value = 0;
    (void) !::read(efd_, &value, sizeof(value));
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Result> out;
    out.swap(results_);
    return out;
}

void FeatureWorker::run() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        Result result;
        if (job.set) {
            result.ok = link_.set_feature(job.data.data(), job.data.size());
        } else {
            uint8_t buf[128];
            const ssize_t n = link_.get_feature(job.report_id, buf, sizeof(buf));
            if (n > 0 && buf[0] == job.report_id) {
                result.ok = true;
                result.data.assign(buf, buf + n);
            }
        }
        result.job = std::move(job);

        {
            std::lock_guard<std::mutex> lock(mu_);
            results_.push_back(std::move(result));
        }
        const uint64_t one = 1;
        (void) !::write(efd_, &one, sizeof(one));
    }
}
