#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include <linux/input.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "face_feature_recognizer.h"
#include "retinaface.h"

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int) {
    g_stop = 1;
}

struct DrmBuffer {
    uint32_t fb_id = 0;
    uint32_t handle = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    uint8_t* map = nullptr;
};

struct DrmState {
    int fd = -1;
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    drmModeModeInfo mode;
    drmModeCrtc* old_crtc = nullptr;
    DrmBuffer buffers[2];
};

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

struct CameraSettings {
    int target_fps = 30;
    int detect_interval = 2;
    int exposure = 120;
    int gain = 40;
    int brightness = 5;
    int contrast = 42;
    int saturation = 70;
    int gamma = 110;
    int white_balance_temp = 5000;
};

enum class ControlId {
    Fps,
    DetectInterval,
    Exposure,
    Gain,
    Brightness,
    Contrast,
    Saturation,
    Gamma,
    WhiteBalanceTemp,
};

struct ControlSpec {
    ControlId id;
    const char* label;
    int min_value;
    int max_value;
    int step;
};

enum class ButtonAction {
    Adjust,
    ToggleUi,
    Save,
    Reset,
};

struct UiButton {
    cv::Rect rect;
    ButtonAction action;
    ControlId id;
    int delta = 0;

    UiButton() {}
    UiButton(const cv::Rect& rect_, ButtonAction action_, ControlId id_, int delta_)
        : rect(rect_), action(action_), id(id_), delta(delta_) {}
};

struct TouchState {
    int fd = -1;
    int raw_x = 0;
    int raw_y = 0;
    int min_x = 0;
    int max_x = 1023;
    int min_y = 0;
    int max_y = 599;
    bool touching = false;
    bool was_touching = false;
    bool have_x = false;
    bool have_y = false;
    int screen_w = 1024;
    int screen_h = 600;
};

static int& setting_value(CameraSettings* settings, ControlId id) {
    switch (id) {
    case ControlId::Fps:
        return settings->target_fps;
    case ControlId::DetectInterval:
        return settings->detect_interval;
    case ControlId::Exposure:
        return settings->exposure;
    case ControlId::Gain:
        return settings->gain;
    case ControlId::Brightness:
        return settings->brightness;
    case ControlId::Contrast:
        return settings->contrast;
    case ControlId::Saturation:
        return settings->saturation;
    case ControlId::Gamma:
        return settings->gamma;
    case ControlId::WhiteBalanceTemp:
        return settings->white_balance_temp;
    }
    return settings->target_fps;
}

static CameraSettings default_camera_settings() {
    CameraSettings settings;
    return settings;
}

static std::vector<ControlSpec> ui_controls() {
    std::vector<ControlSpec> controls;
    controls.push_back({ControlId::Fps, "FPS", 5, 30, 5});
    controls.push_back({ControlId::DetectInterval, "Detect", 1, 10, 1});
    controls.push_back({ControlId::Exposure, "Exposure", 20, 300, 10});
    controls.push_back({ControlId::Gain, "Gain", 0, 63, 4});
    controls.push_back({ControlId::Saturation, "Saturation", 0, 128, 5});
    controls.push_back({ControlId::WhiteBalanceTemp, "WB Temp", 2800, 6500, 100});
    controls.push_back({ControlId::Brightness, "Brightness", -128, 127, 5});
    controls.push_back({ControlId::Contrast, "Contrast", 0, 63, 2});
    controls.push_back({ControlId::Gamma, "Gamma", 1, 500, 10});
    return controls;
}

static bool safe_video_device_path(const char* device) {
    const std::string path = device != nullptr ? device : "";
    if (path.find("/dev/video") != 0) {
        return false;
    }
    for (char ch : path) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '/' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static void apply_camera_settings(const CameraSettings& settings, cv::VideoCapture* cap, const char* device) {
    (void)cap;
    if (!safe_video_device_path(device)) {
        return;
    }

    char command[768];
    std::snprintf(command, sizeof(command),
                  "v4l2-ctl -d %s --set-ctrl=auto_exposure=1,exposure_time_absolute=%d,gain=%d,"
                  "brightness=%d,contrast=%d,saturation=%d,gamma=%d,white_balance_automatic=0,"
                  "white_balance_temperature=%d >/dev/null 2>&1",
                  device, settings.exposure, settings.gain, settings.brightness, settings.contrast,
                  settings.saturation, settings.gamma, settings.white_balance_temp);
    std::system(command);
}

static void save_camera_settings(const CameraSettings& settings) {
    const char* settings_file = std::getenv("SETTINGS_FILE");
    if (settings_file == nullptr || settings_file[0] == '\0') {
        return;
    }

    std::ofstream out(settings_file, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "save settings failed: %s\n", settings_file);
        return;
    }
    out << "FPS=" << settings.target_fps << "\n";
    out << "DETECT_INTERVAL=" << settings.detect_interval << "\n";
    out << "EXPOSURE=" << settings.exposure << "\n";
    out << "GAIN=" << settings.gain << "\n";
    out << "BRIGHTNESS=" << settings.brightness << "\n";
    out << "CONTRAST=" << settings.contrast << "\n";
    out << "SATURATION=" << settings.saturation << "\n";
    out << "GAMMA=" << settings.gamma << "\n";
    out << "WHITE_BALANCE_TEMP=" << settings.white_balance_temp << "\n";
    out << "WHITE_BALANCE_AUTO=0\n";
}

