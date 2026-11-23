import { useWorkspace } from "../state/workspaceContext";
import {
  useEffect,
  useId,
  useRef,
  type CSSProperties,
  type ReactNode,
} from "react";
import { ArrowUpRight, Check, RotateCcw, X } from "lucide-react";

export function FeatureSwitch({
  checked,
  onChange,
  label,
  disabled,
}: {
  checked: boolean;
  onChange: () => void;
  label: string;
  disabled?: boolean;
}) {
  return (
    <button
      type="button"
      className={`toggle ${checked ? "checked" : ""}`}
      role="switch"
      aria-checked={checked}
      aria-label={label}
      disabled={disabled}
      onClick={onChange}
    >
      <span>{checked && <Check size={10} />}</span>
    </button>
  );
}

export function FeatureRange({
  label,
  value,
  onChange,
  disabled,
  min = 0,
}: {
  label: string;
  value: number;
  onChange: (value: number) => void;
  disabled?: boolean;
  min?: number;
}) {
  const id = useId();
  return (
    <div className="slider-row feature-range">
      <label htmlFor={id}>{label}</label>
      <input
        id={id}
        type="range"
        min={min}
        max="100"
        value={value}
        onChange={(event) => onChange(Number(event.target.value))}
        disabled={disabled}
        style={
          {
            "--range-progress": `${((value - min) / (100 - min)) * 100}%`,
          } as CSSProperties
        }
      />
      <output htmlFor={id}>{value}%</output>
    </div>
  );
}

export function FeatureSave({
  online,
  dirty,
  reset,
  disabled,
  unavailable,
}: {
  online: boolean;
  dirty: boolean;
  reset: () => void;
  disabled?: boolean;
  unavailable?: string;
}) {
  const workspace = useWorkspace();
  return (
    <div className="feature-save">
      <p>
        {!online
          ? "设备离线，连接后可应用。"
          : unavailable || (dirty ? "有尚未保存的调整" : "当前设置已保存")}
        <small>
          {workspace.live
            ? workspace.pending
              ? "正在等待设备确认…"
              : "与当前设备同步"
            : "演示模式 · 设置仅保存在当前浏览器"}
        </small>
      </p>
      <div>
        <button
          className="button secondary"
          type="button"
          disabled={!dirty}
          onClick={reset}
        >
          <RotateCcw size={14} />
          还原
        </button>
        <button
          className="button primary"
          type="submit"
          disabled={
            !online ||
            disabled ||
            !dirty ||
            workspace.pending ||
            (workspace.live && !workspace.snapshot?.control.ready)
          }
        >
          保存设置
          <ArrowUpRight size={15} />
        </button>
      </div>
    </div>
  );
}

export function SnapshotDialog({
  title,
  children,
  close,
  closeLabel = "关闭预览",
  note = "这是网页预览，不会触发真实设备。",
}: {
  title: string;
  children: ReactNode;
  close: () => void;
  closeLabel?: string;
  note?: string;
}) {
  const dialogRef = useRef<HTMLDialogElement>(null);
  const titleId = useId();
  useEffect(() => {
    if (!dialogRef.current?.open) dialogRef.current?.showModal();
  }, []);
  return (
    <dialog
      className="feature-dialog"
      ref={dialogRef}
      aria-labelledby={titleId}
      onCancel={(event) => {
        event.preventDefault();
        close();
      }}
      onClick={(event) => {
        if (event.target === event.currentTarget) close();
      }}
    >
      <div className="feature-dialog-body">
        <header>
          <h2 id={titleId}>{title}</h2>
          <button
            className="icon-button subtle"
            type="button"
            onClick={close}
            aria-label={closeLabel}
          >
            <X size={20} />
          </button>
        </header>
        {children}
        <p className="feature-note">{note}</p>
      </div>
    </dialog>
  );
}
