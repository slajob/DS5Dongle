// Runs blocking feature-report requests (GET/SET_REPORT on the Bluetooth
// control channel) off the main loop, so a slow controller reply never stalls
// the 1 ms audio clock. Completions are signalled through an eventfd.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class BtLink;

class FeatureWorker {
public:
    struct Job {
        bool set = false;
        uint8_t report_id = 0;
        std::vector<uint8_t> data; // SET: full report incl. id
        uint64_t token = 0;
    };
    struct Result {
        Job job;
        bool ok = false;
        std::vector<uint8_t> data; // GET: report incl. id
    };

    explicit FeatureWorker(BtLink &link);
    ~FeatureWorker();
    FeatureWorker(const FeatureWorker &) = delete;
    FeatureWorker &operator=(const FeatureWorker &) = delete;

    void post(Job job);
    int event_fd() const { return efd_; }
    std::vector<Result> drain();

private:
    void run();

    BtLink &link_;
    int efd_ = -1;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    bool stop_ = false;
    std::thread thread_;
};
