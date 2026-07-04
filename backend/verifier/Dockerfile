# Mode 3 cloud verifier — ArcFace (InsightFace + ONNX Runtime) on CPU.
# Build context is backend/verifier/. Paths below are relative to it.
FROM python:3.11-slim

WORKDIR /srv

# System libs InsightFace/OpenCV/onnxruntime need at runtime:
#   libgl1, libglib2.0-0 : OpenCV image ops
#   build-essential      : some wheels compile native code on install
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgl1 libglib2.0-0 build-essential \
 && rm -rf /var/lib/apt/lists/*

# Deps first for layer caching.
COPY app/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Pre-download the InsightFace model at BUILD time so the container doesn't have
# to fetch ~300MB on first request (which would make the first /verify slow and
# could fail if the box has no outbound internet). Baked into the image.
ARG INSIGHTFACE_MODEL=buffalo_l
RUN python -c "from insightface.app import FaceAnalysis; \
FaceAnalysis(name='${INSIGHTFACE_MODEL}').prepare(ctx_id=-1, det_size=(640,640))"

COPY app/ /srv/app/
COPY eval/ /srv/eval/

EXPOSE 9000

CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "9000"]
