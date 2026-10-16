#include "app_state.hpp"

std::atomic<bool> running{true};
std::mutex frame_mu;
std::condition_variable frame_cv;
std::vector<uchar> latest_jpeg;
unsigned long frame_seq = 0;

void stop_app()
{
    running = false;
    frame_cv.notify_all();
}
