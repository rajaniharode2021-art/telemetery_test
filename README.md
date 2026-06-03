# Telemetry App Assessment

Time expectation: 2–4 hours.

## Overview

You are building a small telemetry forwarding service for an embedded-style system in C.

A simulated MCU writes binary telemetry packets into shared memory. Your program must read those packets, decode them using the supplied configuration, and publish telemetry to an MQTT broker. Your program must also report operational events to the supplied log server.

Although the data path is intentionally small, treat this as production software. We are interested in how you reason about correctness, reliability, failure handling, observability, and maintainability.

## Provided Files

You will be given the contents of this `telemetry-app` directory. It contains the runtime harness, simulator, configuration, and container setup used for the assessment:

- `simulated-mcu.py`: a script that pushes telemetry data into shared memory.
- `log-server.py`: a script to simulate a remote HTTP server that collects batched logs.
- `telemetry_config.json`: packet and shared-memory configuration.
- `Dockerfile`: the base Debian container used for the exercise.
- `docker-compose.yml`: the local environment definition.
- `start-services.sh`: the container entrypoint that starts the supplied services.

Do not modify `simulated-mcu.py` or `log-server.py`; these represent the supplied assessment harness. All other files are fair game. You may update the `Dockerfile`, `docker-compose.yml`, `start-services.sh`, and add any source files needed to install dependencies, build your solution, and start your telemetry forwarder. Your final submission must run inside this Docker environment.

## Runtime Environment

The Docker environment builds a single container image and runs the assessment harness inside it. That container starts:

- An MQTT broker listening on `localhost:1883` with anonymous access enabled.
- The simulated MCU, which writes binary telemetry packets into shared memory.
- The log server, which accepts operational log batches over HTTP.

The simulated MCU writes telemetry to the shared-memory location described by `telemetry_config.json`. For your submission, do not assume packet details that are not represented in the configuration.

### Shared Memory

Telemetry packets are written to `/dev/shm/data` by default. The shared-memory file path, packet size, and packet layout are also described in `telemetry_config.json`.

The packet `timestamp` field is a `uint32` value containing Unix epoch time in milliseconds truncated to 32 bits. Treat it as a wrapped timestamp value rather than a full-width Unix timestamp.

### MQTT Telemetry

The MQTT broker listens on `localhost:1883` with anonymous access enabled. Publish decoded telemetry to topic `/vehicle/telemetry`.

Each MQTT message should be a single JSON object containing all decoded packet fields from `telemetry_config.json`:

```json
{
  "magic": 57005,
  "seq": 123,
  "timestamp": 1710000000,
  "speed": 43.0,
  "batt_volt": 12.15,
  "fault": 0,
  "software_version": "1.0.0"
}
```

### Log Server

The log server listens on HTTP port `8080` by default unless configured otherwise. It exposes:

- `POST /batch` with a JSON array of log entries.
- `GET /batch` for reading stored log entries.

Each log entry is a JSON object:

```json
{
  "message": "description of the event",
  "timestamp": 1710000000000
}
```

Example requests:

```sh
curl -X POST http://localhost:8080/batch \
  -H 'Content-Type: application/json' \
  -d '[
    {
      "message": "telemetry forwarder started",
      "timestamp": 1710000000000
    },
    {
      "message": "missed sequence 42",
      "timestamp": 1710000001000
    }
  ]'

curl http://localhost:8080/batch
```

The Compose file exposes the MQTT and log server ports to the host. Environment variables in `docker-compose.yml` allow basic tuning of shared memory size, MCU write behavior, and log server failure injection.

## Build and Run Locally

To build and run the environment locally:

```sh
docker compose build
docker compose up
```

To rebuild after changing the `Dockerfile` or source files:

```sh
docker compose up --build
```

To stop and remove the running container:

```sh
docker compose down
```

## Your Task

Create a single top-level process that runs the telemetry forwarder. It may start worker processes or threads if that is appropriate for your design.

Your service should:

- Read telemetry packets from shared memory.
- Decode packets according to `telemetry_config.json`.
- Publish telemetry as JSON to MQTT topic `/vehicle/telemetry`.
- Send operational logs to the log server in batches.
- Continue operating across transient failures.
- Shut down cleanly when the container stops.

Telemetry messages should contain the decoded packet fields from the configuration. Additional useful metadata is acceptable when it helps explain packet state or processing behavior.

Operational logs should include information that would help diagnose the service in production, such as invalid packets, missed sequence ranges, publish failures, reconnects, dropped or retried work, and log delivery failures.

## Reliability Expectations

We will evaluate more than the happy path. Your solution should behave sensibly if dependencies are slow, unavailable, or restarted.

Examples of conditions your program should consider:

- MQTT broker disconnects or publish failures.
- Log server errors, including temporary `5xx` responses.
- Network delays or refused connections.
- Gaps, duplicates, invalid packets, or out-of-order telemetry.
- Process restarts.
- Backpressure when output systems are unavailable.

We are not prescribing a specific architecture. Choose an approach that you can justify and that keeps data loss, duplicate delivery, resource usage, and recovery behavior under control.

## What We Are Looking For

We value clear, robust engineering over a large amount of code.

Strong submissions usually show:

- Correct decoding based on the configuration file rather than hardcoded packet assumptions.
- A reliable publish path with appropriate MQTT behavior.
- Thoughtful buffering and recovery when downstream systems are unavailable.
- Bounded resource usage under failure.
- Batched, useful operational logging.
- Clear separation between data ingestion, delivery, persistence, and supervision concerns.
- Meaningful tests or validation steps.
- Simple build and run instructions.

Please include a short explanation of your design tradeoffs, known limitations, and how you tested your solution.

## Submission

Submit a link to a private GitHub repository. Please ensure the repository is accessible to the reviewers and includes:

- C source code that builds a single binary acting as the telemetry forwarder.
- An updated `Dockerfile` that builds your solution and runs it in the provided environment.
- A GitHub Actions pipeline for CI/CD.
- A brief design note describing the important choices you made.

The evaluator will run your solution in the container with the provided scripts, MQTT broker, log server, and telemetry configuration.
