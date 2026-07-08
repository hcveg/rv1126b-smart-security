#include "face_feature_recognizer.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "common.h"
#include "face_align.h"
#include "file_utils.h"

static void dump_tensor_attr_local(rknn_tensor_attr* attr) {
    std::printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, "
                "qnt_type=%s, zp=%d, scale=%f\n",
                attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
                attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
                get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

static bool is_directory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_image_extension(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return lower.size() >= 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 || lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".bmp") == lower.size() - 4 || lower.rfind(".jpeg") == lower.size() - 5);
}

static bool normalize_embedding(std::vector<float>* embedding) {
    double norm = 0.0;
    for (float value : *embedding) {
        norm += static_cast<double>(value) * static_cast<double>(value);
    }
    norm = std::sqrt(norm);
    if (norm <= 1e-12) {
        return false;
    }
    for (float& value : *embedding) {
        value = static_cast<float>(value / norm);
    }
    return true;
}

static float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) {
        return -1.0f;
    }
    double dot = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * static_cast<double>(b[i]);
    }
    return static_cast<float>(dot);
}

static int init_feature_model(const char* model_path, FaceFeatureContext* ctx) {
    int model_len = 0;
    char* model = nullptr;
    model_len = read_data_from_file(model_path, &model);
    if (model_len <= 0 || model == nullptr) {
        std::printf("load feature model failed: %s\n", model_path);
        return -1;
    }

    rknn_context rknn_ctx = 0;
    int ret = rknn_init(&rknn_ctx, model, model_len, 0, nullptr);
    std::free(model);
    if (ret < 0) {
        std::printf("feature rknn_init failed: ret=%d model=%s\n", ret, model_path);
        return -1;
    }

    rknn_input_output_num io_num;
    std::memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(rknn_ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        std::printf("feature rknn_query io failed: ret=%d\n", ret);
        rknn_destroy(rknn_ctx);
        return -1;
    }
    std::printf("feature model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr* input_attrs = static_cast<rknn_tensor_attr*>(std::calloc(io_num.n_input, sizeof(rknn_tensor_attr)));
    rknn_tensor_attr* output_attrs =
        static_cast<rknn_tensor_attr*>(std::calloc(io_num.n_output, sizeof(rknn_tensor_attr)));
    if (input_attrs == nullptr || output_attrs == nullptr) {
        std::free(input_attrs);
        std::free(output_attrs);
        rknn_destroy(rknn_ctx);
        return -1;
    }

    std::printf("feature input tensors:\n");
    for (uint32_t i = 0; i < io_num.n_input; ++i) {
        input_attrs[i].index = i;
        ret = rknn_query(rknn_ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            std::printf("feature input attr query failed: ret=%d\n", ret);
            std::free(input_attrs);
            std::free(output_attrs);
            rknn_destroy(rknn_ctx);
            return -1;
        }
        dump_tensor_attr_local(&input_attrs[i]);
    }

    std::printf("feature output tensors:\n");
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        output_attrs[i].index = i;
        ret = rknn_query(rknn_ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            std::printf("feature output attr query failed: ret=%d\n", ret);
            std::free(input_attrs);
            std::free(output_attrs);
            rknn_destroy(rknn_ctx);
            return -1;
        }
        dump_tensor_attr_local(&output_attrs[i]);
    }

    ctx->rknn_ctx = rknn_ctx;
    ctx->io_num = io_num;
    ctx->input_attrs = input_attrs;
    ctx->output_attrs = output_attrs;
    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        ctx->model_channel = input_attrs[0].dims[1];
        ctx->model_height = input_attrs[0].dims[2];
        ctx->model_width = input_attrs[0].dims[3];
    } else {
        ctx->model_height = input_attrs[0].dims[1];
        ctx->model_width = input_attrs[0].dims[2];
        ctx->model_channel = input_attrs[0].dims[3];
    }
    ctx->output_elems = output_attrs[0].n_elems > 0 ? output_attrs[0].n_elems : 512;
    std::printf("feature model input height=%d, width=%d, channel=%d output=%d\n", ctx->model_height,
                ctx->model_width, ctx->model_channel, ctx->output_elems);
    return 0;
}

static void release_feature_model(FaceFeatureContext* ctx) {
    if (ctx->input_attrs != nullptr) {
        std::free(ctx->input_attrs);
        ctx->input_attrs = nullptr;
    }
    if (ctx->output_attrs != nullptr) {
        std::free(ctx->output_attrs);
        ctx->output_attrs = nullptr;
    }
    if (ctx->rknn_ctx != 0) {
        rknn_destroy(ctx->rknn_ctx);
        ctx->rknn_ctx = 0;
    }
}

