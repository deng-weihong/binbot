import { useWorkspace } from "../../shared/state/workspaceContext";
import { record } from "../../shared/model/workspaceModel";
import {
  useEffect,
  useId,
  useRef,
  useState,
  type CSSProperties,
  type Dispatch,
  type SetStateAction,
} from "react";
import {
  Check,
  ChevronDown,
  CloudLightning,
  CloudRain,
  Coffee,
  Flame,
  Headphones,
  Keyboard,
  LoaderCircle,
  Pause,
  Play,
  SlidersHorizontal,
  Timer,
  Trees,
  Volume2,
  Waves,
  Wind,
} from "lucide-react";
import type { DeviceData } from "../../shared/model/data";
import { FeatureSwitch, SnapshotDialog } from "../../shared/ui/FeatureControls";
import { NoiseEngine } from "./NoiseEngine";
import {
  levelsFromPreset,
  NoiseCountdown,
  noisePresets,
  noiseSounds,
  noiseTimers,
  type NoiseLevels,
  type NoiseSoundId,
  type NoiseTimer,
  type WhiteNoiseSettings,
} from "../../shared/model/whiteNoise";

const icons = {
  rain: CloudRain,
  waves: Waves,
  fire: Flame,
  forest: Trees,
  cafe: Coffee,
  wind: Wind,
  storm: CloudLightning,
  stream: Waves,
  typing: Keyboard,
};
const clock = (seconds: number) =>
  `${Math.floor(seconds / 60)
    .toString()
    .padStart(2, "0")}:${(seconds % 60).toString().padStart(2, "0")}`;

