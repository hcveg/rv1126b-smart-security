# face_ai_npu_retinaface

Minimal RV1126B NPU RetinaFace + ArcFace demo.

Host keeps source, conversion tools, and build intermediates. The board only needs:

- `retinaface_npu`
- `retinaface_drm_screen`
- `register_face`
- `register_images`
- `recognize_camera`
- `run_once.sh`
- `model/RetinaFace_mobile320_rv1126b.rknn`
- `model/ArcFace_112_rv1126b.rknn`

Face detection uses RetinaFace. Face recognition uses an ArcFace RKNN feature model
and cosine similarity against samples in `/userdata/face_ai/faces/<name>/*.jpg`.
