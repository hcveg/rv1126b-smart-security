#ifndef FACE_AI_NPU_IMAGE_UTILS_H_
#define FACE_AI_NPU_IMAGE_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "common.h"

typedef struct {
    int x_pad;
    int y_pad;
    float scale;
} letterbox_t;

int convert_image(image_buffer_t* src_image, image_buffer_t* dst_image, image_rect_t* src_box,
                  image_rect_t* dst_box, char color);
int convert_image_with_letterbox(image_buffer_t* src_image, image_buffer_t* dst_image,
                                 letterbox_t* letterbox, char color);
int get_image_size(image_buffer_t* image);

#ifdef __cplusplus
}
#endif

#endif