static bool query_abs_axis(int fd, int code, int* min_value, int* max_value) {
    input_absinfo info;
    std::memset(&info, 0, sizeof(info));
    if (ioctl(fd, EVIOCGABS(code), &info) != 0) {
        return false;
    }
    *min_value = info.minimum;
    *max_value = info.maximum;
    return true;
}

static bool init_touch(TouchState* touch, int screen_w, int screen_h) {
    const char* input_dev = std::getenv("INPUT_DEV");
    if (input_dev == nullptr || input_dev[0] == '\0') {
        input_dev = "/dev/input/event2";
    }

    touch->fd = open(input_dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    touch->screen_w = screen_w;
    touch->screen_h = screen_h;
    if (touch->fd < 0) {
        std::fprintf(stderr, "touch input disabled: cannot open %s: %s\n", input_dev, strerror(errno));
        return false;
    }

    bool has_x = query_abs_axis(touch->fd, ABS_MT_POSITION_X, &touch->min_x, &touch->max_x);
    bool has_y = query_abs_axis(touch->fd, ABS_MT_POSITION_Y, &touch->min_y, &touch->max_y);
    if (!has_x) {
        query_abs_axis(touch->fd, ABS_X, &touch->min_x, &touch->max_x);
    }
    if (!has_y) {
        query_abs_axis(touch->fd, ABS_Y, &touch->min_y, &touch->max_y);
    }

    std::printf("touch input enabled: %s x=%d..%d y=%d..%d\n", input_dev, touch->min_x, touch->max_x, touch->min_y,
                touch->max_y);
    std::fflush(stdout);
    return true;
}

static int map_axis(int raw, int min_value, int max_value, int size) {
    if (max_value <= min_value) {
        return clamp_int(raw, 0, size - 1);
    }
    const int mapped = static_cast<int>((static_cast<long long>(raw - min_value) * (size - 1)) / (max_value - min_value));
    return clamp_int(mapped, 0, size - 1);
}

static bool poll_touch(TouchState* touch, int* x, int* y) {
    if (touch->fd < 0) {
        return false;
    }

    input_event event;
    bool released = false;
    while (true) {
        ssize_t bytes = read(touch->fd, &event, sizeof(event));
        if (bytes < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (bytes != sizeof(event)) {
            break;
        }

        if (event.type == EV_ABS) {
            if (event.code == ABS_X || event.code == ABS_MT_POSITION_X) {
                touch->raw_x = event.value;
                touch->have_x = true;
            } else if (event.code == ABS_Y || event.code == ABS_MT_POSITION_Y) {
                touch->raw_y = event.value;
                touch->have_y = true;
            } else if (event.code == ABS_MT_TRACKING_ID) {
                if (event.value >= 0) {
                    touch->touching = true;
                } else {
                    released = touch->touching;
                    touch->touching = false;
                }
            }
        } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
            if (event.value != 0) {
                touch->touching = true;
            } else {
                released = touch->touching;
                touch->touching = false;
            }
        } else if (event.type == EV_SYN) {
            if ((released || (!touch->touching && touch->was_touching)) && touch->have_x && touch->have_y) {
                int mapped_x = map_axis(touch->raw_x, touch->min_x, touch->max_x, touch->screen_w);
                int mapped_y = map_axis(touch->raw_y, touch->min_y, touch->max_y, touch->screen_h);
                if (env_int("TOUCH_SWAP_XY", 0) != 0) {
                    std::swap(mapped_x, mapped_y);
                }
                if (env_int("TOUCH_INVERT_X", 0) != 0) {
                    mapped_x = touch->screen_w - 1 - mapped_x;
                }
                if (env_int("TOUCH_INVERT_Y", 0) != 0) {
                    mapped_y = touch->screen_h - 1 - mapped_y;
                }
                *x = clamp_int(mapped_x, 0, touch->screen_w - 1);
                *y = clamp_int(mapped_y, 0, touch->screen_h - 1);
                touch->was_touching = false;
                released = false;
                return true;
            }
            touch->was_touching = touch->touching;
            released = false;
        }
    }
    return false;
}

static void draw_centered_text(cv::Mat& image, const cv::Rect& rect, const std::string& text, double scale,
                               const cv::Scalar& color, int thickness) {
    int baseline = 0;
    cv::Size size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, scale, thickness, &baseline);
    cv::Point pos(rect.x + (rect.width - size.width) / 2, rect.y + (rect.height + size.height) / 2);
    cv::putText(image, text, pos, cv::FONT_HERSHEY_SIMPLEX, scale, color, thickness);
}

