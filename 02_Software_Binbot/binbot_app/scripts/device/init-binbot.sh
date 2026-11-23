#!/bin/sh

OEM_DIR="/oem"
RUN_DIR="/var/run"
AUDIO_SOCKET="${BINBOT_AUDIO_SOCKET:-/tmp/binbot_audio.sock}"
SLAVE_SOCKET="${BINBOT_SLAVE_SOCKET:-/tmp/binbot_slave.sock}"
AGENT_SOCKET="${BINBOT_AGENT_SOCKET:-/tmp/binbot_agent.sock}"
CONFIG_FILE="${BINBOT_CONFIG_FILE:-${OEM_DIR}/binbot.env}"

if [ -r "$CONFIG_FILE" ]; then
    set -a
    # The file is root-owned deployment configuration, not device input.
    . "$CONFIG_FILE"
    set +a
fi

# China Standard Time, including on images without tzdata.
CONTROL_DEVICE_ID="${BINBOT_DEVICE_ID:-}"
export BINBOT_CONTROL_ENABLE="${BINBOT_CONTROL_ENABLE:-0}"
export BINBOT_CONTROL_SOCKET="${BINBOT_CONTROL_SOCKET:-/tmp/binbot_control.sock}"
export TZ=CST-8
export ALSA_CONFIG_PATH="${ALSA_CONFIG_PATH:-/usr/share/alsa/alsa.conf}"
export LD_LIBRARY_PATH="${OEM_DIR}/lib:${OEM_DIR}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export BINBOT_AUDIO_SOCKET="$AUDIO_SOCKET"
export BINBOT_AGENT_SOCKET="$AGENT_SOCKET"
export BINBOT_DIALOG_URL="${BINBOT_DIALOG_URL:-ws://127.0.0.1:8080/v1/device/ws}"
export BINBOT_DEVICE_ID="${BINBOT_DEVICE_ID:-binbot-001}"
export BINBOT_SLAVE_DEVICE="${BINBOT_SLAVE_DEVICE:-/dev/ttyS4}"
export BINBOT_COMPONENT_UART="${BINBOT_COMPONENT_UART:-/dev/ttyS3}"
export BINBOT_SLAVE_ADDRESS="${BINBOT_SLAVE_ADDRESS:-1}"
export BINBOT_SLAVE_SOCKET="$SLAVE_SOCKET"
export BINBOT_SLAVE_POLL_MS="${BINBOT_SLAVE_POLL_MS:-500}"
export BINBOT_SLAVE_TIMEOUT_MS="${BINBOT_SLAVE_TIMEOUT_MS:-100}"
export BINBOT_SLAVE_RETRIES="${BINBOT_SLAVE_RETRIES:-3}"
export BINBOT_WAKE_SOUND="${BINBOT_WAKE_SOUND:-${OEM_DIR}/assets/audio/wake_notification.wav}"
export BINBOT_WAKE_SOUND_VOLUME="${BINBOT_WAKE_SOUND_VOLUME:-0.35}"
export BINBOT_AUDIO_PLAYBACK_DEVICE="${BINBOT_AUDIO_PLAYBACK_DEVICE:-plug:dmix}"
export BINBOT_WAKE_ENABLE="${BINBOT_WAKE_ENABLE:-0}"
export BINBOT_WAKE_ENGINE="${BINBOT_WAKE_ENGINE:-miya}"
export BINBOT_HIXIAOWEN_MODEL_DIR="${BINBOT_HIXIAOWEN_MODEL_DIR:-${OEM_DIR}/assets/wake/hixiaowen}"
export BINBOT_HIXIAOWEN_CHANNEL="${BINBOT_HIXIAOWEN_CHANNEL:-mix}"
export BINBOT_HIXIAOWEN_THRESHOLD="${BINBOT_HIXIAOWEN_THRESHOLD:-0.9}"
export BINBOT_HIXIAOWEN_INTERVAL_MS="${BINBOT_HIXIAOWEN_INTERVAL_MS:-400}"
export BINBOT_SNOWBOY_RESOURCE="${BINBOT_SNOWBOY_RESOURCE:-${OEM_DIR}/assets/wake/snowboy/common.res}"
export BINBOT_SNOWBOY_MODEL="${BINBOT_SNOWBOY_MODEL:-${OEM_DIR}/assets/wake/snowboy/aa.pmdl}"
export BINBOT_SNOWBOY_SENSITIVITY="${BINBOT_SNOWBOY_SENSITIVITY:-0.4}"
export BINBOT_SNOWBOY_GAIN="${BINBOT_SNOWBOY_GAIN:-1.0}"
export BINBOT_WAKE_MIN_CONFIDENCE="${BINBOT_WAKE_MIN_CONFIDENCE:-0.75}"
export BINBOT_CAMERA_ISP="${BINBOT_CAMERA_ISP:-1}"
export BINBOT_CAMERA_ISP_BIN="${BINBOT_CAMERA_ISP_BIN:-${OEM_DIR}/usr/bin/rkaiq_3A_server}"
export BINBOT_FACE_DETECTION="${BINBOT_FACE_DETECTION:-1}"
export BINBOT_ROCKIVA_LIBRARY="${BINBOT_ROCKIVA_LIBRARY:-librockiva.so}"
export BINBOT_ROCKIVA_MODEL_DIR="${BINBOT_ROCKIVA_MODEL_DIR:-${OEM_DIR}/models/rockiva}"
export BINBOT_FACE_MIN_SCORE="${BINBOT_FACE_MIN_SCORE:-45}"
export BINBOT_FACE_STABLE_FRAMES="${BINBOT_FACE_STABLE_FRAMES:-2}"
export BINBOT_FACE_INTERVAL_MS="${BINBOT_FACE_INTERVAL_MS:-200}"
export BINBOT_HAND_DETECTION="${BINBOT_HAND_DETECTION:-1}"
export BINBOT_FACE_RECOGNITION="${BINBOT_FACE_RECOGNITION:-0}"
export BINBOT_FACE_RECOGNITION_MODEL="${BINBOT_FACE_RECOGNITION_MODEL:-${OEM_DIR}/assets/vision/face/recognition.rknn}"
export BINBOT_FACE_REFERENCE_FEATURE="${BINBOT_FACE_REFERENCE_FEATURE:-${OEM_DIR}/assets/vision/face/reference.f32}"
export BINBOT_FACE_MATCH_THRESHOLD="${BINBOT_FACE_MATCH_THRESHOLD:-0.65}"
export BINBOT_FACE_CONFIRM_FRAMES="${BINBOT_FACE_CONFIRM_FRAMES:-2}"
export BINBOT_FACE_RECOGNITION_INTERVAL_MS="${BINBOT_FACE_RECOGNITION_INTERVAL_MS:-500}"
export BINBOT_FACE_CROP_MARGIN="${BINBOT_FACE_CROP_MARGIN:-0.35}"
export BINBOT_FACE_LOG_RESULTS="${BINBOT_FACE_LOG_RESULTS:-0}"
export BINBOT_FACE_CAPTURE_REFERENCE="${BINBOT_FACE_CAPTURE_REFERENCE:-}"
export BINBOT_FACE_CAPTURE_SAMPLES="${BINBOT_FACE_CAPTURE_SAMPLES:-10}"
export BINBOT_INITIAL_ROUTE="${BINBOT_INITIAL_ROUTE:-home}"
export BINBOT_CAMERA_MIRROR="${BINBOT_CAMERA_MIRROR:-1}"
export BINBOT_HAND_PALM_MODEL="${BINBOT_HAND_PALM_MODEL:-${OEM_DIR}/assets/vision/hand/palm_detection_lite_int8_rv1106.rknn}"
export BINBOT_HAND_LANDMARK_MODEL="${BINBOT_HAND_LANDMARK_MODEL:-${OEM_DIR}/assets/vision/hand/hand_landmark_lite_int8_rv1106.rknn}"
export BINBOT_HAND_PALM_SCORE="${BINBOT_HAND_PALM_SCORE:-50}"
export BINBOT_HAND_PRESENCE_SCORE="${BINBOT_HAND_PRESENCE_SCORE:-50}"
export BINBOT_HAND_INTERVAL_MS="${BINBOT_HAND_INTERVAL_MS:-500}"
export BINBOT_CUP_DETECTION="${BINBOT_CUP_DETECTION:-1}"
export BINBOT_CUP_MODEL="${BINBOT_CUP_MODEL:-${OEM_DIR}/assets/vision/cup/yolov5n_coco_int8_rv1106.rknn}"
export BINBOT_CUP_SCORE="${BINBOT_CUP_SCORE:-0.25}"
export BINBOT_CUP_NMS="${BINBOT_CUP_NMS:-0.45}"
export BINBOT_CUP_INTERVAL_MS="${BINBOT_CUP_INTERVAL_MS:-500}"
export BINBOT_CAMERA_STREAM="${BINBOT_CAMERA_STREAM:-1}"
export BINBOT_STREAM_WIDTH="${BINBOT_STREAM_WIDTH:-640}"
export BINBOT_STREAM_HEIGHT="${BINBOT_STREAM_HEIGHT:-480}"
export BINBOT_STREAM_FPS="${BINBOT_STREAM_FPS:-15}"
export BINBOT_STREAM_BITRATE_KBPS="${BINBOT_STREAM_BITRATE_KBPS:-1024}"
export BINBOT_STREAM_GOP="${BINBOT_STREAM_GOP:-15}"
export BINBOT_STREAM_VI_CHANNEL="${BINBOT_STREAM_VI_CHANNEL:-1}"
export BINBOT_STREAM_VENC_CHANNEL="${BINBOT_STREAM_VENC_CHANNEL:-0}"
export BINBOT_STREAM_RTSP_PORT="${BINBOT_STREAM_RTSP_PORT:-554}"
export BINBOT_STREAM_RTSP_PATH="${BINBOT_STREAM_RTSP_PATH:-/live/binbot}"
export BINBOT_AGENT_ENABLE="${BINBOT_AGENT_ENABLE:-1}"
export BINBOT_AGENT_BASE_URL="${BINBOT_AGENT_BASE_URL:-https://ark.cn-beijing.volces.com/api/v3}"
export BINBOT_AGENT_MODEL="${BINBOT_AGENT_MODEL:-doubao-seed-2-0-lite-260428}"
export BINBOT_AGENT_IMAGE_PATH="${BINBOT_AGENT_IMAGE_PATH:-/tmp/binbot_agent_frame.jpg}"
export BINBOT_AGENT_CAPTURE_REQUEST="${BINBOT_AGENT_CAPTURE_REQUEST:-/tmp/binbot_agent_capture.request}"
export BINBOT_AGENT_CA_FILE="${BINBOT_AGENT_CA_FILE:-${OEM_DIR}/ssl/certs/ca-certificates.crt}"
export BINBOT_AGENT_TIMEOUT_MS="${BINBOT_AGENT_TIMEOUT_MS:-60000}"
export BINBOT_AGENT_IPC_TIMEOUT_MS="${BINBOT_AGENT_IPC_TIMEOUT_MS:-90000}"

