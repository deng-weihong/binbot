FROM python:3.12-slim AS runtime

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

ARG DEBIAN_MIRROR=deb.debian.org
ARG PIP_INDEX_URL=https://pypi.org/simple

RUN sed -i "s#deb.debian.org/debian#${DEBIAN_MIRROR}/debian#g; s#security.debian.org/debian-security#${DEBIAN_MIRROR}/debian-security#g" /etc/apt/sources.list.d/debian.sources \
    && apt-get update \
    && apt-get install --no-install-recommends -y ca-certificates ffmpeg libopus0 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --uid 10001 binbot \
    && mkdir -p /etc/binbot \
    && mkdir -p /var/lib/binbot/assets /var/lib/binbot/feishu \
    && chown -R binbot:binbot /var/lib/binbot

WORKDIR /opt/binbot-server
COPY pyproject.toml README.md ./
COPY src ./src
RUN pip install --no-cache-dir --index-url "${PIP_INDEX_URL}" '.[feishu]'

USER binbot
EXPOSE 8765
STOPSIGNAL SIGTERM
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/healthz', timeout=2)"

ENTRYPOINT ["binbot-server"]
CMD ["--config", "/etc/binbot/config.toml"]