bool extract_face_embedding(FaceFeatureContext* ctx, const cv::Mat& bgr_face, std::vector<float>* embedding) {
    if (ctx == nullptr || ctx->rknn_ctx == 0 || bgr_face.empty() || embedding == nullptr) {
        return false;
    }

    cv::Mat resized;
    cv::resize(bgr_face, resized, cv::Size(ctx->model_width, ctx->model_height), 0, 0, cv::INTER_LINEAR);
    cv::Mat rgb;
    cv::cvtColor(resized, rgb, cv::COLOR_BGR2RGB);
    if (!rgb.isContinuous()) {
        rgb = rgb.clone();
    }

    std::vector<uint8_t> input;
    rknn_tensor_format input_fmt = ctx->input_attrs[0].fmt;
    if (input_fmt == RKNN_TENSOR_NCHW) {
        input.resize(ctx->model_channel * ctx->model_height * ctx->model_width);
        for (int c = 0; c < ctx->model_channel; ++c) {
            for (int y = 0; y < ctx->model_height; ++y) {
                const uint8_t* row = rgb.ptr<uint8_t>(y);
                for (int x = 0; x < ctx->model_width; ++x) {
                    input[c * ctx->model_height * ctx->model_width + y * ctx->model_width + x] =
                        row[x * ctx->model_channel + c];
                }
            }
        }
    } else {
        input.assign(rgb.data, rgb.data + rgb.total() * rgb.elemSize());
    }

    rknn_input inputs[1];
    std::memset(inputs, 0, sizeof(inputs));
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = input_fmt;
    inputs[0].size = static_cast<uint32_t>(input.size());
    inputs[0].buf = input.data();

    int ret = rknn_inputs_set(ctx->rknn_ctx, 1, inputs);
    if (ret < 0) {
        std::printf("feature rknn_inputs_set failed: ret=%d\n", ret);
        return false;
    }

    ret = rknn_run(ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        std::printf("feature rknn_run failed: ret=%d\n", ret);
        return false;
    }

    rknn_output output;
    std::memset(&output, 0, sizeof(output));
    output.index = 0;
    output.want_float = 1;
    ret = rknn_outputs_get(ctx->rknn_ctx, 1, &output, nullptr);
    if (ret < 0) {
        std::printf("feature rknn_outputs_get failed: ret=%d\n", ret);
        return false;
    }

    const int elems = ctx->output_elems > 0 ? ctx->output_elems : static_cast<int>(output.size / sizeof(float));
    const float* values = static_cast<const float*>(output.buf);
    embedding->assign(values, values + elems);
    rknn_outputs_release(ctx->rknn_ctx, 1, &output);
    return normalize_embedding(embedding);
}

static bool load_face_database(const char* face_db_dir, FaceFeatureRecognizer* state) {
    state->persons.clear();
    DIR* root = opendir(face_db_dir);
    if (root == nullptr) {
        std::printf("face feature recognition disabled: db not found: %s\n", face_db_dir);
        return false;
    }

    int total_samples = 0;
    dirent* person_entry = nullptr;
    while ((person_entry = readdir(root)) != nullptr) {
        if (person_entry->d_name[0] == '.') {
            continue;
        }
        const std::string person_name = person_entry->d_name;
        const std::string person_dir = std::string(face_db_dir) + "/" + person_name;
        if (!is_directory(person_dir)) {
            continue;
        }

        std::vector<float> sum;
        int sample_count = 0;
        DIR* person = opendir(person_dir.c_str());
        if (person == nullptr) {
            continue;
        }

        std::vector<std::vector<float> > sample_embeddings;
        dirent* image_entry = nullptr;
        while ((image_entry = readdir(person)) != nullptr) {
            if (image_entry->d_name[0] == '.') {
                continue;
            }
            const std::string image_path = person_dir + "/" + image_entry->d_name;
            if (!has_image_extension(image_path)) {
                continue;
            }
            cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
            if (image.empty()) {
                continue;
            }

            cv::Mat face_chip;
            cv::resize(image, face_chip, cv::Size(state->face_size, state->face_size), 0, 0, cv::INTER_LINEAR);
            std::vector<float> embedding;
            if (!extract_face_embedding(&state->feature_ctx, face_chip, &embedding)) {
                continue;
            }
            if (sum.empty()) {
                sum.assign(embedding.size(), 0.0f);
            }
            if (sum.size() != embedding.size()) {
                continue;
            }
            for (size_t i = 0; i < sum.size(); ++i) {
                sum[i] += embedding[i];
            }
            sample_embeddings.push_back(embedding);
            sample_count++;
        }
        closedir(person);

        if (sample_count > 0 && normalize_embedding(&sum)) {
            FaceFeaturePerson person_feature;
            person_feature.name = person_name;
            person_feature.centroid = sum;
            person_feature.embeddings = sample_embeddings;
            person_feature.samples = sample_count;
            state->persons.push_back(person_feature);
            total_samples += sample_count;
        }
    }
    closedir(root);

    if (state->persons.empty()) {
        std::printf("face feature recognition disabled: empty db: %s\n", face_db_dir);
        return false;
    }

    std::printf("face feature recognition loaded: persons=%zu samples=%d threshold=%.2f margin=%.2f centroid=%.2f "
                "vote=%.2f min_votes=%d min_size=%d db=%s\n",
                state->persons.size(), total_samples, state->threshold, state->margin, state->centroid_threshold,
                state->sample_vote_threshold, state->min_sample_votes, state->min_face_size, face_db_dir);
    return true;
}