wait_for_path()
{
    path="$1"
    attempts="$2"
    count=0
    while [ ! -e "$path" ] && [ "$count" -lt "$attempts" ]; do
        count=$((count + 1))
        sleep 1
    done
    [ -e "$path" ]
}

start_daemon()
{
    name="$1"
    nice_value="${2:-0}"
    path="${OEM_DIR}/${name}"
    pid_file="${RUN_DIR}/${name}.pid"
    log_file="/tmp/${name}.log"

    if [ ! -x "$path" ]; then
        echo "$name: executable not found: $path"
        return 1
    fi
    if [ -f "$pid_file" ] && start-stop-daemon -K -t -p "$pid_file" -x "$path" \
            >/dev/null 2>&1; then
        echo "$name is already running"
        return 0
    fi
    rm -f "$pid_file"
    if [ "$nice_value" -ne 0 ]; then
        nohup nice -n "$nice_value" "$path" </dev/null >>"$log_file" 2>&1 &
    else
        nohup "$path" </dev/null >>"$log_file" 2>&1 &
    fi
    pid=$!
    echo "$pid" >"$pid_file"
    echo "Starting $name"
}

stop_daemon()
{
    name="$1"
    path="${OEM_DIR}/${name}"
    pid_file="${RUN_DIR}/${name}.pid"
    echo "Stopping $name"
    start-stop-daemon -K -o -p "$pid_file" -x "$path" -s TERM >/dev/null 2>&1 || true

    attempts=0
    while start-stop-daemon -K -t -p "$pid_file" -x "$path" >/dev/null 2>&1 && \
          [ "$attempts" -lt 5 ]; do
        attempts=$((attempts + 1))
        sleep 1
    done
    start-stop-daemon -K -o -p "$pid_file" -x "$path" -s KILL >/dev/null 2>&1 || true
    rm -f "$pid_file"
}

