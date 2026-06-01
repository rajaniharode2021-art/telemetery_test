#!/usr/bin/env bash
set -euo pipefail

pids=()

stop_services() {
  if ((${#pids[@]})); then
    kill -TERM "${pids[@]}" 2>/dev/null || true
    wait "${pids[@]}" 2>/dev/null || true
  fi
}

trap stop_services INT TERM EXIT

export TELEMETRY_SHM_PATH="${TELEMETRY_SHM_PATH:-/dev/shm/data}"
export TELEMETRY_PACKET_SIZE="${TELEMETRY_PACKET_SIZE:-26}"
export LOG_SERVER_DATA_DIR="${LOG_SERVER_DATA_DIR:-/var/lib/log-server}"

echo "starting mosquitto on localhost:1883"
mosquitto -c /etc/mosquitto/mosquitto.conf -d

echo "starting log server on :${PORT:-8080}"
python3 /workspace/build/log-server.py &
pids+=("$!")

echo "starting simulated MCU"
python3 /workspace/build/simulated-mcu.py &
pids+=("$!")

wait -n "${pids[@]}"
exit_code=$?

stop_services
exit "$exit_code"
