FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends chromium ffmpeg pulseaudio pulseaudio-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt \
    && useradd --create-home app
COPY server/ server/
USER app
ENV PYTHONUNBUFFERED=1
ENTRYPOINT ["sh", "-c", "pulseaudio --start; exec python server/server.py \"$@\"", "--"]
