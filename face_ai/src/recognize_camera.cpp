#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "face_feature_recognizer.h"
#include "retinaface.h"

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
    if (argc < 5 || argc > 7) {
        std::fprintf(stderr,
                     "usage: %s <retinaface_model.rknn> <arcface_model.rknn> <video_device> <face_db_dir> [frames] [threshold]\n",
                     argv[0]);
        return 2;
    }
    const char* detect_model_path = argv[1];
    const char* feature_model_path = argv[2];
    const char* device = argv[3];
    const char* face_db_dir = argv[4];
    const int frames = argc >= 6 ? std::max(1, std::atoi(argv[5])) : 30;
    const float threshold = argc >= 7 ? static_cast<float>(std::atof(argv[6])) : 0.79f;

    rknn_app_context_t detect_ctx;
    std::memset(&detect_ctx, 0, sizeof(detect_ctx));
    int ret = init_retinaface_model(detect_model_path, &detect_ctx);
    if (ret != 0) {
        return 1;
    }

    FaceFeatureRecognizer recognizer;
    recognizer.threshold = threshold;
    ret = init_face_feature_recognizer(&recognizer, feature_model_path, face_db_dir, recognizer.threshold);
    if (ret != 0) {
        release_retinaface_model(&detect_ctx);
        return 1;
    }

    cv::VideoCapture cap(device, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "open camera failed: %s\n", device);
        release_face_feature_recognizer(&recognizer);
        release_retinaface_model(&detect_ctx);
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('Y', 'U', 'Y', 'V'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 30);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 4);

    for (int frame = 0; frame < frames; ++frame) {
        cv::Mat bgr;
        cap >> bgr;
        if (bgr.empty()) {
            usleep(10000);
            continue;
        }
        if (frame % 3 != 0) {
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
        ret = inference_retinaface_model(&detect_ctx, &src_image, &result);
        if (ret != 0 || result.count <= 0) {
            std::printf("frame=%d no_face\n", frame);
            continue;
        }

        retinaface_result single;
        std::memset(&single, 0, sizeof(single));
        const int face_idx = largest_face_index(result);
        if (face_idx >= 0) {
            single.count = 1;
            single.object[0] = result.object[face_idx];
        }

        std::vector<FeatureRecognitionResult> recognized = recognize_face_features(&recognizer, bgr, single);
        const FeatureRecognitionResult& best = recognized.empty() ? FeatureRecognitionResult() : recognized[0];
        std::printf("frame=%d faces=%d best=%s score=%.4f threshold=%.2f known=%d\n", frame, result.count,
                    best.name.c_str(), best.score, recognizer.threshold, best.known ? 1 : 0);
        std::fflush(stdout);
    }

    release_face_feature_recognizer(&recognizer);
    release_retinaface_model(&detect_ctx);
    return 0;
}
