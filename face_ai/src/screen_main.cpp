#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "retinaface.h"

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
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

static cv::Mat fit_to_screen(const cv::Mat& frame, int screen_w, int screen_h) {
    cv::Mat canvas(screen_h, screen_w, frame.type(), cv::Scalar(0, 0, 0));
    const double scale = std::min(static_cast<double>(screen_w) / frame.cols, static_cast<double>(screen_h) / frame.rows);
    const int out_w = std::max(1, static_cast<int>(frame.cols * scale));
    const int out_h = std::max(1, static_cast<int>(frame.rows * scale));
    const int x = (screen_w - out_w) / 2;
    const int y = (screen_h - out_h) / 2;

    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(out_w, out_h), 0, 0, cv::INTER_LINEAR);
    resized.copyTo(canvas(cv::Rect(x, y, out_w, out_h)));
    return canvas;
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 9) {
        std::fprintf(stderr,
                     "usage: %s <model.rknn> <video_device> [camera_width] [camera_height] [fps] [screen_width] "
                     "[screen_height] [detect_interval]\n",
                     argv[0]);
        return 2;
    }

    const char* model_path = argv[1];
    const char* device = argv[2];
    const int camera_w = argc >= 4 ? std::atoi(argv[3]) : 640;
    const int camera_h = argc >= 5 ? std::atoi(argv[4]) : 480;
    const int fps = argc >= 6 ? std::atoi(argv[5]) : 15;
    const int screen_w = argc >= 7 ? std::atoi(argv[6]) : 1024;
    const int screen_h = argc >= 8 ? std::atoi(argv[7]) : 600;
    const int detect_interval = std::max(1, argc >= 9 ? std::atoi(argv[8]) : 2);

    rknn_app_context_t app_ctx;
    std::memset(&app_ctx, 0, sizeof(app_ctx));
    int ret = init_retinaface_model(model_path, &app_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "init_retinaface_model failed: ret=%d model=%s\n", ret, model_path);
        return 1;
    }

    cv::VideoCapture cap(device, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "open camera failed: %s\n", device);
        release_retinaface_model(&app_ctx);
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, camera_w);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, camera_h);
    cap.set(cv::CAP_PROP_FPS, fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    const std::string window_name = "RV1126B NPU Face Detect";
    cv::namedWindow(window_name, cv::WINDOW_NORMAL);
    cv::resizeWindow(window_name, screen_w, screen_h);
    cv::setWindowProperty(window_name, cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

    long frame_count = 0;
    int frames_in_window = 0;
    double display_fps = 0.0;
    auto fps_window_start = std::chrono::steady_clock::now();

    std::printf("screen display started: device=%s camera=%dx%d fps=%d screen=%dx%d detect_interval=%d\n", device,
                camera_w, camera_h, fps, screen_w, screen_h, detect_interval);
    std::fflush(stdout);

    retinaface_result last_result;
    std::memset(&last_result, 0, sizeof(last_result));
    double last_infer_ms = 0.0;
    int fps_report_count = 0;

    while (true) {
        cv::Mat bgr;
        cap >> bgr;
        if (bgr.empty()) {
            std::fprintf(stderr, "camera returned empty frame\n");
            cv::waitKey(10);
            continue;
        }

        if (frame_count % detect_interval == 0) {
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
            ret = inference_retinaface_model(&app_ctx, &src_image, &result);
            const auto infer_end = std::chrono::steady_clock::now();
            if (ret != 0) {
                std::fprintf(stderr, "inference_retinaface_model failed: ret=%d\n", ret);
                continue;
            }
            last_result = result;
            last_infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
        }

        draw_result(bgr, last_result);

        frame_count++;
        frames_in_window++;
        const auto now = std::chrono::steady_clock::now();
        const double window_sec = std::chrono::duration<double>(now - fps_window_start).count();
        if (window_sec >= 1.0) {
            display_fps = frames_in_window / window_sec;
            frames_in_window = 0;
            fps_window_start = now;
            fps_report_count++;
            if (fps_report_count % 5 == 0) {
                std::printf("display_fps=%.1f faces=%d npu=%.1fms detect_interval=%d\n", display_fps,
                            last_result.count, last_infer_ms, detect_interval);
                std::fflush(stdout);
            }
        }

        char overlay[128];
        std::snprintf(overlay, sizeof(overlay), "faces:%d  npu:%.1fms  det:1/%d", last_result.count,
                      last_infer_ms, detect_interval);
        cv::putText(bgr, overlay, cv::Point(16, 32), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 0), 4);
        cv::putText(bgr, overlay, cv::Point(16, 32), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

        const char* scale_env = std::getenv("SCALE_TO_SCREEN");
        if (scale_env != nullptr && std::string(scale_env) == "1") {
            cv::Mat screen = fit_to_screen(bgr, screen_w, screen_h);
            cv::imshow(window_name, screen);
        } else {
            cv::imshow(window_name, bgr);
        }

        const int key = cv::waitKey(1) & 0xff;
        if (key == 27 || key == 'q' || key == 'Q') {
            break;
        }
    }

    release_retinaface_model(&app_ctx);
    return 0;
}
