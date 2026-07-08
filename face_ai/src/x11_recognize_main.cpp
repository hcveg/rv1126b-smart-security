#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "face_feature_recognizer.h"
#include "retinaface.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int) {
    g_stop = 1;
}

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

static int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return std::atoi(value);
}

static float env_float(const char* name, float fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return static_cast<float>(std::atof(value));
}

static std::string env_string(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : fallback;
}

static int read_int_config_value(const std::string& path, const std::string& key, int fallback, int min_value) {
    if (path.empty()) {
        return std::max(min_value, fallback);
    }
    std::ifstream in(path.c_str());
    if (!in) {
        return std::max(min_value, fallback);
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t pos = text.find("\"" + key + "\"");
    if (pos == std::string::npos) {
        pos = text.find(key);
    }
    if (pos == std::string::npos) {
        return std::max(min_value, fallback);
    }
    pos = text.find(':', pos);
    if (pos == std::string::npos) {
        return std::max(min_value, fallback);
    }
    ++pos;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '"' || text[pos] == '\'')) {
        ++pos;
    }
    bool negative = false;
    if (pos < text.size() && text[pos] == '-') {
        negative = true;
        ++pos;
    }
    int value = 0;
    bool found = false;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        found = true;
        value = value * 10 + (text[pos] - '0');
        ++pos;
    }
    if (!found) {
        return std::max(min_value, fallback);
    }
    if (negative) {
        value = -value;
    }
    return std::max(min_value, value);
}

static long long wall_time_ms() {
    return static_cast<long long>(std::time(nullptr)) * 1000LL;
}

static std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (char ch : text) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << ch;
            break;
        }
    }
    return out.str();
}

static cv::Rect face_rect(const retinaface_object_t& face) {
    const int left = std::min(face.box.left, face.box.right);
    const int top = std::min(face.box.top, face.box.bottom);
    const int right = std::max(face.box.left, face.box.right);
    const int bottom = std::max(face.box.top, face.box.bottom);
    return cv::Rect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

static double rect_iou(const cv::Rect& a, const cv::Rect& b) {
    const int x1 = std::max(a.x, b.x);
    const int y1 = std::max(a.y, b.y);
    const int x2 = std::min(a.x + a.width, b.x + b.width);
    const int y2 = std::min(a.y + a.height, b.y + b.height);
    const int inter_w = std::max(0, x2 - x1);
    const int inter_h = std::max(0, y2 - y1);
    const int inter_area = inter_w * inter_h;
    const int union_area = a.area() + b.area() - inter_area;
    return union_area > 0 ? static_cast<double>(inter_area) / union_area : 0.0;
}

struct StableRecognitionSlot {
    std::string candidate_name;
    std::string stable_name;
    float stable_score = 0.0f;
    cv::Rect box;
    int hits = 0;
    int missed = 0;
    bool active = false;
};

static int match_stable_slot(const cv::Rect& box, const std::vector<StableRecognitionSlot>& slots,
                             const std::vector<bool>& used) {
    int best_index = -1;
    double best_score = 0.0;
    const int cx = box.x + box.width / 2;
    const int cy = box.y + box.height / 2;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (used[i] || !slots[i].active) {
            continue;
        }
        const cv::Rect& old_box = slots[i].box;
        const double iou = rect_iou(box, old_box);
        const int old_cx = old_box.x + old_box.width / 2;
        const int old_cy = old_box.y + old_box.height / 2;
        const int dx = cx - old_cx;
        const int dy = cy - old_cy;
        const int max_w = std::max(box.width, old_box.width);
        const int max_h = std::max(box.height, old_box.height);
        const int max_center_dist_sq = std::max(40 * 40, (max_w * max_w + max_h * max_h) / 4);
        double score = iou;
        if (dx * dx + dy * dy <= max_center_dist_sq) {
            score = std::max(score, 0.25);
        }
        if (score > best_score) {
            best_score = score;
            best_index = static_cast<int>(i);
        }
    }
    return best_score >= 0.15 ? best_index : -1;
}