static void draw_button(cv::Mat& image, const cv::Rect& rect, const std::string& text, const cv::Scalar& fill) {
    cv::rectangle(image, rect, fill, cv::FILLED);
    cv::rectangle(image, rect, cv::Scalar(210, 210, 210), 1);
    draw_centered_text(image, rect, text, 0.62, cv::Scalar(255, 255, 255), 2);
}

static std::vector<UiButton> draw_ui(cv::Mat& screen, const CameraSettings& settings, bool ui_visible) {
    std::vector<UiButton> buttons;
    if (!ui_visible) {
        cv::Rect show_rect(screen.cols - 82, 10, 72, 42);
        draw_button(screen, show_rect, "UI", cv::Scalar(70, 95, 120));
        buttons.push_back(UiButton(show_rect, ButtonAction::ToggleUi, ControlId::Fps, 0));
        return buttons;
    }

    const int panel_w = std::min(360, screen.cols);
    const int panel_x = screen.cols - panel_w;
    cv::Rect panel(panel_x, 0, panel_w, screen.rows);
    cv::rectangle(screen, panel, cv::Scalar(26, 30, 34), cv::FILLED);
    cv::rectangle(screen, panel, cv::Scalar(90, 100, 110), 1);

    cv::putText(screen, "Face Settings", cv::Point(panel_x + 16, 32), cv::FONT_HERSHEY_SIMPLEX, 0.72,
                cv::Scalar(255, 255, 255), 2);
    cv::putText(screen, "tap +/- then Save", cv::Point(panel_x + 16, 56), cv::FONT_HERSHEY_SIMPLEX, 0.48,
                cv::Scalar(175, 190, 200), 1);

    const std::vector<ControlSpec> controls = ui_controls();
    const int row_h = 47;
    const int start_y = 72;
    for (size_t i = 0; i < controls.size(); ++i) {
        const ControlSpec& control = controls[i];
        const int y = start_y + static_cast<int>(i) * row_h;
        int value = setting_value(const_cast<CameraSettings*>(&settings), control.id);

        char value_text[32];
        std::snprintf(value_text, sizeof(value_text), "%d", value);
        cv::putText(screen, control.label, cv::Point(panel_x + 14, y + 19), cv::FONT_HERSHEY_SIMPLEX, 0.50,
                    cv::Scalar(220, 225, 230), 1);
        cv::putText(screen, value_text, cv::Point(panel_x + 142, y + 19), cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    cv::Scalar(255, 230, 120), 2);

        cv::Rect minus_rect(panel_x + panel_w - 112, y - 8, 46, 34);
        cv::Rect plus_rect(panel_x + panel_w - 56, y - 8, 46, 34);
        draw_button(screen, minus_rect, "-", cv::Scalar(72, 78, 86));
        draw_button(screen, plus_rect, "+", cv::Scalar(70, 105, 88));
        buttons.push_back(UiButton(minus_rect, ButtonAction::Adjust, control.id, -control.step));
        buttons.push_back(UiButton(plus_rect, ButtonAction::Adjust, control.id, control.step));
    }

    const int bottom_y = screen.rows - 52;
    cv::Rect save_rect(panel_x + 14, bottom_y, 96, 40);
    cv::Rect reset_rect(panel_x + 124, bottom_y, 96, 40);
    cv::Rect hide_rect(panel_x + panel_w - 110, bottom_y, 96, 40);
    draw_button(screen, save_rect, "Save", cv::Scalar(52, 116, 86));
    draw_button(screen, reset_rect, "Reset", cv::Scalar(96, 84, 64));
    draw_button(screen, hide_rect, "Hide", cv::Scalar(70, 95, 120));
    buttons.push_back(UiButton(save_rect, ButtonAction::Save, ControlId::Fps, 0));
    buttons.push_back(UiButton(reset_rect, ButtonAction::Reset, ControlId::Fps, 0));
    buttons.push_back(UiButton(hide_rect, ButtonAction::ToggleUi, ControlId::Fps, 0));
    return buttons;
}

