#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "face_align.h"
#include "retinaface.h"

static bool ensure_dir(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != '/') {
            continue;
        }
        std::string part = path.substr(0, i);
        if (part.empty()) {
            continue;
        }
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "mkdir failed: %s: %s\n", part.c_str(), strerror(errno));
            return false;
        }
    }
    return true;
}

static int largest_face_index(const retinaface_result& result) {
    int best = -1;
    int best_area = 0;
    for (int i = 0; i < result.count; ++i) {
        const retinaface_object_t& face = result.object[i];
        const int area = std::max(0, face.box.right - face.box.left) * std::max(0, face.box.bottom - face.box.top);
        if (area > best_area) {
            best_area = area;
            best = i;
        }
    }
    return best;
}

int main(int argc, char** argv) {
    if (argc < 4 || argc > 6) {
        std::fprintf(stderr,
                     "usage: %s <model.rknn> <video_device> <person_name> [sample_count] [face_db_dir]\n",
                     argv[0]);
        return 2;
    }

    const char* model_path = argv[1];
    const char* device = argv[2];
    const std::string person_name = argv[3];
    const int target_count = std::max(1, argc >= 5 ? std::atoi(argv[4]) : 8);
    const std::string face_db_dir = argc >= 6 ? argv[5] : "/userdata/face_ai/faces";
    const std::string person_dir = face_db_dir + "/" + person_name;
    if (!ensure_dir(person_dir)) {
        return 1;
    }

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
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 30);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 4);

    int saved = 0;
    int frame_index = 0;
    const int max_frames = target_count * 80;
    while (saved < target_count && frame_index < max_frames) {
        cv::Mat bgr;
        cap >> bgr;
        if (bgr.empty()) {
            usleep(10000);
            continue;
        }
        frame_index++;
        if (frame_index % 6 != 0) {
            continue;
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
        ret = inference_retinaface_model(&app_ctx, &src_image, &result);
        if (ret != 0 || result.count <= 0) {
            std::printf("waiting for face... frame=%d\n", frame_index);
            std::fflush(stdout);
            continue;
        }

        const int face_index = largest_face_index(result);
        if (face_index < 0) {
            continue;
        }
        const cv::Rect roi = expanded_face_rect(result.object[face_index], bgr.cols, bgr.rows);
        if (roi.width < 70 || roi.height < 70) {
            std::printf("face too small: %dx%d\n", roi.width, roi.height);
            std::fflush(stdout);
            continue;
        }

        std::time_t now = std::time(nullptr);
        std::tm tm_now;
        localtime_r(&now, &tm_now);
        char filename[256];
        std::snprintf(filename, sizeof(filename), "%s/%04d%02d%02d_%02d%02d%02d_%02d.jpg", person_dir.c_str(),
                      tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday, tm_now.tm_hour, tm_now.tm_min,
                      tm_now.tm_sec, saved + 1);
        cv::Mat face_chip = make_aligned_face_chip(bgr, result.object[face_index], 112);
        if (face_chip.empty() || !cv::imwrite(filename, face_chip)) {
            std::fprintf(stderr, "write sample failed: %s\n", filename);
            continue;
        }
        saved++;
        std::printf("saved %d/%d: %s\n", saved, target_count, filename);
        std::fflush(stdout);
        usleep(250000);
    }

    release_retinaface_model(&app_ctx);
    if (saved <= 0) {
        std::fprintf(stderr, "no face samples saved\n");
        return 1;
    }
    std::printf("registered person=%s samples=%d dir=%s\n", person_name.c_str(), saved, person_dir.c_str());
    return saved >= target_count ? 0 : 1;
}