static std::vector<FeatureRecognitionResult> stabilize_recognitions(const std::vector<FeatureRecognitionResult>& raw,
                                                                    const retinaface_result& detections,
                                                                    std::vector<StableRecognitionSlot>* slots,
                                                                    int min_hits, int hold_frames) {
    std::vector<FeatureRecognitionResult> stable = raw;
    std::vector<StableRecognitionSlot> next_slots;
    std::vector<bool> used(slots->size(), false);
    next_slots.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i) {
        StableRecognitionSlot slot;
        if (i < static_cast<size_t>(std::max(0, detections.count))) {
            const cv::Rect box = face_rect(detections.object[i]);
            const int matched_index = match_stable_slot(box, *slots, used);
            if (matched_index >= 0) {
                slot = (*slots)[matched_index];
                used[matched_index] = true;
            }
            slot.box = box;
            slot.active = true;
        }

        if (!raw[i].known) {
            slot.missed++;
            if (!slot.stable_name.empty() && slot.missed <= hold_frames) {
                stable[i].known = true;
                stable[i].name = slot.stable_name;
                stable[i].score = std::max(raw[i].score, slot.stable_score);
            } else {
                slot.candidate_name.clear();
                slot.hits = 0;
            }
            next_slots.push_back(slot);
            continue;
        }

        if (slot.candidate_name == raw[i].name) {
            slot.hits++;
        } else {
            slot.candidate_name = raw[i].name;
            slot.hits = 1;
        }
        slot.missed = 0;

        if (slot.hits >= min_hits) {
            slot.stable_name = raw[i].name;
            slot.stable_score = raw[i].score;
        } else if (slot.stable_name == raw[i].name) {
            stable[i].known = true;
            stable[i].name = slot.stable_name;
            stable[i].score = std::max(raw[i].score, slot.stable_score);
        } else {
            stable[i].known = false;
            stable[i].name = "Unknown";
        }
        next_slots.push_back(slot);
    }
    *slots = next_slots;
    return stable;
}

static void draw_result(cv::Mat& bgr, const retinaface_result& result,
                        const std::vector<FeatureRecognitionResult>& recognized) {
    for (int i = 0; i < result.count; ++i) {
        const retinaface_object_t& face = result.object[i];
        const int left = clamp_int(face.box.left, 0, bgr.cols - 1);
        const int top = clamp_int(face.box.top, 0, bgr.rows - 1);
        const int right = clamp_int(face.box.right, 0, bgr.cols - 1);
        const int bottom = clamp_int(face.box.bottom, 0, bgr.rows - 1);
        const int width = std::max(1, right - left);
        const int height = std::max(1, bottom - top);

        const bool known = i < static_cast<int>(recognized.size()) && recognized[i].known;
        cv::rectangle(bgr, cv::Rect(left, top, width, height),
                      known ? cv::Scalar(0, 255, 255) : cv::Scalar(0, 255, 0), 2);

        char label_text[96];
        if (known) {
            std::snprintf(label_text, sizeof(label_text), "%s %.2f", recognized[i].name.c_str(), recognized[i].score);
        } else if (i < static_cast<int>(recognized.size()) && recognized[i].score > 0.0f) {
            std::snprintf(label_text, sizeof(label_text), "Unknown %.2f", recognized[i].score);
        } else {
            std::snprintf(label_text, sizeof(label_text), "%.2f", face.score);
        }
        const int label_y = std::max(22, top - 5);
        cv::putText(bgr, label_text, cv::Point(left, label_y), cv::FONT_HERSHEY_SIMPLEX, 0.65,
                    cv::Scalar(0, 0, 0), 4);
        cv::putText(bgr, label_text, cv::Point(left, label_y), cv::FONT_HERSHEY_SIMPLEX, 0.65,
                    known ? cv::Scalar(0, 255, 255) : cv::Scalar(0, 0, 255), 2);

        for (int j = 0; j < 5; ++j) {
            const int x = clamp_int(face.ponit[j].x, 0, bgr.cols - 1);
            const int y = clamp_int(face.ponit[j].y, 0, bgr.rows - 1);
            cv::circle(bgr, cv::Point(x, y), 2, cv::Scalar(0, 165, 255), 2);
        }
    }
}

static cv::Mat fill_window_crop_center(const cv::Mat& frame, int window_w, int window_h) {
    const double scale = std::max(static_cast<double>(window_w) / frame.cols, static_cast<double>(window_h) / frame.rows);
    const int out_w = std::max(1, static_cast<int>(frame.cols * scale));
    const int out_h = std::max(1, static_cast<int>(frame.rows * scale));

    cv::Mat resized;
    cv::resize(frame, resized, cv::Size(out_w, out_h), 0, 0, cv::INTER_LINEAR);
    const int x = std::max(0, (out_w - window_w) / 2);
    const int y = std::max(0, (out_h - window_h) / 2);
    return resized(cv::Rect(x, y, window_w, window_h)).clone();
}

