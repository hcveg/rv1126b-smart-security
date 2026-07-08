CXX ?= g++
CC ?= gcc
TARGET := retinaface_npu
LIVE_TARGET := retinaface_live_server
SCREEN_TARGET := retinaface_screen
DRM_SCREEN_TARGET := retinaface_drm_screen
X11_RECOGNIZE_TARGET := retinaface_x11_recognize
REGISTER_TARGET := register_face
REGISTER_IMAGES_TARGET := register_images
RECOGNIZE_CAMERA_TARGET := recognize_camera

OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null || pkg-config --cflags opencv)
OPENCV_LIBS := $(shell pkg-config --libs opencv4 2>/dev/null || pkg-config --libs opencv)
LIBDRM_CFLAGS := $(shell pkg-config --cflags libdrm)
LIBDRM_LIBS := $(shell pkg-config --libs libdrm)

CPP_SRCS := src/main.cpp src/retinaface.cc src/image_utils.cpp
C_SRCS := src/file_utils.c
FACE_ALIGN_SRC := src/face_align.cpp
FACE_FEATURE_SRC := src/face_feature_recognizer.cpp

CXXFLAGS += -std=c++11 -O2 -Wall -Wextra -Iinclude $(OPENCV_CFLAGS) $(LIBDRM_CFLAGS)
CFLAGS += -O2 -Wall -Wextra -Iinclude
LDFLAGS += -Wl,--allow-shlib-undefined
LDLIBS += $(OPENCV_LIBS) -lrknnrt -ldl -lpthread -lm

.PHONY: all clean

all: $(TARGET) $(LIVE_TARGET) $(SCREEN_TARGET) $(DRM_SCREEN_TARGET) $(X11_RECOGNIZE_TARGET) $(REGISTER_TARGET) $(REGISTER_IMAGES_TARGET) $(RECOGNIZE_CAMERA_TARGET)

$(TARGET): $(CPP_SRCS) $(C_SRCS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(CPP_SRCS) $(C_SRCS) $(LDLIBS)

$(LIVE_TARGET): src/live_server.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/live_server.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(LDLIBS)

$(SCREEN_TARGET): src/screen_main.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/screen_main.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(LDLIBS)

$(DRM_SCREEN_TARGET): src/drm_screen_main.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(FACE_FEATURE_SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/drm_screen_main.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(FACE_FEATURE_SRC) $(LDLIBS) $(LIBDRM_LIBS)

$(X11_RECOGNIZE_TARGET): src/x11_recognize_main.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(FACE_FEATURE_SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/x11_recognize_main.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(FACE_FEATURE_SRC) $(LDLIBS)

$(REGISTER_TARGET): src/register_face.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/register_face.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(LDLIBS)

$(REGISTER_IMAGES_TARGET): src/register_images.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/register_images.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(LDLIBS)

$(RECOGNIZE_CAMERA_TARGET): src/recognize_camera.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(FACE_FEATURE_SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ src/recognize_camera.cpp src/retinaface.cc src/image_utils.cpp src/file_utils.c $(FACE_ALIGN_SRC) $(FACE_FEATURE_SRC) $(LDLIBS)

clean:
	rm -f $(TARGET) $(LIVE_TARGET) $(SCREEN_TARGET) $(DRM_SCREEN_TARGET) $(X11_RECOGNIZE_TARGET) $(REGISTER_TARGET) $(REGISTER_IMAGES_TARGET) $(RECOGNIZE_CAMERA_TARGET)
