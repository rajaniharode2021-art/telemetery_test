FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    bsdmainutils \
    cmake \
    curl \
    gdb \
    git \
    less \
    pkg-config \
    procps \
    vim \
    python3 \
    python3-pip \
    mosquitto mosquitto-clients \
    && pip3 install --no-cache-dir --break-system-packages paho-mqtt \
    && rm -rf /var/lib/apt/lists/*

RUN printf '%s\n' \
    'listener 1883' \
    'allow_anonymous true' \
    'log_type all' \
    'log_dest stdout' \
    > /etc/mosquitto/mosquitto.conf

RUN mkdir -p /workspace/build
COPY simulated-mcu.py log-server.py telemetry_config.json /workspace/build/
COPY start-services.sh /usr/local/bin/start-services
RUN chmod +x /usr/local/bin/start-services

WORKDIR /workspace

EXPOSE 1883 8080

CMD ["start-services"]