start_camera_isp()
{
    if [ "${BINBOT_CAMERA_ISP}" != "1" ]; then
        echo "camera ISP: disabled by configuration"
        return 0
    fi
    path="${BINBOT_CAMERA_ISP_BIN}"
    pid_file="${RUN_DIR}/binbot_camera_isp.pid"
    log_file="/tmp/binbot_camera_isp.log"
    if [ ! -x "$path" ]; then
        echo "camera ISP: executable not found: $path (camera page will show unavailable)"
        return 0
    fi
    if pidof rkaiq_3A_server >/dev/null 2>&1; then
        echo "camera ISP is already running (external manager)"
        return 0
    fi
    if [ -f "$pid_file" ] && start-stop-daemon -K -t -p "$pid_file" -x "$path" \
            >/dev/null 2>&1; then
        echo "camera ISP is already running"
        return 0
    fi
    rm -f "$pid_file"
    rm -f /tmp/UNIX.domain0
    : >"$log_file"
    nohup env LD_LIBRARY_PATH="${OEM_DIR}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" \
        "$path" </dev/null >>"$log_file" 2>&1 &
    pid=$!
    echo "$pid" >"$pid_file"
    echo "Starting camera ISP"

    # Rockchip AIQ must subscribe to the ISP stream event before VI starts.
    # Under boot-time load, starting binbot_camera immediately can lose that
    # one-shot event and leave all ISP algorithms disabled until a restart.
    attempts=0
    while [ "$attempts" -lt 10 ]; do
        if grep -q "wait stream start event" "$log_file" 2>/dev/null; then
            echo "camera ISP is ready"
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "camera ISP exited during startup; see $log_file"
            return 1
        fi
        attempts=$((attempts + 1))
        sleep 1
    done
    echo "camera ISP readiness timed out; continuing without confirmation"
    return 0
}