int init_face_feature_recognizer(FaceFeatureRecognizer* state, const char* feature_model_path,
                                 const char* face_db_dir, float threshold) {
    if (state == nullptr) {
        return -1;
    }
    state->ready = false;
    state->threshold = threshold;
    int ret = init_feature_model(feature_model_path, &state->feature_ctx);
    if (ret != 0) {
        return ret;
    }
    state->ready = load_face_database(face_db_dir, state);
    return state->ready ? 0 : -1;
}

void release_face_feature_recognizer(FaceFeatureRecognizer* state) {
    if (state == nullptr) {
        return;
    }
    release_feature_model(&state->feature_ctx);
    state->persons.clear();
    state->ready = false;
}

std::vector<FeatureRecognitionResult> recognize_face_features(FaceFeatureRecognizer* state, const cv::Mat& bgr,
                                                              const retinaface_result& detections) {
    std::vector<FeatureRecognitionResult> recognized(detections.count);
    if (state == nullptr || !state->ready || state->persons.empty()) {
        return recognized;
    }

    for (int i = 0; i < detections.count; ++i) {
        const retinaface_object_t& detection = detections.object[i];
        const int face_w = std::max(0, detection.box.right - detection.box.left);
        const int face_h = std::max(0, detection.box.bottom - detection.box.top);
        if (face_w < state->min_face_size || face_h < state->min_face_size) {
            continue;
        }

        cv::Mat face_chip = make_aligned_face_chip(bgr, detections.object[i], state->face_size);
        std::vector<float> embedding;
        if (!extract_face_embedding(&state->feature_ctx, face_chip, &embedding)) {
            continue;
        }

        float best_score = -1.0f;
        float second_score = -1.0f;
        int best_index = -1;
        float best_centroid_score = -1.0f;
        int best_vote_count = 0;
        for (size_t person_idx = 0; person_idx < state->persons.size(); ++person_idx) {
            const float centroid_score = cosine_similarity(embedding, state->persons[person_idx].centroid);
            float score = centroid_score;
            int vote_count = 0;
            for (const std::vector<float>& sample_embedding : state->persons[person_idx].embeddings) {
                const float sample_score = cosine_similarity(embedding, sample_embedding);
                score = std::max(score, sample_score);
                if (sample_score >= state->sample_vote_threshold) {
                    vote_count++;
                }
            }
            if (score > best_score) {
                second_score = best_score;
                best_score = score;
                best_index = static_cast<int>(person_idx);
                best_centroid_score = centroid_score;
                best_vote_count = vote_count;
            } else if (score > second_score) {
                second_score = score;
            }
        }
        recognized[i].score = best_score;
        const bool margin_ok = state->persons.size() <= 1 || (best_score - second_score) >= state->margin;
        const bool centroid_ok = best_centroid_score >= state->centroid_threshold;
        const bool votes_ok = best_vote_count >= state->min_sample_votes;
        if (best_index >= 0 && best_score >= state->threshold && centroid_ok && votes_ok && margin_ok) {
            recognized[i].name = state->persons[best_index].name;
            recognized[i].known = true;
        }
    }
    return recognized;
}