struct SecurityTrack {
    int id = 0;
    cv::Rect box;
    std::chrono::steady_clock::time_point first_seen;
    std::chrono::steady_clock::time_point last_seen;
    std::chrono::steady_clock::time_point last_alert;
    bool entry_reported = false;
    bool alert_sent = false;
    bool updated = false;
};

struct SecurityState {
    std::vector<SecurityTrack> tracks;
    int next_track_id = 1;
    int stranger_entries = 0;
    int stranger_alerts = 0;
    int current_faces = 0;
    int current_known = 0;
    int current_unknown = 0;
    double longest_unknown_sec = 0.0;
    std::string last_alert_text;
    std::vector<std::string> current_known_names;
};

static bool contains_name(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

static double security_track_score(const cv::Rect& box, const cv::Rect& old_box) {
    const int cx = box.x + box.width / 2;
    const int cy = box.y + box.height / 2;
    const double iou = rect_iou(box, old_box);
    const int old_cx = old_box.x + old_box.width / 2;
    const int old_cy = old_box.y + old_box.height / 2;
    const int dx = cx - old_cx;
    const int dy = cy - old_cy;
    const int max_w = std::max(box.width, old_box.width);
    const int max_h = std::max(box.height, old_box.height);
    const int max_center_dist_sq = std::max(50 * 50, (max_w * max_w + max_h * max_h) / 3);
    double score = iou;
    if (dx * dx + dy * dy <= max_center_dist_sq) {
        score = std::max(score, 0.25);
    }
    return score;
}

static int match_security_track(const cv::Rect& box, const std::vector<SecurityTrack>& tracks) {
    int best_index = -1;
    double best_score = 0.0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const double score = security_track_score(box, tracks[i].box);
        if (score > best_score) {
            best_score = score;
            best_index = static_cast<int>(i);
        }
    }
    return best_score >= 0.15 ? best_index : -1;
}

static void append_security_event(const std::string& path, const std::string& type, int track_id, double dwell_sec,
                                  int stranger_entries, int stranger_alerts, int current_unknown) {
    std::ofstream out(path.c_str(), std::ios::app);
    if (!out) {
        return;
    }
    out << "{\"ts_ms\":" << wall_time_ms() << ",\"type\":\"" << json_escape(type) << "\",\"track_id\":"
        << track_id << ",\"dwell_sec\":" << dwell_sec << ",\"stranger_entries\":" << stranger_entries
        << ",\"stranger_alerts\":" << stranger_alerts << ",\"current_unknown\":" << current_unknown << "}\n";
}

static void write_security_status(const std::string& path, const SecurityState& state,
                                  const std::chrono::steady_clock::time_point& start_time,
                                  const std::chrono::steady_clock::time_point& now, int alert_seconds,
                                  int alert_interval_seconds, double entry_confirm_seconds) {
    const double uptime = std::chrono::duration<double>(now - start_time).count();
    const std::string tmp_path = path + ".tmp";
    std::ofstream out(tmp_path.c_str(), std::ios::trunc);
    if (!out) {
        return;
    }
    out << "{";
    out << "\"ts_ms\":" << wall_time_ms();
    out << ",\"uptime_sec\":" << uptime;
    out << ",\"alert_seconds\":" << alert_seconds;
    out << ",\"alert_interval_seconds\":" << alert_interval_seconds;
    out << ",\"min_alert_interval_seconds\":3";
    out << ",\"entry_confirm_seconds\":" << entry_confirm_seconds;
    out << ",\"current_faces\":" << state.current_faces;
    out << ",\"current_known\":" << state.current_known;
    out << ",\"current_unknown\":" << state.current_unknown;
    out << ",\"longest_unknown_sec\":" << state.longest_unknown_sec;
    out << ",\"stranger_entries\":" << state.stranger_entries;
    out << ",\"stranger_alerts\":" << state.stranger_alerts;
    out << ",\"active_unknown_tracks\":" << state.tracks.size();
    out << ",\"last_alert\":\"" << json_escape(state.last_alert_text) << "\"";
    out << ",\"known_names\":[";
    for (size_t i = 0; i < state.current_known_names.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "\"" << json_escape(state.current_known_names[i]) << "\"";
    }
    out << "]}";
    out.close();
    std::rename(tmp_path.c_str(), path.c_str());
}