stop_camera_isp()
{
    path="${BINBOT_CAMERA_ISP_BIN}"
    pid_file="${RUN_DIR}/binbot_camera_isp.pid"
    if [ -f "$pid_file" ]; then
        echo "Stopping camera ISP"
        start-stop-daemon -K -o -p "$pid_file" -x "$path" -s TERM >/dev/null 2>&1 || true
        start-stop-daemon -K -o -p "$pid_file" -x "$path" -s KILL >/dev/null 2>&1 || true
        rm -f "$pid_file"
    fi
}

status_daemon()
{
    name="$1"
    path="${OEM_DIR}/${name}"
    pid_file="${RUN_DIR}/${name}.pid"
    if start-stop-daemon -K -t -p "$pid_file" -x "$path" >/dev/null 2>&1; then
        echo "$name is running (pid $(cat "$pid_file"))"
        return 0
    fi
    echo "$name is not running"
    return 1
}

start_all()
{
    cd "$OEM_DIR" || return 1
    if [ "$BINBOT_CONTROL_ENABLE" = "1" ]; then
        if [ -z "$CONTROL_DEVICE_ID" ] || [ -z "${BINBOT_DEVICE_TOKEN:-}" ] || \
           [ -z "${BINBOT_MQTT_HOST:-}" ] || [ ! -x "${OEM_DIR}/binbot_control" ]; then
            echo "binbot_control: configure a unique BINBOT_DEVICE_ID, BINBOT_DEVICE_TOKEN and BINBOT_MQTT_HOST; install binbot_control"
            return 1
        fi
    fi
    mkdir -p "${OEM_DIR}/data" || return 1
    if [ -x "${OEM_DIR}/binbot_slave" ]; then
        start_daemon binbot_slave || return 1
        if ! wait_for_path "$SLAVE_SOCKET" 5; then
            echo "binbot_slave: IPC socket was not created"
            return 1
        fi
    else
        echo "binbot_slave: executable not found; UART4 integration disabled"
    fi
    start_daemon binbot_audio -8 || return 1
    if ! wait_for_path "$AUDIO_SOCKET" 5; then
        echo "binbot_audio: socket was not created"
        return 1
    fi
    if [ "$BINBOT_WAKE_ENABLE" = "1" ]; then
        # Miya consumes continuous 20 ms blocks; deprioritizing it causes
        # PCM backpressure and repeated DSP resets during CPU contention.
        start_daemon binbot_wake 0 || return 1
    else
        echo "binbot_wake: disabled by configuration"
    fi
    start_camera_isp
    if [ "${BINBOT_CAMERA_STREAM}" = "1" ]; then
        start_daemon binbot_camera 5 || return 1
    else
        echo "binbot_camera: disabled by configuration"
    fi
    if [ "${BINBOT_AGENT_ENABLE}" = "1" ] && \
       { [ -n "${BINBOT_AGENT_API_KEY:-}" ] || [ -n "${ARK_API_KEY:-}" ]; }; then
        start_daemon binbot_agent || return 1
        if ! wait_for_path "$AGENT_SOCKET" 5; then
            echo "binbot_agent: IPC socket was not created"
            return 1
        fi
    else
        echo "binbot_agent: disabled or API key is not configured"
    fi
    # Keep legacy images bootable while allowing the new dialog binary to be
    # deployed independently. A complete deployment always includes it.
    if [ -x "${OEM_DIR}/binbot_dialog" ]; then
        start_daemon binbot_dialog -4 || return 1
    else
        echo "binbot_dialog: executable not found; voice dialog disabled"
    fi

    if ! wait_for_path /dev/fb0 10; then
        echo "binbot_app: /dev/fb0 is not ready"
        return 1
    fi
    unset DISPLAY
    unset WAYLAND_DISPLAY
    export LVGL_FBDEV="${LVGL_FBDEV:-/dev/fb0}"
    start_daemon binbot_app 5 || return 1
    if [ "$BINBOT_CONTROL_ENABLE" = "1" ]; then
        start_daemon binbot_control 0 || return 1
    fi
}

