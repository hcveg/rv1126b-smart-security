#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "retinaface.h"

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: %s <model.rknn> <input.jpg> [output.jpg]\n", argv[0]);
        return 2;
    }

    const char* model_path = argv[1];
    const char* image_path = argv[2];
    const char* output_path = argc >= 4 ? argv[3] : "/tmp/face_ai_npu_result.jpg";

    cv::Mat bgr = cv::imread(image_path, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        std::fprintf(stderr, "read image failed: %s\n", image_path);
        return 1;
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

    rknn_app_context_t app_ctx;
    std::memset(&app_ctx, 0, sizeof(app_ctx));

    int ret = init_retinaface_model(model_path, &app_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "init_retinaface_model failed: ret=%d model=%s\n", ret, model_path);
        return 1;
    }

    retinaface_result result;
    std::memset(&result, 0, sizeof(result));
    const auto t0 = std::chrono::steady_clock::now();
    ret = inference_retinaface_model(&app_ctx, &src_image, &result);
    const auto t1 = std::chrono::steady_clock::now();
    const double infer_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ret != 0) {
        std::fprintf(stderr, "inference_retinaface_model failed: ret=%d\n", ret);
        release_retinaface_model(&app_ctx);
        return 1;
    }

    std::printf("faces=%d inference_ms=%.2f image=%dx%d\n", result.count, infer_ms, rgb.cols, rgb.rows);
    for (int i = 0; i < result.count; ++i) {
        const retinaface_object_t& face = result.object[i];
        const int left = clamp_int(face.box.left, 0, rgb.cols - 1);
        const int top = clamp_int(face.box.top, 0, rgb.rows - 1);
        const int right = clamp_int(face.box.right, 0, rgb.cols - 1);
        const int bottom = clamp_int(face.box.bottom, 0, rgb.rows - 1);
        const int width = std::max(1, right - left);
        const int height = std::max(1, bottom - top);

        std::printf("face[%d] box=(%d,%d,%d,%d) score=%.3f\n", i, left, top, right, bottom, face.score);
        cv::rectangle(rgb, cv::Rect(left, top, width, height), cv::Scalar(0, 255, 0), 2);

        char score_text[32];
        std::snprintf(score_text, sizeof(score_text), "%.2f", face.score);
        cv::putText(rgb, score_text, cv::Point(left, std::max(18, top - 4)), cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    cv::Scalar(255, 0, 0), 2);

        for (int j = 0; j < 5; ++j) {
            const int x = clamp_int(face.ponit[j].x, 0, rgb.cols - 1);
            const int y = clamp_int(face.ponit[j].y, 0, rgb.rows - 1);
            cv::circle(rgb, cv::Point(x, y), 2, cv::Scalar(255, 128, 0), 2);
        }
    }

    cv::Mat out_bgr;
    cv::cvtColor(rgb, out_bgr, cv::COLOR_RGB2BGR);
    if (!cv::imwrite(output_path, out_bgr)) {
        std::fprintf(stderr, "write image failed: %s\n", output_path);
        release_retinaface_model(&app_ctx);
        return 1;
    }

    ret = release_retinaface_model(&app_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "release_retinaface_model failed: ret=%d\n", ret);
        return 1;
    }

    std::printf("result=%s\n", output_path);
    return 0;
}
