import { formatBytes, jobLabels } from "../../shared/api/consoleApi";

const imageLabels: Record<string, string> = {
  preparing: "等待设备处理图片",
  downloading: "设备正在下载图片",
  transferring: "正在写入组件",
  validating: "正在校验图片",
  activating: "正在应用图片",
  verifying: "正在确认组件显示",
  succeeded: "组件已更新图片",
  failed: "组件图片更新失败",
};

export function imageTransferLabel(status: string) {
  return imageLabels[status] || jobLabels[status] || "等待组件处理";
}

export function imageTransferPercent(
  status: string,
  written: unknown,
  total: unknown,
) {
  if (status === "succeeded") return 100;
  if (
    typeof written !== "number" ||
    typeof total !== "number" ||
    total <= 0 ||
    !Number.isFinite(written) ||
    !Number.isFinite(total)
  )
    return undefined;
  return Math.min(99, Math.max(0, Math.floor((written * 100) / total)));
}

export function ImageTransferProgress({
  label,
  name,
  status,
  percent,
  detail,
  written,
  total,
}: {
  label: string;
  name?: string;
  status?: string;
  percent?: number;
  detail?: string;
  written?: number;
  total?: number;
}) {
  const success = status === "succeeded";
  const failure = status === "failed" || status === "cancelled";
  return (
    <div
      className={`gallery-push-result image-transfer-progress ${success ? "is-success" : failure ? "is-error" : ""}`}
      role="status"
    >
      <div>
        <span>{label}</span>
        {percent !== undefined && <strong>{percent}%</strong>}
      </div>
      {(!failure || percent !== undefined) && (
        <progress max={100} value={percent} aria-label="图片更新进度" />
      )}
      <small>
        {name}
        {written !== undefined && total !== undefined && total > 0 && (
          <>
            {" "}
            · {formatBytes(written)} / {formatBytes(total)}
          </>
        )}
        {detail && <span className="image-transfer-detail">{detail}</span>}
      </small>
    </div>
  );
}
