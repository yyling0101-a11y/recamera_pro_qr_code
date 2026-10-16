#include "services.hpp"

#include "app_state.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C"
{
    struct mosquitto;
    int mosquitto_lib_init(void);
    int mosquitto_lib_cleanup(void);
    mosquitto *mosquitto_new(const char *, bool, void *);
    void mosquitto_destroy(mosquitto *);
    int mosquitto_connect_async(mosquitto *, const char *, int, int);
    int mosquitto_username_pw_set(mosquitto *, const char *, const char *);
    int mosquitto_loop_start(mosquitto *);
    int mosquitto_loop_stop(mosquitto *, bool);
    int mosquitto_publish(mosquitto *, int *, const char *, int, const void *, int, bool);
}

namespace
{
using Clock = std::chrono::steady_clock;
struct Seen
{
    Clock::time_point last_seen;
    unsigned consecutive;
    bool emitted;
};

std::mutex clients_mu;
std::mutex events_mu;
std::vector<int> ws_clients;
std::deque<std::string> mqtt_pending;
std::map<std::string, Seen> seen;
constexpr auto cooldown = std::chrono::seconds(60);

bool send_all(int fd, const void *data, size_t size)
{
    const auto *p = static_cast<const char *>(data);
    while (size)
    {
        ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool send_ws_text(int fd, const std::string &value)
{
    std::vector<unsigned char> header{0x81};
    const uint64_t size = value.size();
    if (size < 126)
    {
        header.push_back(static_cast<unsigned char>(size));
    }
    else if (size <= 65535)
    {
        header.push_back(126);
        header.push_back(size >> 8);
        header.push_back(size);
    }
    else
    {
        header.push_back(127);
        for (int i = 7; i >= 0; --i)
            header.push_back(size >> (i * 8));
    }
    return send_all(fd, header.data(), header.size()) && send_all(fd, value.data(), value.size());
}

void publish_ws(const std::string &value)
{
    std::lock_guard<std::mutex> lock(clients_mu);
    for (auto it = ws_clients.begin(); it != ws_clients.end();)
    {
        if (!send_ws_text(*it, value))
        {
            close(*it);
            it = ws_clients.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

std::string header_value(const std::string &request, const std::string &name)
{
    auto begin = request.find(name);
    if (begin == std::string::npos)
        return {};
    begin += name.size();
    auto end = request.find("\r\n", begin);
    auto value = request.substr(begin, end - begin);
    while (!value.empty() && value.front() == ' ')
        value.erase(value.begin());
    return value;
}

bool serve_index(int fd)
{
    const char *root = getenv("QR_WEB_ROOT");
    const std::string path = std::string(root && *root ? root : "/userdata/qr_rec/www") + "/index.html";
    std::ifstream page(path, std::ios::binary);
    if (!page)
        return false;
    const std::string body((std::istreambuf_iterator<char>(page)), std::istreambuf_iterator<char>());
    const std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                             "Cache-Control: no-store\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    return send_all(fd, head.data(), head.size()) && send_all(fd, body.data(), body.size());
}

void serve_client(int fd)
{
    timeval timeout{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char input[8192];
    const ssize_t bytes = recv(fd, input, sizeof(input) - 1, 0);
    if (bytes <= 0)
    {
        close(fd);
        return;
    }
    input[bytes] = 0;
    std::string request(input, bytes);

    if (request.rfind("GET / ", 0) == 0 || request.rfind("GET /index.html ", 0) == 0)
    {
        if (!serve_index(fd))
        {
            const char *reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
            send_all(fd, reply, strlen(reply));
        }
        close(fd);
        return;
    }
    else if (request.rfind("GET /ws ", 0) == 0)
    {
        auto key = header_value(request, "Sec-WebSocket-Key:");
        if (key.empty())
        {
            close(fd);
            return;
        }
        std::string token = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        gchar *hex = g_compute_checksum_for_string(G_CHECKSUM_SHA1, token.c_str(), token.size());
        unsigned char digest[20];
        for (int i = 0; i < 20; ++i)
        {
            unsigned value;
            sscanf(hex + 2 * i, "%02x", &value);
            digest[i] = value;
        }
        g_free(hex);
        gchar *base64 = g_base64_encode(digest, 20);
        std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                            std::string(base64) + "\r\n\r\n";
        g_free(base64);
        if (!send_all(fd, reply.data(), reply.size()))
        {
            close(fd);
            return;
        }

        // A browser can reconnect just after the first event was emitted.
        // Replay the codes still in the 60-second cooldown window to this
        // client only; MQTT and existing clients do not receive duplicates.
        std::vector<std::string> replay;
        {
            std::lock_guard<std::mutex> events_lock(events_mu);
            const auto now = Clock::now();
            for (const auto &[value, item] : seen)
            {
                if (now - item.last_seen < cooldown)
                    replay.push_back(value);
            }
        }
        for (const auto &value : replay)
        {
            if (!send_ws_text(fd, value))
            {
                close(fd);
                return;
            }
        }
        std::lock_guard<std::mutex> lock(clients_mu);
        ws_clients.push_back(fd);
        return;
    }

    if (request.rfind("GET /video.mjpg ", 0) == 0)
    {
        const char *head = "HTTP/1.1 200 OK\r\nContent-Type: multipart/x-mixed-replace; "
                           "boundary=frame\r\nCache-Control: no-store\r\n"
                           "Access-Control-Allow-Origin: *\r\n\r\n";
        if (!send_all(fd, head, strlen(head)))
        {
            close(fd);
            return;
        }
        unsigned long last = 0;
        while (running)
        {
            std::vector<uchar> jpeg;
            {
                std::unique_lock<std::mutex> lock(frame_mu);
                frame_cv.wait_for(lock, std::chrono::seconds(2), [&] { return !running || frame_seq != last; });
                if (!running)
                    break;
                last = frame_seq;
                jpeg = latest_jpeg;
            }
            if (jpeg.empty())
                continue;
            std::string part =
                "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpeg.size()) + "\r\n\r\n";
            if (!send_all(fd, part.data(), part.size()) || !send_all(fd, jpeg.data(), jpeg.size()) ||
                !send_all(fd, "\r\n", 2))
            {
                break;
            }
        }
    }
    else
    {
        const char *reply = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                            "Content-Length: 2\r\n\r\nOK";
        send_all(fd, reply, strlen(reply));
    }
    close(fd);
}
} // namespace

void detected(const std::string &value)
{
    if (value.empty())
        return;
    const auto now = Clock::now();
    bool emit = false;
    {
        std::lock_guard<std::mutex> lock(events_mu);
        auto it = seen.find(value);
        if (it == seen.end())
        {
            seen.emplace(value, Seen{now, 1, true});
            emit = true;
        }
        else
        {
            const auto gap = now - it->second.last_seen;
            if (gap >= cooldown)
            {
                it->second.consecutive = 1;
                it->second.emitted = true;
                emit = true;
            }
            else
                ++it->second.consecutive;
            it->second.last_seen = now;
        }
        if (emit)
        {
            if (mqtt_pending.size() == 1000)
                mqtt_pending.pop_front();
            mqtt_pending.push_back(value);
        }
        for (auto it = seen.begin(); it != seen.end();)
        {
            if (now - it->second.last_seen > std::chrono::hours(24))
            {
                it = seen.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }
    if (emit)
    {
        publish_ws(value);
        std::cout << "detected: " << value << std::endl;
    }
}

void mqtt_worker(std::string host, int port, std::string topic)
{
    mosquitto_lib_init();
    mosquitto *client = mosquitto_new("qr-pro", true, nullptr);
    if (!client)
    {
        mosquitto_lib_cleanup();
        return;
    }
    const char *user = getenv("QR_MQTT_USER");
    const char *password = getenv("QR_MQTT_PASSWORD");
    if (user && password)
        mosquitto_username_pw_set(client, user, password);
    mosquitto_connect_async(client, host.c_str(), port, 30);
    mosquitto_loop_start(client);
    while (running)
    {
        std::string value;
        {
            std::lock_guard<std::mutex> lock(events_mu);
            if (!mqtt_pending.empty())
                value = mqtt_pending.front();
        }
        if (!value.empty() &&
            mosquitto_publish(client, nullptr, topic.c_str(), value.size(), value.data(), 1, false) == 0)
        {
            std::lock_guard<std::mutex> lock(events_mu);
            if (!mqtt_pending.empty() && mqtt_pending.front() == value)
                mqtt_pending.pop_front();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    mosquitto_loop_stop(client, true);
    mosquitto_destroy(client);
    mosquitto_lib_cleanup();
}

void http_worker(int port)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    if (server < 0)
    {
        stop_app();
        return;
    }
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    timeval timeout{1, 0};
    setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = INADDR_ANY;
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(server, 16))
    {
        perror("HTTP bind/listen");
        close(server);
        stop_app();
        return;
    }
    while (running)
    {
        int fd = accept(server, nullptr, nullptr);
        if (fd >= 0)
            std::thread(serve_client, fd).detach();
    }
    close(server);
}
