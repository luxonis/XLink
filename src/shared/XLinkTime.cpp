#include <chrono>

#include "XLinkTime.h"

void getMonotonicTimestamp(struct timespec* ts) {
    auto now = std::chrono::steady_clock::now();
    auto epoch = now.time_since_epoch();
    ts->tv_sec = std::chrono::duration_cast<std::chrono::seconds>(epoch).count();
    ts->tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(epoch).count() % 1000000000;
}