FROM python:3.12-slim

RUN apt-get update && apt-get install -y --no-install-recommends chromium ffmpeg gosu pulseaudio pulseaudio-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt \
    && useradd --create-home app
COPY server/ server/
ENV PYTHONUNBUFFERED=1
ENTRYPOINT ["sh", "-c", "mkdir -p /home/app/.local/share && chown -R app:app /home/app/.local/share && exec gosu app sh -c 'pulseaudio --start; exec python server/server.py \"$@\"' -- \"$@\"", "--"]
