#include "camera.hpp"

#include "app_state.hpp"
#include "barcode.hpp"
#include "services.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <iostream>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

/**
 * @brief 使用本地图像测试条形码识别流程。
 *
 * 主要用于不连接摄像头时测试扫码和发送功能。
 */
int run_test_image(const char *path, BarcodeScanner &scanner)
{
    cv::Mat frame = cv::imread(path);

    if (frame.empty())
    {
        std::cerr << "Cannot read test image" << std::endl;

        return 2;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));

    process_frame(frame.clone(), scanner);

    std::this_thread::sleep_for(std::chrono::seconds(3));

    return 0;
}

/**
 * @brief 启动摄像头并持续执行条形码识别。
 *
 * 摄像头采集使用 video13 的 1920x1080 NV12。
 * 主线程负责视频采集和 Web 预览，
 * 独立线程负责条形码识别。
 *
 * 去重和 60 秒冷却由 services.cpp 中的 detected() 统一维护。
 */
int run_camera(BarcodeScanner &scanner)
{
    const char *camera = getenv("QR_VIDEO_DEVICE");

    if (!camera || !*camera)
    {
        camera = "/dev/video13";
    }

    const std::string description = std::string("v4l2src device=") + camera +
                                    " ! video/x-raw,"
                                    "format=NV12,"
                                    "width=1920,"
                                    "height=1080,"
                                    "framerate=30/1"
                                    " ! appsink "
                                    "name=qr_sink "
                                    "emit-signals=false "
                                    "max-buffers=1 "
                                    "drop=true "
                                    "sync=false";

    GError *error = nullptr;

    GstElement *pipeline = gst_parse_launch(description.c_str(), &error);

    if (!pipeline)
    {
        std::cerr << "pipeline: " << (error ? error->message : "unknown") << std::endl;

        if (error)
            g_error_free(error);

        return 1;
    }

    if (error)
        g_error_free(error);

    auto *sink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "qr_sink"));

    if (!sink)
    {
        std::cerr << "Cannot find qr_sink" << std::endl;

        gst_object_unref(pipeline);

        return 1;
    }

    GstBus *bus = gst_element_get_bus(pipeline);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
        std::cerr << "Failed to start GStreamer pipeline" << std::endl;

        gst_object_unref(bus);
        gst_object_unref(sink);
        gst_object_unref(pipeline);

        return 1;
    }

    // 扫描线程相关共享数据。
    std::mutex scan_mu;
    std::condition_variable scan_cv;

    cv::Mat pending_gray;

    std::vector<cv::Rect> latest_boxes;

    /**
     * 独立条形码扫描线程。
     * 扫描线程一次只处理一帧。
     * 如果扫描还没有结束，主线程不会继续堆积新任务。
     */
    std::thread scan_thread([&] {
        while (running)
        {
            cv::Mat gray;

            {
                std::unique_lock<std::mutex> lock(scan_mu);

                scan_cv.wait(lock, [&] { return !running || !pending_gray.empty(); });

                if (!running)
                    break;

                gray = std::move(pending_gray);
            }

            const auto scan_start = Clock::now();

            // 执行当前 1080p 帧扫描。
            auto result = scanner.scan(gray);

            const auto scan_end = Clock::now();

            const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - scan_start).count();

            /*
             * 输出当前扫描耗时和结果数量。
             * 后面调性能时非常有用。
             */
            std::cout << "[BARCODE] scan=" << scan_ms << " ms"
                      << ", count=" << result.values.size() << std::endl;

            for (const auto &value : result.values)
            {
                // detected() sends immediately on first observation and then
                // suppresses repeats until the code has left for 60 seconds.
                detected(value);
            }

            // 保存最近一次扫描得到的检测框。
            {
                std::lock_guard<std::mutex> lock(scan_mu);

                latest_boxes = std::move(result.boxes);
            }
        }
    });

    int status = 0;

    /*
     * 流水线移动较慢，
     * 没必要每秒扫描十几次。
     * 这里设置为 250ms。扫描线程忙时不会积压帧，因此实际速率受
     * 解码耗时限制，而不是固定轮询间隔限制。
     */
    constexpr auto SCAN_INTERVAL = std::chrono::milliseconds(250);

    auto next_scan = Clock::now();

    while (running)
    {
        GstSample *sample = gst_app_sink_try_pull_sample(sink, GST_SECOND);

        if (!sample)
        {
            GstMessage *message =
                gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

            if (message)
            {
                GError *gst_error = nullptr;

                gchar *debug = nullptr;

                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR)
                {
                    gst_message_parse_error(message, &gst_error, &debug);
                }

                std::cerr << "GStreamer stopped: " << (gst_error ? gst_error->message : GST_MESSAGE_TYPE_NAME(message))
                          << std::endl;

                if (gst_error)
                    g_error_free(gst_error);

                if (debug)
                    g_free(debug);

                gst_message_unref(message);

                status = 1;

                break;
            }

            continue;
        }

        GstVideoInfo info;
        GstVideoFrame video;

        if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
            !gst_video_frame_map(&video, &info, gst_sample_get_buffer(sample), GST_MAP_READ))
        {
            gst_sample_unref(sample);

            continue;
        }

        /*
         * NV12：
         * Plane 0 = Y
         * Plane 1 = UV
         * 扫条形码只需要 Y 平面。
         */
        cv::Mat y(info.height, info.width, CV_8UC1,

                  GST_VIDEO_FRAME_PLANE_DATA(&video, 0),

                  GST_VIDEO_FRAME_PLANE_STRIDE(&video, 0));

        cv::Mat uv(info.height / 2, info.width / 2, CV_8UC2,

                   GST_VIDEO_FRAME_PLANE_DATA(&video, 1),

                   GST_VIDEO_FRAME_PLANE_STRIDE(&video, 1));

        /*
         * Web 预览图只需要 960x540。
         */
        cv::Mat small_y;
        cv::Mat small_uv;
        cv::Mat frame;

        /*
         * 到达下一次扫码时间后，
         * 才尝试给扫描线程发送一张完整 1080p 灰度图。
         */
        const auto now = Clock::now();

        if (now >= next_scan)
        {
            bool queued_scan = false;

            {
                std::lock_guard<std::mutex> lock(scan_mu);

                /*
                 * pending_gray 非空代表扫码线程还没取走上一帧。
                 * 不进行任务堆积。
                 */
                if (pending_gray.empty())
                {
                    pending_gray = y.clone();

                    queued_scan = true;
                }
            }

            if (queued_scan)
            {
                scan_cv.notify_one();

                next_scan = now + SCAN_INTERVAL;
            }
        }

        /*
         * 缩小 Y / UV 用于浏览器实时预览。
         */
        cv::resize(y, small_y, cv::Size(960, 540), 0, 0, cv::INTER_AREA);

        cv::resize(uv, small_uv, cv::Size(480, 270), 0, 0, cv::INTER_AREA);

        /*
         * resize 后已经不再依赖 GStreamer buffer。
         */
        gst_video_frame_unmap(&video);

        gst_sample_unref(sample);

        /*
         * NV12 -> BGR。
         */
        cv::cvtColorTwoPlane(small_y, small_uv, frame, cv::COLOR_YUV2BGR_NV12);

        /*
         * 获取扫码线程最新的检测框。
         */
        std::vector<cv::Rect> boxes;

        {
            std::lock_guard<std::mutex> lock(scan_mu);

            boxes = latest_boxes;
        }

        /*
         * 检测框坐标属于原始 1080p 图像，
         * publish_frame() 会映射到 960x540。
         */
        publish_frame(frame, boxes, cv::Size(info.width, info.height));
    }

    /*
     * 停止应用和后台扫描线程。
     */
    stop_app();

    scan_cv.notify_all();

    if (scan_thread.joinable())
        scan_thread.join();

    /*
     * 释放 GStreamer。
     */
    gst_element_set_state(pipeline, GST_STATE_NULL);

    gst_object_unref(bus);
    gst_object_unref(sink);
    gst_object_unref(pipeline);

    return status;
}
