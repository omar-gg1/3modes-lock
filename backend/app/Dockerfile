# Build context is backend/ (see docker-compose.yml). Paths below are relative
# to backend/, so `app/` refers to backend/app/ — the Python package.
FROM python:3.11-slim

WORKDIR /srv

# Install deps first so Docker caches this layer when only code changes.
COPY app/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy the whole package so it imports as `app.*` (app/main.py -> app.main).
COPY app/ /srv/app/

EXPOSE 8000

# --host 0.0.0.0 so the API is reachable from outside the container.
CMD ["uvicorn", "app.main:app", "--host", "0.0.0.0", "--port", "8000"]
