import { useWorkspace } from "../../shared/state/workspaceContext";
import { Clock3, Mic } from "lucide-react";
import type { FeaturePageProps } from "../../shared/ui/featureTypes";
import WhiteNoisePlayer from "./WhiteNoisePlayer";
import { navigate } from "../../shared/navigation";

export default function HomeQuickControls({
  device,
  data,
  setData,
  notify,
}: FeaturePageProps) {
  const workspace = useWorkspace();
  const toggleMicrophone = () => {
    if (!device.online) return;
    setData((previous) => ({ ...previous, microphone: !previous.microphone }));
  };
  return (
    <div className="home-quick-controls" aria-label="首页快捷控制">
      <div className="home-quick-actions">
        <a
          href={`/devices/${device.id}/watchface`}
          onClick={(event) => {
            event.preventDefault();
            navigate(`/devices/${device.id}/watchface`);
          }}
        >
          <Clock3 size={15} />
          <span>表盘设置</span>
        </a>
        <button
          type="button"
          disabled={
            !device.online ||
            workspace.pending ||
            (workspace.live && !workspace.snapshot?.control.ready)
          }
          aria-label={data.microphone ? "关闭麦克风" : "开启麦克风"}
          aria-pressed={data.microphone}
          onClick={toggleMicrophone}
        >
          <Mic size={15} />
          <span>{data.microphone ? "麦克风开" : "麦克风关"}</span>
        </button>
      </div>
      <WhiteNoisePlayer data={data} setData={setData} notify={notify} />
    </div>
  );
}