static void update_security_state(SecurityState* state, const retinaface_result& detections,
                                  const std::vector<FeatureRecognitionResult>& recognized,
                                  const std::chrono::steady_clock::time_point& now, int alert_seconds,
                                  int alert_interval_seconds, double entry_confirm_seconds,
                                  const std::string& events_file) {
    for (SecurityTrack& track : state->tracks) {
        track.updated = false;
    }
    state->current_faces = std::max(0, detections.count);
    state->current_known = 0;
    state->current_unknown = 0;
    state->longest_unknown_sec = 0.0;
    state->current_known_names.clear();

    std::vector<cv::Rect> known_boxes;
    for (int i = 0; i < detections.count; ++i) {
        const bool known = i < static_cast<int>(recognized.size()) && recognized[i].known;
        if (known) {
            state->current_known++;
            if (!contains_name(state->current_known_names, recognized[i].name)) {
                state->current_known_names.push_back(recognized[i].name);
            }
            known_boxes.push_back(face_rect(detections.object[i]));
        }
    }

    if (!known_boxes.empty() && !state->tracks.empty()) {
        std::vector<SecurityTrack> unknown_only_tracks;
        unknown_only_tracks.reserve(state->tracks.size());
        for (const SecurityTrack& track : state->tracks) {
            bool now_known = false;
            for (const cv::Rect& known_box : known_boxes) {
                if (security_track_score(known_box, track.box) >= 0.15) {
                    now_known = true;
                    break;
                }
            }
            if (!now_known) {
                unknown_only_tracks.push_back(track);
            }
        }
        state->tracks.swap(unknown_only_tracks);
    }

    for (int i = 0; i < detections.count; ++i) {
        const bool known = i < static_cast<int>(recognized.size()) && recognized[i].known;
        if (known) {
            continue;
        }

        state->current_unknown++;
        const cv::Rect box = face_rect(detections.object[i]);
        int track_index = match_security_track(box, state->tracks);
        if (track_index < 0) {
            SecurityTrack track;
            track.id = state->next_track_id++;
            track.box = box;
            track.first_seen = now;
            track.last_seen = now;
            track.updated = true;
            state->tracks.push_back(track);
            track_index = static_cast<int>(state->tracks.size()) - 1;
        } else {
            SecurityTrack& track = state->tracks[track_index];
            track.box = box;
            track.last_seen = now;
            track.updated = true;
        }

        SecurityTrack& track = state->tracks[track_index];
        const double dwell_sec = std::chrono::duration<double>(now - track.first_seen).count();
        state->longest_unknown_sec = std::max(state->longest_unknown_sec, dwell_sec);
        if (!track.entry_reported && dwell_sec >= entry_confirm_seconds) {
            track.entry_reported = true;
            state->stranger_entries++;
            append_security_event(events_file, "stranger_entry", track.id, dwell_sec, state->stranger_entries,
                                  state->stranger_alerts, state->current_unknown);
        }
        const double since_last_alert_sec =
            track.alert_sent ? std::chrono::duration<double>(now - track.last_alert).count() : 0.0;
        if (dwell_sec >= alert_seconds && (!track.alert_sent || since_last_alert_sec >= alert_interval_seconds)) {
            if (!track.entry_reported) {
                track.entry_reported = true;
                state->stranger_entries++;
                append_security_event(events_file, "stranger_entry", track.id, dwell_sec, state->stranger_entries,
                                      state->stranger_alerts, state->current_unknown);
            }
            const bool repeat_alert = track.alert_sent;
            track.alert_sent = true;
            track.last_alert = now;
            state->stranger_alerts++;
            char alert[160];
            std::snprintf(alert, sizeof(alert), repeat_alert ? "陌生人仍在画面内，已停留 %.1f 秒"
                                                             : "陌生人已在画面内停留 %.1f 秒",
                          dwell_sec);
            state->last_alert_text = alert;
            append_security_event(events_file, "stranger_dwell", track.id, dwell_sec, state->stranger_entries,
                                  state->stranger_alerts, state->current_unknown);
        }
    }

    std::vector<SecurityTrack> active;
    active.reserve(state->tracks.size());
    for (const SecurityTrack& track : state->tracks) {
        const double missing_sec = std::chrono::duration<double>(now - track.last_seen).count();
        if (missing_sec <= 2.5) {
            active.push_back(track);
        }
    }
    state->tracks.swap(active);
}

