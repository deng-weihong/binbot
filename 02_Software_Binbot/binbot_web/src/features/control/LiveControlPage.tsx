import { useWorkspace } from "../../shared/state/workspaceContext";
import { useEffect, useMemo, useRef, useState, type FormEvent } from "react";
import { ArrowUpRight, Mic, Monitor, Volume2 } from "lucide-react";
import { createConsoleTransport } from "../../shared/api/consoleApi";
import {
  controlDelay,
  parseDeviceControl,
  type DeviceControlSnapshot,
} from "./deviceControl";
import { FeatureRange, FeatureSwitch } from "../../shared/ui/FeatureControls";
import { uid, type Device } from "../../shared/model/data";

export default function LiveControlPage({
  device,
  notify,
}: {
  device: Device;
  notify: (message: string) => void;
}) {
  const workspace = useWorkspace();
  const microphone = workspace.snapshot?.reported.microphone === true;
  const api = useMemo(() => createConsoleTransport(), []);
  const [snapshot, setSnapshot] = useState<DeviceControlSnapshot | null>(null);
  const [draftBrightness, setBrightness] = useState<number | null>(null);
  const [draftVolume, setVolume] = useState<number | null>(null);
  const [pending, setPending] = useState("");
  const [error, setError] = useState("");
  const [loadError, setLoadError] = useState("");
  const [loading, setLoading] = useState(true);
  const lifetime = useRef<AbortController | null>(null);
  const busy = useRef(false);
  const requestSequence = useRef(0);
  const appliedSequence = useRef(0);

  async function load(signal: AbortSignal) {
    const sequence = ++requestSequence.current;
    const value = parseDeviceControl(
      await api.state(device.id, signal),
      device.id,
    );
    if (!signal.aborted && sequence > appliedSequence.current) {
      appliedSequence.current = sequence;
      setSnapshot((previous) => {
        if (
          previous?.control &&
          value.control?.boot_id === previous.control.boot_id &&
          value.control.revision < previous.control.revision
        )
          return previous;
        return value;
      });
    }
    return value;
  }
  useEffect(() => {
    const controller = new AbortController();
    lifetime.current = controller;
    setSnapshot(null);
    setBrightness(null);
    setVolume(null);
    setError("");
    setLoadError("");
    setLoading(true);
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      try {
        await load(controller.signal);
        if (!controller.signal.aborted) setLoadError("");
      } catch (reason) {
        if (!controller.signal.aborted) {
          setSnapshot(null);
          setLoadError(
            reason instanceof Error ? reason.message : "设备状态读取失败。",
          );
        }
      } finally {
        if (!controller.signal.aborted) {
          setLoading(false);
          timer = setTimeout(poll, 2000);
        }
      }
    };
    void poll();
    return () => {
      controller.abort();
      clearTimeout(timer);
    };
    // This page is mounted again when switching the device.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [device.id]);

  const state = snapshot?.control;
  const fresh = !!state && Date.now() - Date.parse(state.updated_at) < 20_000;
  const ready =
    snapshot?.control_online && snapshot.control_ready && state?.ready && fresh;
  const supported = (capability: string) =>
    !!ready && state?.capabilities.includes(capability);

  async function apply(kind: "brightness" | "volume", event: FormEvent) {
    event.preventDefault();
    const value = kind === "brightness" ? draftBrightness : draftVolume;
    const controller = lifetime.current;
    if (
      !controller ||
      controller.signal.aborted ||
      busy.current ||
      value === null ||
      !supported(kind === "brightness" ? "brightness_v1" : "volume_v1")
    )
      return;
    busy.current = true;
    setPending(kind);
    setError("");
    try {
      let command = await api.sendControl(
        device.id,
        kind === "brightness" ? "screen.brightness.set" : "audio.volume.set",
        value,
        uid(),
        controller.signal,
      );
      const deadline = Date.now() + 20_000;
      while (
        !["completed", "failed"].includes(command.status) &&
        Date.now() < deadline
      ) {
        await controlDelay(400, controller.signal);
        command = await api.command(
          device.id,
          command.command_id,
          controller.signal,
        );
      }
      if (command.status !== "completed")
        throw new Error(
          command.status === "failed"
            ? command.message || "设备执行失败。"
            : "设备执行结果未确认，请刷新状态。",
        );
      await load(controller.signal);
      if (!controller.signal.aborted) {
        if (kind === "brightness") setBrightness(null);
        else setVolume(null);
        notify(
          kind === "brightness"
            ? "设备已确认亮度设置。"
            : "设备已确认音量设置。",
        );
      }
    } catch (reason) {
      if (!controller.signal.aborted)
        setError(reason instanceof Error ? reason.message : "控制操作失败。");
    } finally {
      if (!controller.signal.aborted) {
        setPending("");
        busy.current = false;
      }
    }
  }

  const caption = loading
    ? "读取中"
    : ready
      ? "已连接"
      : snapshot?.control_online
        ? "设备暂未就绪"
        : "控制未连接";
  return (
    <div className="basic-controls feature-page">
      {error && (
        <p className="inline-error" role="alert">
          {error}
        </p>
      )}
      {loadError && (
        <p className="inline-error" role="alert">
          {loadError}
        </p>
      )}
      {(["brightness", "volume"] as const).map((kind) => {
        const screen = kind === "brightness",
          reported = screen ? state?.brightness : state?.volume;
        const draft = screen ? draftBrightness : draftVolume;
        const enabled =
          supported(screen ? "brightness_v1" : "volume_v1") &&
          reported !== null &&
          reported !== undefined;
        return (
          <form
            key={kind}
            className={`card ${screen ? "basic-screen-card" : "basic-sound-card"}`}
            onSubmit={(event) => void apply(kind, event)}
          >
            <header className="basic-card-heading">
              <span className={`basic-heading-icon ${screen ? "" : "mint"}`}>
                {screen ? (
                  <Monitor size={18} strokeWidth={1.5} />
                ) : (
                  <Volume2 size={18} strokeWidth={1.5} />
                )}
              </span>
              <h2>{screen ? "屏幕" : "声音与语音"}</h2>
              <span className="basic-card-caption">{caption}</span>
            </header>
            <div className="basic-setting-row basic-screen-summary">
              <div>
                <strong>{screen ? "屏幕亮度" : "设备音量"}</strong>
                <small>
                  {enabled
                    ? "调整后应用到当前设备，等待设备确认。"
                    : "等待设备连接并上报实际设置。"}
                </small>
              </div>
              <span className="quiet-chip">
                {reported === null || reported === undefined
                  ? "尚未读取"
                  : `已上报 ${reported}%`}
              </span>
            </div>
            {reported !== null && reported !== undefined && (
              <fieldset disabled={!enabled || !!pending}>
                <legend className="sr-only">
                  {screen ? "主屏亮度" : "设备音量"}
                </legend>
                <FeatureRange
                  label={screen ? "亮度" : "音量"}
                  min={screen ? 1 : 0}
                  value={draft ?? reported}
                  onChange={screen ? setBrightness : setVolume}
                />
                <footer className="basic-save-row">
                  <span>
                    {pending === kind
                      ? "等待设备确认…"
                      : draft !== null && draft !== reported
                        ? "有待应用的调整"
                        : "显示设备实际状态"}
                  </span>
                  <button
                    className="button primary"
                    type="submit"
                    disabled={draft === null || draft === reported}
                  >
                    {pending === kind ? "应用中…" : "应用到设备"}
                    <ArrowUpRight size={14} />
                  </button>
                </footer>
              </fieldset>
            )}
            {!screen && (
              <div className="basic-setting-row basic-microphone">
                <div>
                  <strong>
                    <Mic size={14} strokeWidth={1.5} />
                    麦克风
                  </strong>
                  <small>控制设备录音与唤醒。</small>
                </div>
                <FeatureSwitch
                  label="设备麦克风"
                  checked={microphone}
                  disabled={
                    !workspace.snapshot?.control.ready || workspace.pending
                  }
                  onChange={() =>
                    void workspace.action("microphone.set", {
                      enabled: !microphone,
                    })
                  }
                />
              </div>
            )}
            <p className="basic-local-note">
              {screen
                ? "表盘请在「表盘设置」中选择。"
                : "音量以设备确认的设置为准。"}
            </p>
          </form>
        );
      })}
    </div>
  );
}