static bool handle_ui_tap(int x, int y, const std::vector<UiButton>& buttons, CameraSettings* settings,
                          cv::VideoCapture* cap, const char* device, bool* ui_visible) {
    for (const UiButton& button : buttons) {
        if (!button.rect.contains(cv::Point(x, y))) {
            continue;
        }

        if (button.action == ButtonAction::ToggleUi) {
            *ui_visible = !*ui_visible;
            return true;
        }
        if (button.action == ButtonAction::Save) {
            save_camera_settings(*settings);
            std::printf("settings saved\n");
            std::fflush(stdout);
            return true;
        }
        if (button.action == ButtonAction::Reset) {
            *settings = default_camera_settings();
            apply_camera_settings(*settings, cap, device);
            std::printf("settings reset\n");
            std::fflush(stdout);
            return true;
        }

        const std::vector<ControlSpec> controls = ui_controls();
        for (const ControlSpec& control : controls) {
            if (control.id != button.id) {
                continue;
            }
            int& value = setting_value(settings, button.id);
            value = clamp_int(value + button.delta, control.min_value, control.max_value);
            apply_camera_settings(*settings, cap, device);
            std::printf("setting changed: %s=%d\n", control.label, value);
            std::fflush(stdout);
            return true;
        }
    }
    return false;
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

        cv::rectangle(bgr, cv::Rect(left, top, width, height), cv::Scalar(0, 255, 0), 2);

        char label_text[96];
        if (i < static_cast<int>(recognized.size()) && recognized[i].known) {
            std::snprintf(label_text, sizeof(label_text), "%s %.2f", recognized[i].name.c_str(), recognized[i].score);
        } else if (i < static_cast<int>(recognized.size()) && recognized[i].score > 0.0f) {
            std::snprintf(label_text, sizeof(label_text), "Unknown %.2f", recognized[i].score);
        } else {
            std::snprintf(label_text, sizeof(label_text), "%.2f", face.score);
        }
        const cv::Scalar label_color =
            (i < static_cast<int>(recognized.size()) && recognized[i].known) ? cv::Scalar(0, 255, 255)
                                                                             : cv::Scalar(0, 0, 255);
        cv::putText(bgr, label_text, cv::Point(left, std::max(18, top - 4)), cv::FONT_HERSHEY_SIMPLEX, 0.55,
                    label_color, 2);

        for (int j = 0; j < 5; ++j) {
            const int x = clamp_int(face.ponit[j].x, 0, bgr.cols - 1);
            const int y = clamp_int(face.ponit[j].y, 0, bgr.rows - 1);
            cv::circle(bgr, cv::Point(x, y), 2, cv::Scalar(0, 165, 255), 2);
        }
    }
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
        const int max_center_dist_sq = std::max(40 * 40, (std::max(box.width, old_box.width) *
                                                          std::max(box.width, old_box.width) +
                                                          std::max(box.height, old_box.height) *
                                                          std::max(box.height, old_box.height)) /
                                                             4);
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