export default function WhiteNoisePlayer({
  data,
  setData,
  notify,
}: {
  data: DeviceData;
  setData: Dispatch<SetStateAction<DeviceData>>;
  notify: (message: string) => void;
}) {
  const workspace = useWorkspace();
  const [liveSettings, setLiveSettings] = useState(data.features.whiteNoise);
  const settings = workspace.live ? liveSettings : data.features.whiteNoise;
  const reported = record(workspace.snapshot?.reported.whiteNoise);
  const devicePlaying = reported.playing === true;
  const mixTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const editPending = useRef(false);
  useEffect(() => {
    if (workspace.live && !editPending.current && !workspace.pending)
      setLiveSettings(data.features.whiteNoise);
  }, [data.features.whiteNoise, workspace.live, workspace.pending]);
  useEffect(
    () => () => {
      if (mixTimer.current) clearTimeout(mixTimer.current);
    },
    [],
  );
  const applyMix = (next: WhiteNoiseSettings, play = devicePlaying) =>
    workspace.action("noise.set", {
      playing: play && Object.values(next.levels).some((n) => n > 0),
      volume: next.master,
      timerMinutes: next.timerSeconds / 60,
      levels: noiseSounds.map((sound) => next.levels[sound.id]),
    });
  const id = useId();
  const engineRef = useRef<NoiseEngine | null>(null);
  const timerRef = useRef(new NoiseCountdown(settings.timerSeconds));
  const [localPlaying, setPlaying] = useState(false);
  const playing = workspace.live ? devicePlaying : localPlaying;
  const [starting, setStarting] = useState(false);
  const [mixerOpen, setMixerOpen] = useState(false);
  const [remaining, setRemaining] = useState<number>(settings.timerSeconds);
  const active = noiseSounds.filter(
    ({ id: soundId }) => settings.levels[soundId] > 0,
  );
  const preset = noisePresets.find((item) =>
    noiseSounds.every(
      ({ id: soundId }, index) =>
        settings.levels[soundId] === item.levels[index],
    ),
  );

  useEffect(() => {
    if (workspace.live) return;
    const engine = new NoiseEngine((isPlaying) => {
      if (isPlaying) timerRef.current.start(performance.now());
      else timerRef.current.pause(performance.now());
      if (isPlaying)
        engine.setStopAfter(
          timerRef.current.durationSeconds > 0
            ? timerRef.current.remaining(performance.now())
            : null,
        );
      setPlaying(isPlaying);
      setRemaining(timerRef.current.remaining(performance.now()));
    });
    engineRef.current = engine;
    return () => {
      engineRef.current = null;
      engine.dispose();
    };
  }, []);

  useEffect(() => {
    engineRef.current?.update(settings);
  }, [settings]);
  useEffect(() => {
    timerRef.current.reset(settings.timerSeconds);
    if (engineRef.current?.playing) {
      timerRef.current.start(performance.now());
      engineRef.current.setStopAfter(settings.timerSeconds || null);
    }
    setRemaining(settings.timerSeconds);
  }, [settings.timerSeconds]);

  useEffect(() => {
    if (workspace.live || !playing || !settings.timerSeconds) return;
    const tick = () => {
      const now = performance.now();
      setRemaining(timerRef.current.remaining(now));
      if (timerRef.current.expired(now)) {
        engineRef.current?.pause();
        notify("白噪音试听已按定时关闭");
      }
    };
    const interval = window.setInterval(tick, 250);
    return () => window.clearInterval(interval);
  }, [playing, settings.timerSeconds, notify]);

  const update = (changes: Partial<WhiteNoiseSettings>) => {
    if (workspace.live) {
      const next = { ...settings, ...changes };
      setLiveSettings(next);
      editPending.current = true;
      if (mixTimer.current) clearTimeout(mixTimer.current);
      mixTimer.current = setTimeout(() => {
        mixTimer.current = undefined;
        void applyMix(next).finally(() => {
          editPending.current = false;
        });
      }, 400);
      return;
    }
    setData((previous) => ({
      ...previous,
      features: {
        ...previous.features,
        whiteNoise: { ...previous.features.whiteNoise, ...changes },
      },
    }));
  };
  const updateLevels = (levels: NoiseLevels) => {
    if (!Object.values(levels).some((level) => level > 0))
      engineRef.current?.pause();
    update({ levels });
  };
  const changeLevel = (soundId: NoiseSoundId, level: number) =>
    updateLevels({ ...settings.levels, [soundId]: level });
  const changeTimer = (seconds: NoiseTimer) => {
    // Clicking the current duration intentionally starts that timer over.
    timerRef.current.reset(seconds);
    if (engineRef.current?.playing) {
      timerRef.current.start(performance.now());
      engineRef.current.setStopAfter(seconds || null);
    }
    setRemaining(seconds);
    update({ timerSeconds: seconds });
  };
  const togglePlayback = async () => {
    if (workspace.live) {
      if (workspace.pending || editPending.current) return;
      await applyMix(settings, !devicePlaying);
      return;
    }
    const engine = engineRef.current;
    if (!engine || starting) return;
    if (engine.playing) {
      engine.pause();
      return;
    }
    if (timerRef.current.expired(performance.now())) {
      timerRef.current.reset(settings.timerSeconds);
      setRemaining(settings.timerSeconds);
    }
    setStarting(true);
    try {
      await engine.play(settings);
    } catch (error) {
      if (engineRef.current === engine)
        notify(
          error instanceof Error
            ? error.message
            : "暂时无法试听，请再次点击播放。",
        );
    } finally {
      if (engineRef.current === engine) setStarting(false);
    }
  };

  return (
    <section
      className="home-noise"
      id="white-noise"
      aria-labelledby={`${id}-title`}
    >
      <div className="home-noise-heading">
        <span
          className={`home-noise-icon ${playing ? "is-playing" : ""}`}
          aria-hidden="true"
        >
          <Headphones size={18} strokeWidth={1.5} />
        </span>
        <div>
          <h3 id={`${id}-title`}>白噪音</h3>
          <p>
            {active.length
              ? active.map((sound) => sound.name).join(" · ")
              : "选一种喜欢的声音"}
          </p>
        </div>
        <button
          type="button"
          className="home-noise-play"
          disabled={
            !active.length ||
            starting ||
            (workspace.live &&
              (workspace.pending || !workspace.snapshot?.control.ready))
          }
          aria-label={
            workspace.live
              ? playing
                ? "暂停白噪音"
                : "开始白噪音"
              : playing
                ? "暂停白噪音试听"
                : "开始白噪音试听"
          }
          onClick={() => void togglePlayback()}
        >
          {starting ? (
            <LoaderCircle size={15} className="spinning" />
          ) : playing ? (
            <Pause size={14} fill="currentColor" />
          ) : (
            <Play size={14} fill="currentColor" />
          )}
        </button>
      </div>
      <div className="home-noise-presets" role="group" aria-label="白噪音预设">
        {noisePresets.map((item) => (
          <button
            type="button"
            key={item.id}
            disabled={workspace.live && workspace.pending}
            aria-pressed={preset?.id === item.id}
            onClick={() => updateLevels(levelsFromPreset(item.levels))}
          >
            {preset?.id === item.id && <Check size={10} />}
            {item.name}
          </button>
        ))}
        <button
          type="button"
          className="home-noise-mix-button"
          aria-label="调整白噪音混音"
          aria-haspopup="dialog"
          onClick={() => setMixerOpen(true)}
        >
          <SlidersHorizontal size={14} />
        </button>
      </div>
      <div className="home-noise-tools">
        <div className="home-noise-volume">
          <label htmlFor={`${id}-master`} aria-label="白噪音试听音量">
            <Volume2 size={14} />
          </label>
          <input
            id={`${id}-master`}
            aria-label="白噪音试听音量"
            type="range"
            min="0"
            max="100"
            disabled={workspace.live && workspace.pending}
            value={settings.master}
            onChange={(event) => update({ master: Number(event.target.value) })}
            style={
              { "--range-progress": `${settings.master}%` } as CSSProperties
            }
          />
          <output htmlFor={`${id}-master`}>{settings.master}%</output>
        </div>
        <label className="home-noise-timer">
          <Timer size={13} aria-hidden="true" />
          <select
            aria-label="白噪音定时关闭"
            disabled={workspace.live && workspace.pending}
            value={settings.timerSeconds}
            onChange={(event) =>
              changeTimer(Number(event.target.value) as NoiseTimer)
            }
          >
            {!noiseTimers.some(
              (item) => item.seconds === settings.timerSeconds,
            ) && (
              <option value={settings.timerSeconds}>
                {settings.timerSeconds / 60} 分钟
              </option>
            )}
            {noiseTimers.map(({ seconds, label }) => (
              <option key={seconds} value={seconds}>
                {label}
              </option>
            ))}
          </select>
          <ChevronDown size={11} aria-hidden="true" />
        </label>
      </div>
      <div className="home-noise-status">
        <span role="status">
          {starting
            ? "准备声音中……"
            : workspace.live
              ? workspace.pending
                ? "正在同步…"
                : devicePlaying
                  ? "正在设备上播放"
                  : "在设备上播放 · 离开网页继续"
              : playing
                ? settings.master === 0
                  ? "试听中 · 已静音"
                  : "正在本机试听"
                : "本机试听 · 离开首页停止"}
        </span>
        {settings.timerSeconds > 0 && playing && (
          <output aria-label="白噪音剩余时间">
            剩余{" "}
            {clock(
              workspace.live
                ? Number(reported.remainingSeconds || 0)
                : remaining,
            )}
          </output>
        )}
      </div>
      {mixerOpen && (
        <SnapshotDialog
          title="调一调喜欢的声音"
          note={
            workspace.live
              ? "混音在设备上播放，关闭网页后仍会继续。"
              : "声音在当前浏览器试听。"
          }
          close={() => setMixerOpen(false)}
        >
          <div className="home-noise-sounds">
            {noiseSounds.map((sound) => {
              const Icon = icons[sound.id];
              const level = settings.levels[sound.id];
              const enabled = level > 0;
              const sliderId = `${id}-${sound.id}`;
              return (
                <article
                  key={sound.id}
                  className={`home-noise-sound ${enabled ? "is-active" : ""}`}
                >
                  <div className="home-noise-sound-heading">
                    <Icon size={18} strokeWidth={1.5} />
                    <label htmlFor={sliderId}>{sound.name}</label>
                    <FeatureSwitch
                      label={`${sound.name}混音`}
                      checked={enabled}
                      disabled={workspace.live && workspace.pending}
                      onChange={() => changeLevel(sound.id, enabled ? 0 : 50)}
                    />
                  </div>
                  <div className="home-noise-sound-level">
                    <input
                      id={sliderId}
                      type="range"
                      min="5"
                      max="100"
                      value={enabled ? level : 50}
                      disabled={
                        !enabled || (workspace.live && workspace.pending)
                      }
                      onChange={(event) =>
                        changeLevel(sound.id, Number(event.target.value))
                      }
                      style={
                        {
                          "--range-progress": enabled
                            ? `${((level - 5) / 95) * 100}%`
                            : "0%",
                        } as CSSProperties
                      }
                    />
                    <output htmlFor={sliderId}>
                      {enabled ? `${level}%` : "未加入"}
                    </output>
                  </div>
                </article>
              );
            })}
          </div>
        </SnapshotDialog>
      )}
    </section>
  );
}
