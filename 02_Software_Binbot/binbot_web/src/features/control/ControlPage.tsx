import { useState, type FormEvent } from "react";
import { ArrowUpRight, Mic, Monitor, Volume2 } from "lucide-react";
import { FeatureRange, FeatureSwitch } from "../../shared/ui/FeatureControls";
import { useConsole } from "../../shared/state/consoleContext";
import type { FeaturePageProps } from "../../shared/ui/featureTypes";
import LiveControlPage from "./LiveControlPage";

export default function ControlPage({
  device,
  data,
  setData,
  notify,
}: FeaturePageProps) {
  const { mode } = useConsole();
  const preview = mode === "demo";
  const [brightness, setBrightness] = useState(data.brightness);
  const brightnessDirty = brightness !== data.brightness;
  if (!preview) return <LiveControlPage key={device.id} device={device} notify={notify} />;
  const applyScreen = (event: FormEvent) => {
    event.preventDefault();
    if (!preview || !device.online || !brightnessDirty) return;
    setData((previous) => ({ ...previous, brightness }));
    notify("亮度预览已保存到当前浏览器。");
  };

  return (
    <div className="basic-controls feature-page">
      <form className="card basic-screen-card" onSubmit={applyScreen}>
        <header className="basic-card-heading">
          <span className="basic-heading-icon">
            <Monitor size={18} strokeWidth={1.5} />
          </span>
          <h2>屏幕</h2>
          <span className="basic-card-caption">设备主屏 · 待接入</span>
        </header>
        <div className="basic-setting-row basic-screen-summary">
          <div>
            <strong>屏幕亮度</strong>
            <small>目前请在设备上调整，后台远程设置待接入。</small>
          </div>
          {!preview && <span className="quiet-chip">尚未读取</span>}
        </div>
        {preview && (
          <fieldset disabled={!device.online}>
            <legend className="sr-only">主屏亮度预览</legend>
            <FeatureRange
              label="亮度预览"
              min={1}
              value={brightness}
              onChange={setBrightness}
            />
            <footer className="basic-save-row">
              <span>
                {brightnessDirty ? "有待保存的预览" : "仅保存在当前浏览器"}
              </span>
              <button
                type="submit"
                className="button primary"
                disabled={!brightnessDirty}
              >
                保存预览
                <ArrowUpRight size={14} />
              </button>
            </footer>
          </fieldset>
        )}
        <p className="basic-local-note">
          {preview
            ? "示例亮度仅用于界面预览，不会调整设备。表盘请在「表盘设置」中选择。"
            : "后台暂不能读取或调整主屏亮度，表盘请在「表盘设置」中查看。"}
        </p>
      </form>
      <section className="card basic-sound-card">
        <header className="basic-card-heading">
          <span className="basic-heading-icon mint">
            <Volume2 size={18} strokeWidth={1.5} />
          </span>
          <h2>声音与语音</h2>
          <span className="basic-card-caption">日常偏好</span>
        </header>
        <fieldset disabled={!device.online}>
          <legend className="sr-only">声音与语音设置</legend>
          <div className="basic-volume">
            <FeatureRange
              label="设备音量"
              value={data.volume}
              onChange={(volume) =>
                setData((previous) => ({ ...previous, volume }))
              }
            />
          </div>
          <div className="basic-setting-row basic-microphone">
            <div>
              <strong>
                <Mic size={14} strokeWidth={1.5} />
                麦克风
              </strong>
              <small>
                {data.microphone ? "语音输入已开启" : "语音输入已关闭"}
              </small>
            </div>
            <FeatureSwitch
              label="麦克风"
              checked={data.microphone}
              onChange={() => {
                if (!device.online) return;
                setData((previous) => ({
                  ...previous,
                  microphone: !previous.microphone,
                }));
              }}
            />
          </div>
        </fieldset>
        <p className="basic-local-note">
          演示设置仅保存在当前浏览器，设备控制和语音服务待接入。
        </p>
      </section>
    </div>
  );
}
