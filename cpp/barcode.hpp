#pragma once

#include <opencv2/core.hpp>
#include <set>
#include <string>
#include <vector>

struct ScanResult {
    std::vector<cv::Rect> boxes;
    std::set<std::string> values;
};

class BarcodeScanner {
public:
    BarcodeScanner();
    ~BarcodeScanner();
    BarcodeScanner(const BarcodeScanner &) = delete;
    BarcodeScanner &operator=(const BarcodeScanner &) = delete;

    ScanResult scan(const cv::Mat &gray);

private:
    void *scanner_;
};

void publish_frame(cv::Mat frame, const std::vector<cv::Rect> &boxes,
                   const cv::Size &scan_size);
void process_frame(cv::Mat frame, BarcodeScanner &scanner);
