#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "retinaface.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int) {
    g_stop = 1;
}

struct FrameState {
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<uchar> jpeg;
    long frame_id = 0;
    int faces = 0;
    double infer_ms = 0.0;
    double total_ms = 0.0;
    double fps = 0.0;
    std::atomic<int> clients{0};
};

struct ClientGuard {
    explicit ClientGuard(FrameState* state) : state_(state) {
        state_->clients.fetch_add(1);
    }
    ~ClientGuard() {
        state_->clients.fetch_sub(1);
    }
    FrameState* state_;
};

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

static bool send_all(int fd, const void* data, size_t size) {
    const char* ptr = static_cast<const char*>(data);
    while (size > 0) {
        ssize_t sent = send(fd, ptr, size, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (sent == 0) {
            return false;
        }
        ptr += sent;
        size -= static_cast<size_t>(sent);
    }
    return true;
}

static bool send_text(int fd, const std::string& text) {
    return send_all(fd, text.data(), text.size());
}

static std::string read_request(int fd) {
    std::string request;
    char buf[1024];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        ssize_t got = recv(fd, buf, sizeof(buf), 0);
        if (got <= 0) {
            break;
        }
        request.append(buf, static_cast<size_t>(got));
    }
    return request;
}

static void draw_result(cv::Mat& bgr, const retinaface_result& result) {
    for (int i = 0; i < result.count; ++i) {
        const retinaface_object_t& face = result.object[i];
        const int left = clamp_int(face.box.left, 0, bgr.cols - 1);
        const int top = clamp_int(face.box.top, 0, bgr.rows - 1);
        const int right = clamp_int(face.box.right, 0, bgr.cols - 1);
        const int bottom = clamp_int(face.box.bottom, 0, bgr.rows - 1);
        const int width = std::max(1, right - left);
        const int height = std::max(1, bottom - top);

        cv::rectangle(bgr, cv::Rect(left, top, width, height), cv::Scalar(0, 255, 0), 2);

        char score_text[32];
        std::snprintf(score_text, sizeof(score_text), "%.2f", face.score);
        cv::putText(bgr, score_text, cv::Point(left, std::max(18, top - 4)), cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    cv::Scalar(0, 0, 255), 2);

        for (int j = 0; j < 5; ++j) {
            const int x = clamp_int(face.ponit[j].x, 0, bgr.cols - 1);
            const int y = clamp_int(face.ponit[j].y, 0, bgr.rows - 1);
            cv::circle(bgr, cv::Point(x, y), 2, cv::Scalar(0, 165, 255), 2);
        }
    }
}

static bool process_frame(cv::VideoCapture& cap, rknn_app_context_t* app_ctx, std::vector<uchar>* jpg,
                          int* face_count, double* infer_ms, double* total_ms) {
    const auto total_start = std::chrono::steady_clock::now();
    cv::Mat bgr;
    cap >> bgr;
    if (bgr.empty()) {
        return false;
    }

    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    if (!rgb.isContinuous()) {
        rgb = rgb.clone();
    }

    image_buffer_t src_image;
    std::memset(&src_image, 0, sizeof(src_image));
    src_image.width = rgb.cols;
    src_image.height = rgb.rows;
    src_image.format = IMAGE_FORMAT_RGB888;
    src_image.virt_addr = rgb.data;
    src_image.size = static_cast<int>(rgb.total() * rgb.elemSize());

    retinaface_result result;
    std::memset(&result, 0, sizeof(result));

    const auto infer_start = std::chrono::steady_clock::now();
    int ret = inference_retinaface_model(app_ctx, &src_image, &result);
    const auto infer_end = std::chrono::steady_clock::now();
    if (ret != 0) {
        return false;
    }

    draw_result(bgr, result);

    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 80};
    if (!cv::imencode(".jpg", bgr, *jpg, params)) {
        return false;
    }

    const auto total_end = std::chrono::steady_clock::now();
    *face_count = result.count;
    *infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
    *total_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    return true;
}

