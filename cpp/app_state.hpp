#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <opencv2/core.hpp>
#include <vector>

extern std::atomic<bool> running;
extern std::mutex frame_mu;
extern std::condition_variable frame_cv;
extern std::vector<uchar> latest_jpeg;
extern unsigned long frame_seq;

void stop_app();
