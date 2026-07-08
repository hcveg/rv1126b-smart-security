#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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

static bool has_image_extension(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return lower.size() >= 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 || lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".bmp") == lower.size() - 4 || lower.rfind(".jpeg") == lower.size() - 5);
}

static std::vector<std::string> list_images(const std::string& dir_path) {
    std::vector<std::string> images;
    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        std::fprintf(stderr, "open image dir failed: %s: %s\n", dir_path.c_str(), strerror(errno));
        return images;
    }
    dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        std::string path = dir_path + "/" + entry->d_name;
        if (has_image_extension(path)) {
            images.push_back(path);
        }
    }
    closedir(dir);
    std::sort(images.begin(), images.end());
    return images;
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
    if (argc < 5 || argc > 6) {
        std::fprintf(stderr, "usage: %s <model.rknn> <input_image_dir> <person_name> <face_db_dir> [max_samples]\n",
                     argv[0]);
        return 2;
    }

    const char* model_path = argv[1];
    const std::string input_dir = argv[2];
    const std::string person_name = argv[3];
    const std::string face_db_dir = argv[4];
    const int max_samples = argc >= 6 ? std::max(1, std::atoi(argv[5])) : 1000;
    const std::string person_dir = face_db_dir + "/" + person_name;

    std::vector<std::string> images = list_images(input_dir);
    if (images.empty()) {
        std::fprintf(stderr, "no images found: %s\n", input_dir.c_str());
        return 1;
    }
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

    int saved = 0;
    for (size_t i = 0; i < images.size() && saved < max_samples; ++i) {
        cv::Mat bgr = cv::imread(images[i], cv::IMREAD_COLOR);
        if (bgr.empty()) {
            std::fprintf(stderr, "read image failed: %s\n", images[i].c_str());
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
            std::fprintf(stderr, "no face detected: %s\n", images[i].c_str());
            continue;
        }

        const int face_index = largest_face_index(result);
        if (face_index < 0) {
            std::fprintf(stderr, "no usable face: %s\n", images[i].c_str());
            continue;
        }

        cv::Rect roi = expanded_face_rect(result.object[face_index], bgr.cols, bgr.rows);
        if (roi.width < 70 || roi.height < 70) {
            std::fprintf(stderr, "face too small: %s %dx%d\n", images[i].c_str(), roi.width, roi.height);
            continue;
        }

        char out_path[512];
        std::snprintf(out_path, sizeof(out_path), "%s/%02d.jpg", person_dir.c_str(), saved + 1);
        cv::Mat face_chip = make_aligned_face_chip(bgr, result.object[face_index], 112);
        if (face_chip.empty() || !cv::imwrite(out_path, face_chip)) {
            std::fprintf(stderr, "write crop failed: %s\n", out_path);
            continue;
        }

        saved++;
        std::printf("registered %d: %s <- %s faces=%d\n", saved, out_path, images[i].c_str(), result.count);
        std::fflush(stdout);
    }

    release_retinaface_model(&app_ctx);
    std::printf("done person=%s saved=%d input=%zu db=%s\n", person_name.c_str(), saved, images.size(),
                person_dir.c_str());
    return saved > 0 ? 0 : 1;
}