int main(int argc, char** argv) {
    if (argc < 5 || argc > 11) {
        std::fprintf(stderr,
                     "usage: %s <retinaface_model.rknn> <arcface_model.rknn> <video_device> <face_db_dir> "
                     "[camera_width] [camera_height] [fps] [window_width] [window_height] [detect_interval]\n",
                     argv[0]);
        return 2;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    const char* detect_model_path = argv[1];
    const char* feature_model_path = argv[2];
    const char* device = argv[3];
    const char* face_db_dir = argv[4];
    const int camera_w = argc >= 6 ? std::atoi(argv[5]) : 640;
    const int camera_h = argc >= 7 ? std::atoi(argv[6]) : 480;
    const int fps = argc >= 8 ? std::atoi(argv[7]) : 15;
    const int window_w = argc >= 9 ? std::atoi(argv[8]) : 512;
    const int window_h = argc >= 10 ? std::atoi(argv[9]) : 310;
    const int detect_interval = std::max(1, argc >= 11 ? std::atoi(argv[10]) : 2);
    const int window_x = env_int("WINDOW_X", 0);
    const int window_y = env_int("WINDOW_Y", 0);
    const int camera_buffers = std::max(1, env_int("CAMERA_BUFFERS", 2));
    const std::string pixel_format = env_string("PIXEL_FORMAT", "MJPG");

    rknn_app_context_t detect_ctx;
    std::memset(&detect_ctx, 0, sizeof(detect_ctx));
    int ret = init_retinaface_model(detect_model_path, &detect_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "init_retinaface_model failed: ret=%d model=%s\n", ret, detect_model_path);
        return 1;
    }

    FaceFeatureRecognizer recognizer;
    recognizer.threshold = env_float("FACE_RECOG_THRESHOLD", recognizer.threshold);
    recognizer.margin = env_float("FACE_RECOG_MARGIN", recognizer.margin);
    recognizer.centroid_threshold = env_float("FACE_RECOG_CENTROID_THRESHOLD", recognizer.centroid_threshold);
    recognizer.sample_vote_threshold = env_float("FACE_RECOG_SAMPLE_VOTE_THRESHOLD", recognizer.sample_vote_threshold);
    recognizer.min_sample_votes = std::max(1, env_int("FACE_RECOG_MIN_SAMPLE_VOTES", recognizer.min_sample_votes));
    recognizer.min_face_size = env_int("FACE_RECOG_MIN_SIZE", recognizer.min_face_size);
    init_face_feature_recognizer(&recognizer, feature_model_path, face_db_dir, recognizer.threshold);
    const int face_recog_interval = std::max(1, env_int("FACE_RECOG_INTERVAL", 3));
    const int face_recog_stable_hits = std::max(1, env_int("FACE_RECOG_STABLE_HITS", 2));
    const int face_recog_hold_frames = std::max(0, env_int("FACE_RECOG_HOLD_FRAMES", 8));
    const int stranger_alert_seconds = std::max(1, env_int("STRANGER_ALERT_SECONDS", 10));
    const int initial_alert_interval_seconds =
        std::max(3, env_int("STRANGER_ALERT_INTERVAL_SECONDS", stranger_alert_seconds));
    const double stranger_entry_confirm_seconds =
        std::max(0.0f, env_float("STRANGER_ENTRY_CONFIRM_SECONDS", 1.0f));
    const std::string security_status_file =
        env_string("SECURITY_STATUS_FILE", "/tmp/face_ai_security_status.json");
    const std::string security_events_file =
        env_string("SECURITY_EVENTS_FILE", "/tmp/face_ai_security_events.jsonl");
    const std::string security_config_file =
        env_string("SECURITY_CONFIG_FILE", "/tmp/face_ai_security_config.json");

    cv::VideoCapture cap(device, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "open camera failed: %s\n", device);
        release_face_feature_recognizer(&recognizer);
        release_retinaface_model(&detect_ctx);
        return 1;
    }
    const char f0 = pixel_format.size() > 0 ? pixel_format[0] : 'Y';
    const char f1 = pixel_format.size() > 1 ? pixel_format[1] : 'U';
    const char f2 = pixel_format.size() > 2 ? pixel_format[2] : 'Y';
    const char f3 = pixel_format.size() > 3 ? pixel_format[3] : 'V';
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc(f0, f1, f2, f3));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, camera_w);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, camera_h);
    cap.set(cv::CAP_PROP_FPS, fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, camera_buffers);

    const std::string window_name = "RV1126B Face Recognition";
    cv::namedWindow(window_name, cv::WINDOW_NORMAL);
    cv::resizeWindow(window_name, window_w, window_h);
    cv::moveWindow(window_name, window_x, window_y);

    retinaface_result last_result;
    std::memset(&last_result, 0, sizeof(last_result));
    std::vector<FeatureRecognitionResult> last_recognition;
    std::vector<StableRecognitionSlot> stable_recognition_slots;
    double last_infer_ms = 0.0;
    double display_fps = 0.0;
    long frame_count = 0;
    int frames_in_window = 0;
    auto fps_window_start = std::chrono::steady_clock::now();
    auto status_write_start = std::chrono::steady_clock::now();
    auto config_read_start = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto start_time = std::chrono::steady_clock::now();
    SecurityState security_state;
    int alert_interval_seconds = initial_alert_interval_seconds;

    std::printf("x11 recognition started: device=%s camera=%dx%d fps=%d window=%dx%d+%d+%d detect_interval=%d "
                "recog_ready=%d persons=%zu\n",
                device, camera_w, camera_h, fps, window_w, window_h, window_x, window_y, detect_interval,
                recognizer.ready ? 1 : 0, recognizer.persons.size());
    std::fflush(stdout);

    while (!g_stop) {
        cv::Mat bgr;
        cap >> bgr;
        if (bgr.empty()) {
            usleep(10000);
            continue;
        }

        if (frame_count % detect_interval == 0) {
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
            const auto infer_start = std::chrono::steady_clock::now();
            ret = inference_retinaface_model(&detect_ctx, &src_image, &result);
            const auto infer_end = std::chrono::steady_clock::now();
            if (ret == 0) {
                last_result = result;
                last_infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
            }
        }

        if (recognizer.ready &&
            (frame_count % face_recog_interval == 0 ||
             last_recognition.size() != static_cast<size_t>(std::max(0, last_result.count)))) {
            std::vector<FeatureRecognitionResult> raw = recognize_face_features(&recognizer, bgr, last_result);
            last_recognition =
                stabilize_recognitions(raw, last_result, &stable_recognition_slots, face_recog_stable_hits,
                                       face_recog_hold_frames);
        }

        const auto security_now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(security_now - config_read_start).count() >= 1.0) {
            alert_interval_seconds =
                read_int_config_value(security_config_file, "alert_interval_seconds", alert_interval_seconds, 3);
            config_read_start = security_now;
        }
        update_security_state(&security_state, last_result, last_recognition, security_now, stranger_alert_seconds,
                              alert_interval_seconds, stranger_entry_confirm_seconds, security_events_file);
        if (std::chrono::duration<double>(security_now - status_write_start).count() >= 1.0) {
            write_security_status(security_status_file, security_state, start_time, security_now,
                                  stranger_alert_seconds, alert_interval_seconds, stranger_entry_confirm_seconds);
            status_write_start = security_now;
        }

        draw_result(bgr, last_result, last_recognition);

        frame_count++;
        frames_in_window++;
        const auto now = std::chrono::steady_clock::now();
        const double window_sec = std::chrono::duration<double>(now - fps_window_start).count();
        if (window_sec >= 1.0) {
            display_fps = frames_in_window / window_sec;
            frames_in_window = 0;
            fps_window_start = now;
        }

        char overlay[160];
        std::snprintf(overlay, sizeof(overlay), "faces:%d ids:%zu npu:%.1fms", last_result.count,
                      recognizer.persons.size(), last_infer_ms);
        cv::putText(bgr, overlay, cv::Point(14, 30), cv::FONT_HERSHEY_SIMPLEX, 0.72, cv::Scalar(0, 0, 0), 4);
        cv::putText(bgr, overlay, cv::Point(14, 30), cv::FONT_HERSHEY_SIMPLEX, 0.72, cv::Scalar(0, 255, 255), 2);

        cv::Mat view = fill_window_crop_center(bgr, window_w, window_h);
        cv::imshow(window_name, view);
        cv::moveWindow(window_name, window_x, window_y);
        const int key = cv::waitKey(1) & 0xff;
        if (key == 27 || key == 'q' || key == 'Q') {
            break;
        }
    }

    cv::destroyWindow(window_name);
    release_face_feature_recognizer(&recognizer);
    release_retinaface_model(&detect_ctx);
    return 0;
}