static void capture_worker(cv::VideoCapture* cap, rknn_app_context_t* app_ctx, FrameState* state) {
    int frames_in_window = 0;
    auto window_start = std::chrono::steady_clock::now();

    while (!g_stop) {
        if (state->clients.load() <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        std::vector<uchar> jpg;
        int faces = 0;
        double infer_ms = 0.0;
        double total_ms = 0.0;
        if (!process_frame(*cap, app_ctx, &jpg, &faces, &infer_ms, &total_ms)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        frames_in_window++;
        const auto now = std::chrono::steady_clock::now();
        const double window_sec = std::chrono::duration<double>(now - window_start).count();
        double fps = 0.0;
        if (window_sec >= 1.0) {
            fps = frames_in_window / window_sec;
            frames_in_window = 0;
            window_start = now;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->jpeg.swap(jpg);
            state->frame_id++;
            state->faces = faces;
            state->infer_ms = infer_ms;
            state->total_ms = total_ms;
            if (fps > 0.0) {
                state->fps = fps;
            }
        }
        state->condition.notify_all();
    }
}

static bool send_index(int fd) {
    const char* body =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<title>RV1126B Face Detect</title>"
        "<style>body{margin:0;background:#101214;color:#f4f4f4;font-family:Arial,sans-serif}"
        "header{height:44px;display:flex;align-items:center;padding:0 14px;background:#1b1f23}"
        "img{display:block;width:100vw;height:calc(100vh - 44px);object-fit:contain;background:#000}"
        ".meta{margin-left:auto;color:#b7bec7;font-size:13px}</style></head><body>"
        "<header><strong>RV1126B NPU Face Detect</strong><span class='meta'>/stream.mjpg</span></header>"
        "<img src='/stream.mjpg' alt='live face detection stream'></body></html>";
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
             << std::strlen(body) << "\r\nConnection: close\r\n\r\n"
             << body;
    return send_text(fd, response.str());
}

static bool send_status(int fd, FrameState* state) {
    long frame_id;
    int faces;
    double infer_ms;
    double total_ms;
    double fps;
    int clients = state->clients.load();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        frame_id = state->frame_id;
        faces = state->faces;
        infer_ms = state->infer_ms;
        total_ms = state->total_ms;
        fps = state->fps;
    }

    std::ostringstream body;
    body << "{\"frames\":" << frame_id << ",\"last_faces\":" << faces << ",\"last_infer_ms\":" << infer_ms
         << ",\"last_total_ms\":" << total_ms << ",\"fps\":" << fps << ",\"clients\":" << clients << "}";
    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " << body.str().size()
             << "\r\nConnection: close\r\n\r\n"
             << body.str();
    return send_text(fd, response.str());
}

static bool wait_for_frame(FrameState* state, long after_id, std::vector<uchar>* jpg, long* frame_id, int timeout_ms) {
    std::unique_lock<std::mutex> lock(state->mutex);
    bool ok = state->condition.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return g_stop || (state->frame_id > after_id && !state->jpeg.empty());
    });
    if (!ok || state->jpeg.empty()) {
        return false;
    }
    *jpg = state->jpeg;
    *frame_id = state->frame_id;
    return true;
}

static void handle_client(int client_fd, FrameState* state) {
    std::string request = read_request(client_fd);
    if (request.find("GET /status") == 0) {
        send_status(client_fd, state);
        return;
    }

    if (request.find("GET /snapshot.jpg") == 0) {
        ClientGuard guard(state);
        long old_id = 0;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            old_id = state->frame_id;
        }
        std::vector<uchar> jpg;
        long frame_id = 0;
        if (!wait_for_frame(state, old_id, &jpg, &frame_id, 3000)) {
            send_text(client_fd, "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n");
            return;
        }
        std::ostringstream header;
        header << "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: " << jpg.size()
               << "\r\nConnection: close\r\n\r\n";
        send_text(client_fd, header.str());
        send_all(client_fd, jpg.data(), jpg.size());
        return;
    }

    if (request.find("GET /stream.mjpg") != 0) {
        send_index(client_fd);
        return;
    }

    ClientGuard guard(state);
    const char* header =
        "HTTP/1.1 200 OK\r\n"
        "Age: 0\r\n"
        "Cache-Control: no-cache, private\r\n"
        "Pragma: no-cache\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
    if (!send_text(client_fd, header)) {
        return;
    }

    long last_id = -1;
    while (!g_stop) {
        std::vector<uchar> jpg;
        long frame_id = 0;
        if (!wait_for_frame(state, last_id, &jpg, &frame_id, 2000)) {
            continue;
        }
        last_id = frame_id;

        std::ostringstream part;
        part << "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " << jpg.size() << "\r\n\r\n";
        if (!send_text(client_fd, part.str()) || !send_all(client_fd, jpg.data(), jpg.size()) ||
            !send_text(client_fd, "\r\n")) {
            return;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 7) {
        std::fprintf(stderr, "usage: %s <model.rknn> <video_device> <port> [width] [height] [fps]\n", argv[0]);
        return 2;
    }

    const char* model_path = argv[1];
    const char* device = argv[2];
    const int port = std::atoi(argv[3]);
    const int width = argc >= 5 ? std::atoi(argv[4]) : 640;
    const int height = argc >= 6 ? std::atoi(argv[5]) : 480;
    const int fps = argc >= 7 ? std::atoi(argv[6]) : 15;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    rknn_app_context_t app_ctx;
    std::memset(&app_ctx, 0, sizeof(app_ctx));
    int ret = init_retinaface_model(model_path, &app_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "init_retinaface_model failed: %d\n", ret);
        return 1;
    }

    cv::VideoCapture cap(device, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "open camera failed: %s\n", device);
        release_retinaface_model(&app_ctx);
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);
    cap.set(cv::CAP_PROP_FPS, fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        release_retinaface_model(&app_ctx);
        return 1;
    }

    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        release_retinaface_model(&app_ctx);
        return 1;
    }
    if (listen(server_fd, 8) < 0) {
        perror("listen");
        close(server_fd);
        release_retinaface_model(&app_ctx);
        return 1;
    }

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    FrameState state;
    std::thread worker(capture_worker, &cap, &app_ctx, &state);

    std::printf("open http://0.0.0.0:%d/ device=%s size=%dx%d fps=%d\n", port, device, width, height, fps);
    std::fflush(stdout);

    while (!g_stop) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            perror("accept");
            break;
        }
        std::thread(handle_client, client_fd, &state).detach();
    }

    close(server_fd);
    state.condition.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    release_retinaface_model(&app_ctx);
    return 0;
}