stop_all()
{
    if [ -x "${OEM_DIR}/binbot_control" ]; then
        stop_daemon binbot_control
    fi
    stop_daemon binbot_app
    stop_daemon binbot_dialog
    if [ -x "${OEM_DIR}/binbot_agent" ]; then
        stop_daemon binbot_agent
    fi
    if [ -x "${OEM_DIR}/binbot_camera" ]; then
        stop_daemon binbot_camera
    fi
    stop_camera_isp
    stop_daemon binbot_wake
    stop_daemon binbot_audio
    if [ -x "${OEM_DIR}/binbot_slave" ]; then
        stop_daemon binbot_slave
    fi
    rm -f "$AUDIO_SOCKET"
    rm -f "$SLAVE_SOCKET"
    rm -f "$AGENT_SOCKET" "$BINBOT_AGENT_CAPTURE_REQUEST"
    rm -f "$BINBOT_CONTROL_SOCKET"
}

status_all()
{
    result=0
    if [ -x "${OEM_DIR}/binbot_slave" ]; then
        status_daemon binbot_slave || result=1
    fi
    status_daemon binbot_audio || result=1
    if [ "$BINBOT_WAKE_ENABLE" = "1" ]; then
        status_daemon binbot_wake || result=1
    else
        echo "binbot_wake is disabled"
    fi
    if [ -x "${OEM_DIR}/binbot_dialog" ]; then
        status_daemon binbot_dialog || result=1
    fi
    if [ "${BINBOT_AGENT_ENABLE}" = "1" ] && \
       { [ -n "${BINBOT_AGENT_API_KEY:-}" ] || [ -n "${ARK_API_KEY:-}" ]; }; then
        status_daemon binbot_agent || result=1
    else
        echo "binbot_agent is disabled"
    fi
    if [ "${BINBOT_CAMERA_STREAM}" = "1" ] && [ -x "${OEM_DIR}/binbot_camera" ]; then
        status_daemon binbot_camera || result=1
    fi
    status_daemon binbot_app || result=1
    if [ "$BINBOT_CONTROL_ENABLE" = "1" ]; then
        status_daemon binbot_control || result=1
    fi
    return "$result"
}

case "${1:-start}" in
    start)
        start_all
        ;;
    stop)
        stop_all
        ;;
    restart)
        stop_all
        start_all
        ;;
    status)
        status_all
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
