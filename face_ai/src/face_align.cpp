#include "face_align.h"

#include <algorithm>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

static int clamp_int_local(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

cv::Rect expanded_face_rect(const retinaface_object_t& face, int image_w, int image_h) {
    const int left = clamp_int_local(face.box.left, 0, image_w - 1);
    const int top = clamp_int_local(face.box.top, 0, image_h - 1);
    const int right = clamp_int_local(face.box.right, 0, image_w - 1);
    const int bottom = clamp_int_local(face.box.bottom, 0, image_h - 1);
    const int width = std::max(1, right - left);
    const int height = std::max(1, bottom - top);
    const int pad_x = width / 5;
    const int pad_y = height / 4;
    const int x0 = clamp_int_local(left - pad_x, 0, image_w - 1);
    const int y0 = clamp_int_local(top - pad_y, 0, image_h - 1);
    const int x1 = clamp_int_local(right + pad_x, 0, image_w - 1);
    const int y1 = clamp_int_local(bottom + pad_y, 0, image_h - 1);
    return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
}

cv::Mat make_aligned_face_chip(const cv::Mat& bgr, const retinaface_object_t& face, int output_size) {
    if (bgr.empty()) {
        return cv::Mat();
    }

    std::vector<cv::Point2f> src(5);
    bool valid_landmarks = true;
    for (int i = 0; i < 5; ++i) {
        const int x = face.ponit[i].x;
        const int y = face.ponit[i].y;
        if (x <= 0 || y <= 0 || x >= bgr.cols || y >= bgr.rows) {
            valid_landmarks = false;
            break;
        }
        src[i] = cv::Point2f(static_cast<float>(x), static_cast<float>(y));
    }

    if (valid_landmarks) {
        const float scale = static_cast<float>(output_size) / 112.0f;
        std::vector<cv::Point2f> dst;
        dst.push_back(cv::Point2f(38.2946f * scale, 51.6963f * scale));
        dst.push_back(cv::Point2f(73.5318f * scale, 51.5014f * scale));
        dst.push_back(cv::Point2f(56.0252f * scale, 71.7366f * scale));
        dst.push_back(cv::Point2f(41.5493f * scale, 92.3655f * scale));
        dst.push_back(cv::Point2f(70.7299f * scale, 92.2041f * scale));

        cv::Mat transform = cv::estimateAffinePartial2D(src, dst);
        if (!transform.empty()) {
            cv::Mat aligned;
            cv::warpAffine(bgr, aligned, transform, cv::Size(output_size, output_size), cv::INTER_LINEAR,
                           cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0));
            return aligned;
        }
    }

    cv::Rect roi = expanded_face_rect(face, bgr.cols, bgr.rows);
    cv::Mat chip;
    cv::resize(bgr(roi), chip, cv::Size(output_size, output_size), 0, 0, cv::INTER_LINEAR);
    return chip;
}
