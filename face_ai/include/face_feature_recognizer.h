#ifndef FACE_FEATURE_RECOGNIZER_H
#define FACE_FEATURE_RECOGNIZER_H

#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "retinaface.h"
#include "rknn_api.h"

struct FeatureRecognitionResult {
    std::string name = "Unknown";
    float score = 0.0f;
    bool known = false;
};

struct FaceFeaturePerson {
    std::string name;
    std::vector<float> centroid;
    std::vector<std::vector<float> > embeddings;
    int samples = 0;
};

struct FaceFeatureContext {
    rknn_context rknn_ctx = 0;
    rknn_input_output_num io_num;
    rknn_tensor_attr* input_attrs = nullptr;
    rknn_tensor_attr* output_attrs = nullptr;
    int model_channel = 3;
    int model_width = 112;
    int model_height = 112;
    int output_elems = 512;
};

struct FaceFeatureRecognizer {
    FaceFeatureContext feature_ctx;
    std::vector<FaceFeaturePerson> persons;
    bool ready = false;
    float threshold = 0.79f;
    float margin = 0.08f;
    float centroid_threshold = 0.66f;
    float sample_vote_threshold = 0.72f;
    int min_sample_votes = 2;
    int min_face_size = 70;
    int face_size = 112;
};

int init_face_feature_recognizer(FaceFeatureRecognizer* state, const char* feature_model_path,
                                 const char* face_db_dir, float threshold);
void release_face_feature_recognizer(FaceFeatureRecognizer* state);
bool extract_face_embedding(FaceFeatureContext* ctx, const cv::Mat& bgr_face, std::vector<float>* embedding);
std::vector<FeatureRecognitionResult> recognize_face_features(FaceFeatureRecognizer* state, const cv::Mat& bgr,
                                                              const retinaface_result& detections);

#endif
