#ifndef FACE_ALIGN_H
#define FACE_ALIGN_H

#include <opencv2/core.hpp>

#include "retinaface.h"

cv::Rect expanded_face_rect(const retinaface_object_t& face, int image_w, int image_h);
cv::Mat make_aligned_face_chip(const cv::Mat& bgr, const retinaface_object_t& face, int output_size);

#endif