static bool create_drm_buffer(DrmState* drm, int width, int height, DrmBuffer* out) {
    struct drm_mode_create_dumb create;
    std::memset(&create, 0, sizeof(create));
    create.width = width;
    create.height = height;
    create.bpp = 32;

    if (drmIoctl(drm->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        return false;
    }

    out->handle = create.handle;
    out->pitch = create.pitch;
    out->size = create.size;

    uint32_t handles[4] = {out->handle, 0, 0, 0};
    uint32_t pitches[4] = {out->pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(drm->fd, width, height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &out->fb_id, 0) != 0) {
        perror("drmModeAddFB2");
        return false;
    }

    struct drm_mode_map_dumb map;
    std::memset(&map, 0, sizeof(map));
    map.handle = out->handle;
    if (drmIoctl(drm->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return false;
    }

    out->map = static_cast<uint8_t*>(mmap(nullptr, out->size, PROT_READ | PROT_WRITE, MAP_SHARED, drm->fd, map.offset));
    if (out->map == MAP_FAILED) {
        out->map = nullptr;
        perror("mmap drm buffer");
        return false;
    }

    std::memset(out->map, 0, out->size);
    return true;
}

static void destroy_drm_buffer(DrmState* drm, DrmBuffer* buffer) {
    if (buffer->map != nullptr) {
        munmap(buffer->map, buffer->size);
        buffer->map = nullptr;
    }
    if (buffer->fb_id != 0) {
        drmModeRmFB(drm->fd, buffer->fb_id);
        buffer->fb_id = 0;
    }
    if (buffer->handle != 0) {
        struct drm_gem_close gem_close;
        std::memset(&gem_close, 0, sizeof(gem_close));
        gem_close.handle = buffer->handle;
        drmIoctl(drm->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
        buffer->handle = 0;
    }
}

static drmModeCrtc* find_crtc_for_connector(int fd, drmModeRes* resources, drmModeConnector* connector) {
    if (connector->encoder_id != 0) {
        drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoder_id);
        if (encoder != nullptr) {
            if (encoder->crtc_id != 0) {
                uint32_t crtc_id = encoder->crtc_id;
                drmModeFreeEncoder(encoder);
                return drmModeGetCrtc(fd, crtc_id);
            }
            drmModeFreeEncoder(encoder);
        }
    }

    for (int i = 0; i < connector->count_encoders; ++i) {
        drmModeEncoder* encoder = drmModeGetEncoder(fd, connector->encoders[i]);
        if (encoder == nullptr) {
            continue;
        }
        for (int j = 0; j < resources->count_crtcs; ++j) {
            if (encoder->possible_crtcs & (1 << j)) {
                uint32_t crtc_id = resources->crtcs[j];
                drmModeFreeEncoder(encoder);
                return drmModeGetCrtc(fd, crtc_id);
            }
        }
        drmModeFreeEncoder(encoder);
    }
    return nullptr;
}

static bool init_drm(DrmState* drm) {
    drm->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm->fd < 0) {
        perror("open /dev/dri/card0");
        return false;
    }

    drmModeRes* resources = drmModeGetResources(drm->fd);
    if (resources == nullptr) {
        perror("drmModeGetResources");
        return false;
    }

    drmModeConnector* connector = nullptr;
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnector* candidate = drmModeGetConnector(drm->fd, resources->connectors[i]);
        if (candidate == nullptr) {
            continue;
        }
        if (candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes > 0) {
            connector = candidate;
            break;
        }
        drmModeFreeConnector(candidate);
    }
    if (connector == nullptr) {
        std::fprintf(stderr, "no connected DRM connector found\n");
        drmModeFreeResources(resources);
        return false;
    }

    drmModeCrtc* crtc = find_crtc_for_connector(drm->fd, resources, connector);
    if (crtc == nullptr) {
        std::fprintf(stderr, "no usable DRM CRTC found\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        return false;
    }

    drm->connector_id = connector->connector_id;
    drm->crtc_id = crtc->crtc_id;
    drm->old_crtc = crtc;
    drm->mode = connector->modes[0];
    for (int i = 0; i < connector->count_modes; ++i) {
        if (connector->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            drm->mode = connector->modes[i];
            break;
        }
    }

    int width = drm->mode.hdisplay;
    int height = drm->mode.vdisplay;
    bool ok = create_drm_buffer(drm, width, height, &drm->buffers[0]) &&
              create_drm_buffer(drm, width, height, &drm->buffers[1]);

    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    return ok;
}

static void restore_drm(DrmState* drm) {
    if (drm->fd >= 0 && drm->old_crtc != nullptr) {
        drmModeSetCrtc(drm->fd, drm->old_crtc->crtc_id, drm->old_crtc->buffer_id, drm->old_crtc->x, drm->old_crtc->y,
                       &drm->connector_id, 1, &drm->old_crtc->mode);
    }
    destroy_drm_buffer(drm, &drm->buffers[0]);
    destroy_drm_buffer(drm, &drm->buffers[1]);
    if (drm->old_crtc != nullptr) {
        drmModeFreeCrtc(drm->old_crtc);
        drm->old_crtc = nullptr;
    }
    if (drm->fd >= 0) {
        close(drm->fd);
        drm->fd = -1;
    }
}

static void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void* data) {
    int* waiting = static_cast<int*>(data);
    *waiting = 0;
}

static bool wait_for_page_flip(DrmState* drm, int* waiting) {
    drmEventContext event_context;
    std::memset(&event_context, 0, sizeof(event_context));
    event_context.version = DRM_EVENT_CONTEXT_VERSION;
    event_context.page_flip_handler = page_flip_handler;

    while (*waiting != 0 && !g_stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(drm->fd, &fds);

        int ret = select(drm->fd + 1, &fds, nullptr, nullptr, nullptr);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select drm event");
            return false;
        }
        if (FD_ISSET(drm->fd, &fds) && drmHandleEvent(drm->fd, &event_context) != 0) {
            perror("drmHandleEvent");
            return false;
        }
    }
    return true;
}

