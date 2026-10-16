#include "barcode.hpp"

#include "app_state.hpp"
#include "services.hpp"

#include <algorithm>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

// The firmware provides the ZBar ABI and library, but not its headers.
extern "C"
{
    struct zbar_image_scanner_s;
    struct zbar_image_s;
    struct zbar_symbol_s;
    using zbar_image_scanner_t = zbar_image_scanner_s;
    using zbar_image_t = zbar_image_s;
    using zbar_symbol_t = zbar_symbol_s;

    zbar_image_scanner_t *zbar_image_scanner_create(void);
    void zbar_image_scanner_destroy(zbar_image_scanner_t *);
    zbar_image_t *zbar_image_create(void);
    void zbar_image_destroy(zbar_image_t *);
    void zbar_image_set_format(zbar_image_t *, unsigned long);
    void zbar_image_set_size(zbar_image_t *, unsigned, unsigned);
    void zbar_image_set_data(zbar_image_t *, const void *, unsigned long, void (*)(zbar_image_t *));
    int zbar_scan_image(zbar_image_scanner_t *, zbar_image_t *);
    const zbar_symbol_t *zbar_image_first_symbol(const zbar_image_t *);
    const zbar_symbol_t *zbar_symbol_next(const zbar_symbol_t *);
    const char *zbar_symbol_get_data(const zbar_symbol_t *);
    unsigned zbar_symbol_get_data_length(const zbar_symbol_t *);
    unsigned zbar_symbol_get_loc_size(const zbar_symbol_t *);
    int zbar_symbol_get_loc_x(const zbar_symbol_t *, unsigned);
    int zbar_symbol_get_loc_y(const zbar_symbol_t *, unsigned);

    int zbar_image_scanner_set_config(zbar_image_scanner_t *, int symbology, int config, int value);

    int zbar_symbol_get_type(const zbar_symbol_t *);
    const char *zbar_get_symbol_name(int);
}

namespace
{

/**
 * @brief 使用 ZBar 扫描单通道灰度图像。
 *
 * 识别图像中的二维码或条形码，并返回解码结果和目标位置。
 */
ScanResult scan_zbar(zbar_image_scanner_t *scanner, const cv::Mat &gray)
{
    zbar_image_t *image = zbar_image_create();
    zbar_image_set_format(image, 0x30303859UL); // Y800
    zbar_image_set_size(image, gray.cols, gray.rows);
    zbar_image_set_data(image, gray.data, gray.total(), nullptr);
    zbar_scan_image(scanner, image);

    ScanResult result;
    for (const zbar_symbol_t *symbol = zbar_image_first_symbol(image); symbol; symbol = zbar_symbol_next(symbol))
    {
        const int type = zbar_symbol_get_type(symbol);

        std::string value(zbar_symbol_get_data(symbol), zbar_symbol_get_data_length(symbol));

        std::cout << "[ZBAR] type=" << zbar_get_symbol_name(type) << ", value=" << value << std::endl;

        result.values.emplace(value);

        std::vector<cv::Point> points;

        for (unsigned i = 0; i < zbar_symbol_get_loc_size(symbol); ++i)
        {
            points.emplace_back(zbar_symbol_get_loc_x(symbol, i), zbar_symbol_get_loc_y(symbol, i));
        }

        if (points.size() >= 2)
        {
            result.boxes.push_back(cv::boundingRect(points));
        }
    }
    zbar_image_destroy(image);
    return result;
}

} // namespace

BarcodeScanner::BarcodeScanner() : scanner_(zbar_image_scanner_create())
{
    auto *scanner = static_cast<zbar_image_scanner_t *>(scanner_);
    // Keep the common QR and 1D formats enabled, but turn off DataBar. On this
    // scene it produced the known false numeric result "175764".
    constexpr int ZBAR_DATABAR = 34;
    constexpr int ZBAR_DATABAR_EXP = 35;
    constexpr int ZBAR_CFG_ENABLE = 0;
    zbar_image_scanner_set_config(scanner, ZBAR_DATABAR, ZBAR_CFG_ENABLE, 0);
    zbar_image_scanner_set_config(scanner, ZBAR_DATABAR_EXP, ZBAR_CFG_ENABLE, 0);
}

BarcodeScanner::~BarcodeScanner()
{
    zbar_image_scanner_destroy(static_cast<zbar_image_scanner_t *>(scanner_));
}

/**
 * @brief 扫描灰度图中的二维码或条形码。
 *
 * 先针对 video13 的有限范围亮度做去噪二值化，再回退到原图扫描。
 */
ScanResult BarcodeScanner::scan(const cv::Mat &gray)
{
    auto *scanner = static_cast<zbar_image_scanner_t *>(scanner_);

    if (!scanner || gray.empty())
        return {};

    // NV12 luma from video13 is limited range. Expanding it and applying a
    // small blur removes paper texture that otherwise fragments Code 128 bars.
    cv::Mat full_range, smooth, binary;
    gray.convertTo(full_range, CV_8U, 255.0 / 219.0, -16.0 * 255.0 / 219.0);
    cv::GaussianBlur(full_range, smooth, cv::Size(), 1.5);
    cv::threshold(smooth, binary, 100, 255, cv::THRESH_BINARY);

    auto result = scan_zbar(scanner, binary);
    if (!result.values.empty())
        return result;

    return scan_zbar(scanner, gray);
}

/**
 * @brief 在视频帧中绘制识别框并发布 JPEG 图像。
 *
 * 根据扫描图像和输出图像的尺寸比例映射检测框，
 * 然后编码为 JPEG 并更新全局最新帧。
 */
void publish_frame(cv::Mat frame, const std::vector<cv::Rect> &boxes, const cv::Size &scan_size)
{
    const double sx = static_cast<double>(frame.cols) / scan_size.width;

    const double sy = static_cast<double>(frame.rows) / scan_size.height;

    for (const cv::Rect &box : boxes)
    {
        cv::Rect output_box(cvRound(box.x * sx), cvRound(box.y * sy), std::max(1, cvRound(box.width * sx)),
                            std::max(1, cvRound(box.height * sy)));

        cv::rectangle(frame, output_box, cv::Scalar(0, 255, 50), 3);
    }

    std::vector<uchar> jpeg;

    cv::imencode(".jpg", frame, jpeg, {cv::IMWRITE_JPEG_QUALITY, 75});

    {
        std::lock_guard<std::mutex> lock(frame_mu);
        latest_jpeg.swap(jpeg);
        ++frame_seq;
    }

    frame_cv.notify_all();
}

/**
 * @brief 处理一帧视频并执行条形码识别。
 *
 * 将输入 BGR 图像转换为灰度图，执行扫码，
 * 上报识别结果，并发布带检测框的视频帧。
 */
void process_frame(cv::Mat frame, BarcodeScanner &scanner)
{
    cv::Mat gray;

    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    auto result = scanner.scan(gray);

    for (const auto &value : result.values)
        detected(value);

    publish_frame(frame, result.boxes, gray.size());
}
