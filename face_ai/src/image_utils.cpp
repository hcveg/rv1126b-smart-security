#include "image_utils.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <opencv2/imgproc.hpp>

extern "C" int get_image_size(image_buffer_t* image) {
    if (image == nullptr) {
        return 0;
    }

    switch (image->format) {
        case IMAGE_FORMAT_GRAY8:
            return image->width * image->height;
        case IMAGE_FORMAT_RGB888:
            return image->width * image->height * 3;
        case IMAGE_FORMAT_RGBA8888:
            return image->width * image->height * 4;
        case IMAGE_FORMAT_YUV420SP_NV21:
        case IMAGE_FORMAT_YUV420SP_NV12:
            return image->width * image->height * 3 / 2;
        default:
            return 0;
    }
}

static bool check_rgb_image(const image_buffer_t* image) {
    return image != nullptr && image->virt_addr != nullptr && image->width > 0 && image->height > 0 &&
           image->format == IMAGE_FORMAT_RGB888;
}

extern "C" int convert_image(image_buffer_t* src_image, image_buffer_t* dst_image, image_rect_t* src_box,
                             image_rect_t* dst_box, char color) {
    if (!check_rgb_image(src_image) || dst_image == nullptr || dst_image->width <= 0 || dst_image->height <= 0 ||
        dst_image->format != IMAGE_FORMAT_RGB888) {
        return -1;
    }

    dst_image->size = get_image_size(dst_image);
    if (dst_image->virt_addr == nullptr) {
        dst_image->virt_addr = static_cast<unsigned char*>(std::malloc(dst_image->size));
        if (dst_image->virt_addr == nullptr) {
            return -1;
        }
    }

    cv::Mat src(src_image->height, src_image->width, CV_8UC3, src_image->virt_addr);
    cv::Mat dst(dst_image->height, dst_image->width, CV_8UC3, dst_image->virt_addr);
    dst.setTo(cv::Scalar(color, color, color));

    cv::Rect src_rect(0, 0, src_image->width, src_image->height);
    if (src_box != nullptr) {
        src_rect = cv::Rect(src_box->left, src_box->top, src_box->right - src_box->left + 1,
                            src_box->bottom - src_box->top + 1) &
                   cv::Rect(0, 0, src_image->width, src_image->height);
    }

    cv::Rect dst_rect(0, 0, dst_image->width, dst_image->height);
    if (dst_box != nullptr) {
        dst_rect = cv::Rect(dst_box->left, dst_box->top, dst_box->right - dst_box->left + 1,
                            dst_box->bottom - dst_box->top + 1) &
                   cv::Rect(0, 0, dst_image->width, dst_image->height);
    }

    if (src_rect.empty() || dst_rect.empty()) {
        return -1;
    }

    cv::Mat resized;
    cv::resize(src(src_rect), resized, dst_rect.size(), 0, 0, cv::INTER_LINEAR);
    resized.copyTo(dst(dst_rect));
    return 0;
}

extern "C" int convert_image_with_letterbox(image_buffer_t* src_image, image_buffer_t* dst_image,
                                            letterbox_t* letterbox, char color) {
    if (!check_rgb_image(src_image) || dst_image == nullptr || dst_image->width <= 0 || dst_image->height <= 0 ||
        dst_image->format != IMAGE_FORMAT_RGB888 || letterbox == nullptr) {
        return -1;
    }

    const float scale_w = static_cast<float>(dst_image->width) / static_cast<float>(src_image->width);
    const float scale_h = static_cast<float>(dst_image->height) / static_cast<float>(src_image->height);
    const float scale = std::min(scale_w, scale_h);
    const int resize_w = std::max(1, static_cast<int>(src_image->width * scale));
    const int resize_h = std::max(1, static_cast<int>(src_image->height * scale));
    const int x_pad = (dst_image->width - resize_w) / 2;
    const int y_pad = (dst_image->height - resize_h) / 2;

    letterbox->x_pad = x_pad;
    letterbox->y_pad = y_pad;
    letterbox->scale = scale;

    image_rect_t src_box = {0, 0, src_image->width - 1, src_image->height - 1};
    image_rect_t dst_box = {x_pad, y_pad, x_pad + resize_w - 1, y_pad + resize_h - 1};
    return convert_image(src_image, dst_image, &src_box, &dst_box, color);
}
