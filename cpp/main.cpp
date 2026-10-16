#include "app_state.hpp"
#include "barcode.hpp"
#include "camera.hpp"
#include "services.hpp"

#include <csignal>
#include <cstdlib>
#include <gst/gst.h>
#include <string>
#include <thread>

namespace
{
void on_signal(int)
{
    stop_app();
}
} // namespace

int main(int argc, char **argv)
{
    const std::string mqtt_host = argc > 1 ? argv[1] : "127.0.0.1";
    const int mqtt_port = argc > 2 ? std::stoi(argv[2]) : 1883;
    const std::string topic = argc > 3 ? argv[3] : "recamera/qr/result";
    const int http_port = argc > 4 ? std::stoi(argv[4]) : 8766;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    gst_init(&argc, &argv);

    std::thread http_thread(http_worker, http_port);
    std::thread mqtt_thread(mqtt_worker, mqtt_host, mqtt_port, topic);
    BarcodeScanner scanner;

    int status;
    if (const char *test_image = getenv("QR_TEST_IMAGE"))
    {
        status = run_test_image(test_image, scanner);
    }
    else
    {
        status = run_camera(scanner);
    }

    stop_app();
    http_thread.join();
    mqtt_thread.join();
    return status;
}
