#!/usr/bin/env bash

set -euo pipefail

if [[ "$(uname)" == "Darwin" ]]; then
    GETOPT_CMD="/opt/homebrew/opt/gnu-getopt/bin/getopt"
else
    GETOPT_CMD="getopt"
fi

PARSED_ARGS=$("${GETOPT_CMD}" -o c:v:i --long command:,container_version:,interactive -- "$@")
if [[ $? -ne 0 ]]; then
    echo "Error parsing arguments" >&2
    exit 1
fi

eval set -- "$PARSED_ARGS"

COMMAND=""
CONTAINER_VERSION=""
CONTAINER_INTERACTIVE=""

while true; do
    case "$1" in
        -c|--command) COMMAND="$2"; shift 2 ;;
        -v|--container_version) CONTAINER_VERSION="$2"; shift 2 ;;
        -i|--interactive) CONTAINER_INTERACTIVE="-it"; shift ;;
        --) shift; break ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$CONTAINER_INTERACTIVE" && -z "$COMMAND" ]]; then
    echo "Error: pass --interactive or --command" >&2
    exit 1
fi

if [[ -z "$CONTAINER_VERSION" ]]; then
    echo "Error: --container_version is required" >&2
    exit 1
fi

CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-podman}"
CONTAINER_REGISTRY="${CONTAINER_REGISTRY:-ghcr.io}"
CONTAINER_REPOSITORY="${CONTAINER_REPOSITORY:-matgla/tinycc-armv8m}"
CONTAINER_IMAGE="${CONTAINER_REGISTRY}/${CONTAINER_REPOSITORY}:${CONTAINER_VERSION}"

STATE_HOME="${XDG_STATE_HOME:-${HOME}/.local/state}"
HISTORY_DIR="${STATE_HOME}/tinycc-armv8m-container"
HISTORY_FILE="${HISTORY_DIR}/bash_history"
mkdir -p "$HISTORY_DIR"
touch "$HISTORY_FILE"

RUN_ARGS=(
    run
    --rm
    -v "$(pwd):/workspace"
    -v "${HISTORY_FILE}:/root/.bash_history"
    -e "HISTFILE=/root/.bash_history"
    -w /workspace
)

if [[ "$CONTAINER_RUNTIME" == "podman" ]]; then
    RUN_ARGS+=(--userns=keep-id)
fi

if [[ -n "$CONTAINER_INTERACTIVE" ]]; then
    RUN_ARGS+=("$CONTAINER_INTERACTIVE")
fi

RUN_ARGS+=("$CONTAINER_IMAGE")

if [[ -n "$COMMAND" ]]; then
    RUN_ARGS+=(/bin/bash -lc "$COMMAND")
else
    RUN_ARGS+=(/bin/bash)
fi

exec "$CONTAINER_RUNTIME" "${RUN_ARGS[@]}"
