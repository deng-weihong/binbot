import { useEffect, useRef, useState } from "react";
import {
  Camera,
  LoaderCircle,
  Maximize,
  Pause,
  Play,
  RefreshCw,
  FlipHorizontal2,
} from "lucide-react";
import type { Device } from "../../shared/model/data";
import { CameraConnection, type CameraPhase } from "./cameraSession";
import { useConsole } from "../../shared/state/consoleContext";

type CameraProps = {
  device: Device;
  name: string;
  notify?: (message: string) => void;
};

export default function HomeCameraCard({ device, name, notify }: CameraProps) {
  const { mode } = useConsole();
  const videoRef = useRef<HTMLVideoElement>(null);
  const frameRef = useRef<HTMLDivElement>(null);
  const connectionRef = useRef<CameraConnection | null>(null);
  const [paused, setPaused] = useState(false);
  const [attempt, setAttempt] = useState(0);
  const [phase, setPhase] = useState<CameraPhase | "idle">("idle");
  const [message, setMessage] = useState("");
  const [mirrored, setMirrored] = useState(false);
  const requested = mode === "live" && !paused;
  const live = requested && device.online && phase === "playing";
  const connecting =
    requested && device.online && (phase === "idle" || phase === "connecting");
  const previewActive = live || connecting;

  useEffect(() => {
    if (!requested || !device.online) return;
    let active = true;
    const video = videoRef.current;
    const connection = new CameraConnection(device.id, {
      phase: (value, detail) => {
        if (active) {
          setPhase(value);
          setMessage(detail);
        }
      },
      stream: (stream) => {
        if (!active || !video) return;
        video.srcObject = stream;
        if (stream)
          void video
            .play()
            .catch(() =>
              connection.fail(
                new Error("浏览器暂时无法播放画面，请重新连接。"),
              ),
            );
      },
    });
    connectionRef.current = connection;
    const release = () => {
      connection.dispose();
      if (video) {
        video.pause();
        video.srcObject = null;
      }
    };
    const pagehide = () => {
      release();
      setPhase("idle");
    };
    const pageshow = (event: PageTransitionEvent) => {
      if (event.persisted) setAttempt((value) => value + 1);
    };
    window.addEventListener("pagehide", pagehide);
    window.addEventListener("pageshow", pageshow);
    queueMicrotask(() => void connection.start());
    return () => {
      active = false;
      window.removeEventListener("pagehide", pagehide);
      window.removeEventListener("pageshow", pageshow);
      connectionRef.current = null;
      release();
    };
  }, [requested, attempt, device.id, device.online]);

  const connect = () => {
    if (mode === "demo") return;
    setMessage("");
    setPhase("connecting");
    setPaused(false);
    setAttempt((value) => value + 1);
  };
  const pause = () => {
    connectionRef.current?.dispose();
    setPaused(true);
    setPhase("idle");
    setMessage("");
  };
  const fullscreen = async () => {
    try {
      await frameRef.current?.requestFullscreen();
    } catch {
      notify?.("当前浏览器无法打开全屏预览。");
    }
  };
  const title = !device.online
    ? "摄像头离线"
    : connecting
      ? "正在连接摄像头"
      : phase === "error"
        ? "暂时无法预览"
        : paused
          ? "预览已暂停"
          : "预览未开启";
  const placeholderDetail = !device.online
    ? "设备上线后，可查看画面。"
    : mode === "demo"
      ? "登录真实设备后，打开首页即可预览。"
      : message;

  return (
    <section
      className="card home-camera-card"
      id="camera-preview"
      aria-label="摄像头视频预览"
    >
      <header className="home-camera-heading">
        <h2>桌面预览</h2>
        <button
          type="button"
          className="icon-button subtle"
          aria-label="全屏预览"
          title="全屏预览"
          disabled={
            !live ||
            typeof document === "undefined" ||
            !document.fullscreenEnabled
          }
          onClick={() => void fullscreen()}
        >
          <Maximize size={17} />
        </button>
      </header>
      <div className="camera-preview is-compact">
        <div className={`camera-frame ${live ? "is-live" : ""}`} ref={frameRef}>
          <video
            ref={videoRef}
            muted
            playsInline
            autoPlay
            className={mirrored ? "is-mirrored" : ""}
            aria-label={`${name}摄像头实时画面`}
            onPlaying={() => connectionRef.current?.markPlaying()}
            onError={() =>
              connectionRef.current?.fail(
                new Error("视频播放中断，请重新连接。"),
              )
            }
          />
          {!live && (
            <div className="camera-placeholder">
              <span className="camera-placeholder-icon">
                {connecting ? (
                  <LoaderCircle
                    className="spinning"
                    size={24}
                    strokeWidth={1.4}
                  />
                ) : (
                  <Camera size={24} strokeWidth={1.2} />
                )}
              </span>
              <strong>{title}</strong>
              {placeholderDetail && <p>{placeholderDetail}</p>}
            </div>
          )}
          {live && (
            <span className="camera-live-tag">
              <i />
              实时画面
            </span>
          )}
        </div>
        <div className="camera-preview-controls">
          <span
            className={`camera-view-status ${live ? "is-live" : ""}`}
            role="status"
          >
            <i />
            {!device.online
              ? "设备离线"
              : live
                ? "已连接"
                : connecting
                  ? "连接中"
                  : phase === "error"
                    ? "未连接"
                    : paused
                      ? "已暂停"
                      : "预览已关闭"}
          </span>
          <div>
            <button
              type="button"
              className="camera-connect-link"
              disabled={mode === "demo" || !device.online}
              aria-label={
                previewActive
                  ? "暂停摄像头预览"
                  : paused
                    ? "恢复摄像头预览"
                    : "连接摄像头预览"
              }
              onClick={previewActive ? pause : connect}
            >
              {previewActive ? <Pause size={13} /> : <Play size={13} />}
              {previewActive
                ? "暂停预览"
                : paused
                  ? "恢复预览"
                  : phase === "error"
                    ? "重新连接"
                    : "连接预览"}
            </button>
            <button
              type="button"
              className="icon-button subtle"
              aria-label="重新连接摄像头"
              title="重新连接"
              disabled={!requested || connecting || !device.online}
              onClick={connect}
            >
              <RefreshCw size={16} />
            </button>
            <button
              type="button"
              className="icon-button subtle"
              aria-label="镜像画面"
              title="镜像画面"
              aria-pressed={mirrored}
              onClick={() => setMirrored((value) => !value)}
            >
              <FlipHorizontal2 size={16} />
            </button>
          </div>
        </div>
      </div>
    </section>
  );
}