static void copy_bgr_to_xrgb_center(const cv::Mat& bgr, DrmState* drm, DrmBuffer* buffer) {
    const int screen_w = drm->mode.hdisplay;
    const int screen_h = drm->mode.vdisplay;
    const int copy_w = std::min(screen_w, bgr.cols);
    const int copy_h = std::min(screen_h, bgr.rows);
    const int x0 = (screen_w - copy_w) / 2;
    const int y0 = (screen_h - copy_h) / 2;

    std::memset(buffer->map, 0, buffer->size);
    for (int y = 0; y < copy_h; ++y) {
        const uint8_t* src = bgr.ptr<uint8_t>(y);
        uint32_t* dst = reinterpret_cast<uint32_t*>(buffer->map + (y0 + y) * buffer->pitch) + x0;
        for (int x = 0; x < copy_w; ++x) {
            const uint8_t b = src[x * 3 + 0];
            const uint8_t g = src[x * 3 + 1];
            const uint8_t r = src[x * 3 + 2];
            dst[x] = (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3 || argc > 7) {
        std::fprintf(stderr, "usage: %s <model.rknn> <video_device> [width] [height] [fps] [detect_interval]\n",
                     argv[0]);
        return 2;
    }

    const char* model_path = argv[1];
    const char* device = argv[2];
    const int camera_w = argc >= 4 ? std::atoi(argv[3]) : 640;
    const int camera_h = argc >= 5 ? std::atoi(argv[4]) : 480;
    const int initial_fps = argc >= 6 ? std::atoi(argv[5]) : 30;
    const int initial_detect_interval = std::max(1, argc >= 7 ? std::atoi(argv[6]) : 2);
    CameraSettings settings = default_camera_settings();
    settings.target_fps = env_int("FPS", initial_fps);
    settings.detect_interval = env_int("DETECT_INTERVAL", initial_detect_interval);
    settings.exposure = env_int("EXPOSURE", settings.exposure);
    settings.gain = env_int("GAIN", settings.gain);
    settings.brightness = env_int("BRIGHTNESS", settings.brightness);
    settings.contrast = env_int("CONTRAST", settings.contrast);
    settings.saturation = env_int("SATURATION", settings.saturation);
    settings.gamma = env_int("GAMMA", settings.gamma);
    settings.white_balance_temp = env_int("WHITE_BALANCE_TEMP", settings.white_balance_temp);
    const char* pixel_format_env = std::getenv("PIXEL_FORMAT");
    std::string pixel_format = pixel_format_env != nullptr ? pixel_format_env : "YUYV";
    std::transform(pixel_format.begin(), pixel_format.end(), pixel_format.begin(), ::toupper);
    if (pixel_format.size() != 4) {
        pixel_format = "YUYV";
    }
    const char* camera_buffers_env = std::getenv("CAMERA_BUFFERS");
    const int camera_buffers = std::max(1, camera_buffers_env != nullptr ? std::atoi(camera_buffers_env) : 4);
    const int camera_capture_fps = env_int("CAMERA_FPS", 30);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    DrmState drm;
    if (!init_drm(&drm)) {
        restore_drm(&drm);
        return 1;
    }

    rknn_app_context_t app_ctx;
    std::memset(&app_ctx, 0, sizeof(app_ctx));
    int ret = init_retinaface_model(model_path, &app_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "init_retinaface_model failed: ret=%d model=%s\n", ret, model_path);
        restore_drm(&drm);
        return 1;
    }

    cv::VideoCapture cap(device, cv::CAP_V4L2);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "open camera failed: %s\n", device);
        release_retinaface_model(&app_ctx);
        restore_drm(&drm);
        return 1;
    }
    cap.set(cv::CAP_PROP_FOURCC,
            cv::VideoWriter::fourcc(pixel_format[0], pixel_format[1], pixel_format[2], pixel_format[3]));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, camera_w);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, camera_h);
    cap.set(cv::CAP_PROP_FPS, camera_capture_fps);
    cap.set(cv::CAP_PROP_BUFFERSIZE, camera_buffers);
    apply_camera_settings(settings, &cap, device);

    retinaface_result last_result;
    std::memset(&last_result, 0, sizeof(last_result));
    double last_infer_ms = 0.0;
    double display_fps = 0.0;
    if (drmModeSetCrtc(drm.fd, drm.crtc_id, drm.buffers[0].fb_id, 0, 0, &drm.connector_id, 1, &drm.mode) != 0) {
        perror("drmModeSetCrtc initial");
        release_retinaface_model(&app_ctx);
        restore_drm(&drm);
        return 1;
    }

    int buffer_index = 1;
    long frame_count = 0;
    int frames_in_window = 0;
    int fps_report_count = 0;
    double cap_ms_sum = 0.0;
    double draw_ms_sum = 0.0;
    double copy_ms_sum = 0.0;
    double flip_ms_sum = 0.0;
    double infer_ms_sum = 0.0;
    int timing_frames = 0;
    int timing_infer_frames = 0;
    auto fps_window_start = std::chrono::steady_clock::now();
    cv::Mat screen_bgr(drm.mode.vdisplay, drm.mode.hdisplay, CV_8UC3);
    TouchState touch;
    init_touch(&touch, drm.mode.hdisplay, drm.mode.vdisplay);
    bool ui_visible = env_int("UI_VISIBLE", 1) != 0;
    std::vector<UiButton> ui_buttons;
    FaceFeatureRecognizer face_recognizer;
    const char* face_db_env = std::getenv("FACE_DB_DIR");
    const std::string face_db_dir = face_db_env != nullptr && face_db_env[0] != '\0' ? face_db_env : "/userdata/face_ai/faces";
    const char* feature_model_env = std::getenv("FACE_FEATURE_MODEL");
    const std::string feature_model =
        feature_model_env != nullptr && feature_model_env[0] != '\0' ? feature_model_env
                                                                      : "/userdata/face_ai/model/ArcFace_112_rv1126b.rknn";
    const float face_threshold = env_float("FACE_RECOG_THRESHOLD", face_recognizer.threshold);
    face_recognizer.margin = env_float("FACE_RECOG_MARGIN", face_recognizer.margin);
    face_recognizer.centroid_threshold = env_float("FACE_RECOG_CENTROID_THRESHOLD", face_recognizer.centroid_threshold);
    face_recognizer.sample_vote_threshold =
        env_float("FACE_RECOG_SAMPLE_VOTE_THRESHOLD", face_recognizer.sample_vote_threshold);
    face_recognizer.min_sample_votes = std::max(1, env_int("FACE_RECOG_MIN_SAMPLE_VOTES",
                                                           face_recognizer.min_sample_votes));
    face_recognizer.min_face_size = env_int("FACE_RECOG_MIN_SIZE", face_recognizer.min_face_size);
    init_face_feature_recognizer(&face_recognizer, feature_model.c_str(), face_db_dir.c_str(), face_threshold);
    const int face_recog_interval = std::max(1, env_int("FACE_RECOG_INTERVAL", 2));
    const int face_recog_stable_hits = std::max(1, env_int("FACE_RECOG_STABLE_HITS", 3));
    const int face_recog_hold_frames = std::max(0, env_int("FACE_RECOG_HOLD_FRAMES", 6));
    std::vector<FeatureRecognitionResult> last_recognition;
    std::vector<StableRecognitionSlot> stable_recognition_slots;

    std::printf("drm display started: device=%s camera=%dx%d capture_fps=%d target_fps=%d format=%s buffers=%d drm=%dx%d detect_interval=%d\n",
                device, camera_w, camera_h, camera_capture_fps, settings.target_fps, pixel_format.c_str(),
                camera_buffers, drm.mode.hdisplay, drm.mode.vdisplay, settings.detect_interval);
    std::fflush(stdout);

    while (!g_stop) {
        const auto frame_start = std::chrono::steady_clock::now();
        int tap_x = 0;
        int tap_y = 0;
        if (poll_touch(&touch, &tap_x, &tap_y)) {
            handle_ui_tap(tap_x, tap_y, ui_buttons, &settings, &cap, device, &ui_visible);
        }

        cv::Mat bgr;
        const auto cap_start = std::chrono::steady_clock::now();
        cap >> bgr;
        const auto cap_end = std::chrono::steady_clock::now();
        if (bgr.empty()) {
            usleep(1000);
            continue;
        }
        cap_ms_sum += std::chrono::duration<double, std::milli>(cap_end - cap_start).count();

        if (frame_count % settings.detect_interval == 0) {
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
            ret = inference_retinaface_model(&app_ctx, &src_image, &result);
            const auto infer_end = std::chrono::steady_clock::now();
            if (ret == 0) {
                last_result = result;
                last_infer_ms = std::chrono::duration<double, std::milli>(infer_end - infer_start).count();
                infer_ms_sum += last_infer_ms;
                timing_infer_frames++;
            }
        }

        if (face_recognizer.ready &&
            (frame_count % face_recog_interval == 0 ||
             last_recognition.size() != static_cast<size_t>(std::max(0, last_result.count)))) {
            std::vector<FeatureRecognitionResult> raw_recognition =
                recognize_face_features(&face_recognizer, bgr, last_result);
            last_recognition = stabilize_recognitions(raw_recognition, last_result, &stable_recognition_slots,
                                                      face_recog_stable_hits, face_recog_hold_frames);
        }

        const auto draw_start = std::chrono::steady_clock::now();
        draw_result(bgr, last_result, last_recognition);

        frame_count++;
        frames_in_window++;
        const auto now = std::chrono::steady_clock::now();
        const double window_sec = std::chrono::duration<double>(now - fps_window_start).count();
        if (window_sec >= 1.0) {
            display_fps = frames_in_window / window_sec;
            frames_in_window = 0;
            fps_window_start = now;
            fps_report_count++;
            if (fps_report_count % 5 == 0) {
                std::printf("display_fps=%.1f faces=%d npu=%.1fms detect_interval=%d\n", display_fps,
                            last_result.count, last_infer_ms, settings.detect_interval);
                std::fflush(stdout);
            }
        }

        char overlay[128];
        std::snprintf(overlay, sizeof(overlay), "faces:%d ids:%zu npu:%.1fms det:1/%d", last_result.count,
                      face_recognizer.persons.size(), last_infer_ms, settings.detect_interval);
        cv::putText(bgr, overlay, cv::Point(16, 32), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 0), 4);
        cv::putText(bgr, overlay, cv::Point(16, 32), cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 255), 2);

        screen_bgr.setTo(cv::Scalar(0, 0, 0));
        const int paste_w = std::min(screen_bgr.cols, bgr.cols);
        const int paste_h = std::min(screen_bgr.rows, bgr.rows);
        const int paste_y = std::max(0, (screen_bgr.rows - paste_h) / 2);
        cv::Rect src_rect(0, 0, paste_w, paste_h);
        cv::Rect dst_rect(0, paste_y, paste_w, paste_h);
        bgr(src_rect).copyTo(screen_bgr(dst_rect));
        ui_buttons = draw_ui(screen_bgr, settings, ui_visible);
        const auto draw_end = std::chrono::steady_clock::now();
        draw_ms_sum += std::chrono::duration<double, std::milli>(draw_end - draw_start).count();

        DrmBuffer* buffer = &drm.buffers[buffer_index];
        const auto copy_start = std::chrono::steady_clock::now();
        copy_bgr_to_xrgb_center(screen_bgr, &drm, buffer);
        const auto copy_end = std::chrono::steady_clock::now();
        copy_ms_sum += std::chrono::duration<double, std::milli>(copy_end - copy_start).count();

        const auto flip_start = std::chrono::steady_clock::now();
        int waiting_for_flip = 1;
        if (drmModePageFlip(drm.fd, drm.crtc_id, buffer->fb_id, DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip) != 0) {
            perror("drmModePageFlip");
            break;
        }
        if (!wait_for_page_flip(&drm, &waiting_for_flip)) {
            break;
        }
        const auto flip_end = std::chrono::steady_clock::now();
        flip_ms_sum += std::chrono::duration<double, std::milli>(flip_end - flip_start).count();
        timing_frames++;
        buffer_index = 1 - buffer_index;

        if (fps_report_count > 0 && fps_report_count % 5 == 0 && timing_frames > 0) {
            const double avg_cap = cap_ms_sum / timing_frames;
            const double avg_draw = draw_ms_sum / timing_frames;
            const double avg_copy = copy_ms_sum / timing_frames;
            const double avg_flip = flip_ms_sum / timing_frames;
            const double avg_infer = timing_infer_frames > 0 ? infer_ms_sum / timing_infer_frames : 0.0;
            std::printf("timing_ms cap=%.1f draw=%.1f copy=%.1f flip=%.1f infer_avg=%.1f infer_frames=%d\n",
                        avg_cap, avg_draw, avg_copy, avg_flip, avg_infer, timing_infer_frames);
            std::fflush(stdout);
            cap_ms_sum = 0.0;
            draw_ms_sum = 0.0;
            copy_ms_sum = 0.0;
            flip_ms_sum = 0.0;
            infer_ms_sum = 0.0;
            timing_frames = 0;
            timing_infer_frames = 0;
            fps_report_count = 1;
        }

        const int target_fps = clamp_int(settings.target_fps, 1, 60);
        const double target_ms = 1000.0 / target_fps;
        const auto frame_end = std::chrono::steady_clock::now();
        const double elapsed_ms = std::chrono::duration<double, std::milli>(frame_end - frame_start).count();
        if (elapsed_ms < target_ms) {
            usleep(static_cast<useconds_t>((target_ms - elapsed_ms) * 1000.0));
        }
    }

    release_face_feature_recognizer(&face_recognizer);
    release_retinaface_model(&app_ctx);
    restore_drm(&drm);
    return 0;
}
