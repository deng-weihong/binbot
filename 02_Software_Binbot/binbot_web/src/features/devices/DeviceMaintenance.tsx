import { useWorkspace } from "../../shared/state/workspaceContext";
import { useEffect, useRef, useState } from "react";
import {
  Activity,
  ArrowUpRight,
  Check,
  CircleAlert,
  Cpu,
  Download,
  HardDrive,
  LoaderCircle,
  Package,
  RefreshCw,
  ShieldCheck,
  Upload,
  Wifi,
  X,
} from "lucide-react";
import { useConsole } from "../../shared/state/consoleContext";
import {
  createConsoleTransport,
  formatBytes,
  formatDate,
  jobLabels,
  jobPercent,
  matchingFirmware,
  profileLabels,
  terminalJob,
  type ComponentInfo,
  type DeviceJob,
  type Firmware,
} from "../../shared/api/consoleApi";
import { uid, type Device } from "../../shared/model/data";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";

const api = createConsoleTransport();
export default function DeviceMaintenance({
  device,
  name,
  notify,
}: {
  device: Device;
  name: string;
  notify: (message: string) => void;
}) {
  const { mode, refresh } = useConsole();
  const { snapshot: workspace } = useWorkspace();
  const system = workspace?.system || {};
  const [component, setComponent] = useState<ComponentInfo | null>(null),
    [slave, setSlave] = useState<ComponentInfo | null>(null);
  const [componentError, setComponentError] = useState(""),
    [slaveError, setSlaveError] = useState("");
  const [firmware, setFirmware] = useState<Firmware[]>([]),
    [jobs, setJobs] = useState<DeviceJob[]>([]);
  const [target, setTarget] = useState<"component-1" | "slave-1">(
      "component-1",
    ),
    [selectedId, setSelectedId] = useState("");
  const [busy, setBusy] = useState(""),
    [error, setError] = useState(""),
    [jobsError, setJobsError] = useState("");
  const [confirm, setConfirm] = useState(false),
    [confirmError, setConfirmError] = useState("");
  const [reportedAt, setReportedAt] = useState<string | undefined>();
  const [checkedAt, setCheckedAt] = useState<number>();
  const lifetime = useRef<AbortController | null>(null),
    loadRef = useRef<AbortController | null>(null);
  const upgradeRequest = useRef({ fingerprint: "", key: "" });
  const currentInfo = target === "slave-1" ? slave : component;
  const choices = firmware.filter((item) =>
    matchingFirmware(currentInfo, item),
  );
  const selected = choices.find((item) => item.firmware_id === selectedId);
  const activeJobs = jobs.some((job) => !terminalJob(job.status));
  const jobsRef = useRef(jobs);
  jobsRef.current = jobs;
  const canManage =
    mode === "live" &&
    device.online &&
    device.capabilities?.includes("component_ota_v1");

  async function load(fresh = false) {
    if (mode !== "live") return;
    loadRef.current?.abort();
    const controller = new AbortController();
    loadRef.current = controller;
    setBusy(fresh ? "probe" : "load");
    setError("");
    const reads = await Promise.allSettled([
      api.component(device.id, "component-1", fresh, controller.signal),
      api.component(device.id, "slave-1", fresh, controller.signal),
      api.firmware(controller.signal),
      api.jobs(device.id, controller.signal),
      api.state(device.id, controller.signal),
    ]);
    if (controller.signal.aborted || lifetime.current?.signal.aborted) return;
    const [one, two, catalog, history, snapshot] = reads;
    setComponent(one.status === "fulfilled" ? one.value : null);
    setComponentError(one.status === "rejected" ? messageOf(one.reason) : "");
    setSlave(two.status === "fulfilled" ? two.value : null);
    setSlaveError(two.status === "rejected" ? messageOf(two.reason) : "");
    if (catalog.status === "fulfilled") setFirmware(catalog.value);
    else setError(messageOf(catalog.reason));
    if (history.status === "fulfilled") {
      setJobs(history.value);
      setJobsError("");
    } else setJobsError(messageOf(history.reason));
    if (snapshot.status === "fulfilled")
      setReportedAt(snapshot.value.state?.updated_at);
    if (reads.some((read) => read.status === "fulfilled"))
      setCheckedAt(Date.now());
    setBusy("");
    if (fresh) {
      void refresh();
      notify(
        one.status === "fulfilled" || two.status === "fulfilled"
          ? "版本信息已重新读取。"
          : "暂未读取到版本，请查看连接状态。",
      );
    }
  }
  useEffect(() => {
    lifetime.current = new AbortController();
    void load();
    return () => {
      lifetime.current?.abort();
      loadRef.current?.abort();
    };
    // The page is recreated when switching devices.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [device.id, mode]);

  useEffect(() => {
    if (!activeJobs || mode !== "live") return;
    const controller = new AbortController();
    let timer: number | undefined;
    const poll = async () => {
      try {
        const result = await api.jobs(device.id, controller.signal);
        if (!controller.signal.aborted) {
          const finished = result.some(
            (job) =>
              terminalJob(job.status) &&
              jobsRef.current.some(
                (previous) =>
                  previous.job_id === job.job_id &&
                  !terminalJob(previous.status),
              ),
          );
          setJobs(result);
          setJobsError("");
          if (finished) {
            // Version refresh follows the page lifetime, since the polling
            // effect stops as soon as the last active job finishes.
            const signal = lifetime.current?.signal;
            void Promise.allSettled([
              api.component(device.id, "component-1", false, signal),
              api.component(device.id, "slave-1", false, signal),
            ]).then(([one, two]) => {
              if (signal?.aborted) return;
              if (one.status === "fulfilled") {
                setComponent(one.value);
                setComponentError("");
              }
              if (two.status === "fulfilled") {
                setSlave(two.value);
                setSlaveError("");
              }
              if (one.status === "fulfilled" || two.status === "fulfilled")
                setCheckedAt(Date.now());
            });
          }
        }
      } catch (reason) {
        if (!controller.signal.aborted) setJobsError(messageOf(reason));
      }
      if (!controller.signal.aborted)
        timer = window.setTimeout(() => void poll(), 2500);
    };
    void poll();
    return () => {
      controller.abort();
      window.clearTimeout(timer);
    };
  }, [activeJobs, device.id, mode]);

  async function upload(file?: File) {
    if (!file || !canManage || busy) return;
    setBusy("upload");
    setError("");
    try {
      const result = await api.uploadFirmware(file, lifetime.current?.signal);
      if (lifetime.current?.signal.aborted) return;
      setFirmware((previous) => [
        result,
        ...previous.filter((item) => item.firmware_id !== result.firmware_id),
      ]);
      setTarget(result.profile === "slave_mcu" ? "slave-1" : "component-1");
      setSelectedId(result.firmware_id);
      notify("固件已上传并校验，尚未开始升级。");
    } catch (reason) {
      if (!lifetime.current?.signal.aborted) setError(messageOf(reason));
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }

  async function upgrade() {
    if (!selected || !canManage || busy || activeJobs) return;
    setBusy("upgrade");
    setConfirmError("");
    try {
      const fingerprint = `${device.id}:${target}:${selected.firmware_id}`;
      if (
        upgradeRequest.current.fingerprint !== fingerprint ||
        !upgradeRequest.current.key
      )
        upgradeRequest.current = { fingerprint, key: uid() };
      const job = await api.startUpgrade(
        device.id,
        target,
        selected.firmware_id,
        lifetime.current?.signal,
        upgradeRequest.current.key,
      );
      if (lifetime.current?.signal.aborted) return;
      upgradeRequest.current.key = "";
      setJobs((previous) => [
        job,
        ...previous.filter((item) => item.job_id !== job.job_id),
      ]);
      setConfirm(false);
      notify("升级任务已提交，等待设备执行。");
    } catch (reason) {
      if (!lifetime.current?.signal.aborted) setConfirmError(messageOf(reason));
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }

  async function cancel(job: DeviceJob) {
    if (!canManage || busy) return;
    setBusy(job.job_id);
    setJobsError("");
    try {
      const result = await api.cancel(
        device.id,
        job.job_id,
        lifetime.current?.signal,
      );
      if (!lifetime.current?.signal.aborted) {
        setJobs((previous) =>
          previous.map((item) =>
            item.job_id === result.job_id ? result : item,
          ),
        );
        notify(
          result.status === "cancelled"
            ? "任务已取消。"
            : "取消请求已发送，等待设备确认。",
        );
      }
    } catch (reason) {
      if (!lifetime.current?.signal.aborted) setJobsError(messageOf(reason));
    } finally {
      if (!lifetime.current?.signal.aborted) setBusy("");
    }
  }

  function diagnostics() {
    const payload = {
      device_id: device.id,
      name,
      mode,
      exported_at: new Date().toISOString(),
      online: device.online,
      remote_ip: device.remoteIp || null,
      connected_at: device.connectedAt || null,
      last_seen_at: reportedAt || device.lastSeenAt || null,
      capabilities: device.capabilities || [],
      system,
      component,
      slave,
      jobs: jobs.map(({ job_id, status, component_id, kind, result }) => ({
        job_id,
        status,
        component_id,
        kind,
        result,
      })),
    };
    const url = URL.createObjectURL(
      new Blob([JSON.stringify(payload, null, 2)], {
        type: "application/json",
      }),
    );
    const link = document.createElement("a");
    link.href = url;
    link.download = `${device.id}-diagnostics.json`;
    link.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
    notify("设备诊断信息已导出。");
  }

  return (
    <div className="maintenance-page">
      <div className="maintenance-overview">
        <div>
          <span
            className={`maintenance-online ${device.online ? "" : "is-offline"}`}
          >
            <i className={`status-dot ${device.online ? "" : "offline"}`} />
            {mode === "demo"
              ? "示例设备"
              : device.online
                ? "设备在线"
                : "设备离线"}
          </span>
          <h2>{name}的运行状态</h2>
          <p>
            {checkedAt
              ? `最近读取 · ${formatDate(checkedAt / 1000)}`
              : mode === "demo"
                ? "真实运行数据将在设备接入后显示。"
                : "查看设备连接、组件版本与维护记录。"}
          </p>
        </div>
        <div className="maintenance-overview-actions">
          <button
            type="button"
            className="button secondary"
            onClick={diagnostics}
          >
            <Download size={15} />
            导出诊断
          </button>
          <button
            type="button"
            className="button primary"
            disabled={!canManage || !!busy}
            onClick={() => void load(true)}
          >
            <RefreshCw
              size={15}
              className={busy === "probe" ? "spinning" : ""}
            />
            {busy === "probe" ? "正在读取" : "刷新版本"}
          </button>
        </div>
      </div>
      <div className="maintenance-info-grid">
        <section className="card maintenance-card">
          <CardHeading
            icon={Cpu}
            title="设备与版本"
          />
          <dl className="maintenance-details">
            <InfoRow label="设备编号" value={device.id} />
            <InfoRow
              label="主机应用版本"
              value={String(system.app_version || "尚未上报")}
            />
            <InfoRow label="从机版本" value={slave?.version || "尚未读取"} />
            <InfoRow
              label="当前组件"
              value={
                component
                  ? profileLabels[component.profile] || "未知组件"
                  : "尚未读取"
              }
            />
            <InfoRow
              label="组件版本"
              value={component?.version || "尚未读取"}
            />
          </dl>
          {(componentError || slaveError) && (
            <div className="maintenance-read-notes">
              {componentError && (
                <p>
                  <Package size={12} />
                  {componentError}
                </p>
              )}
              {slaveError && slaveError !== componentError && (
                <p>
                  <Cpu size={12} />
                  {slaveError}
                </p>
              )}
            </div>
          )}
          <p className="maintenance-footnote">
            刷新版本只读取信息，不会触发升级。
          </p>
        </section>
        <section className="card maintenance-card">
          <CardHeading icon={Wifi} title="运行与连接" />
          <dl className="maintenance-details">
            <InfoRow label="位置" value={device.location} />
            <InfoRow label="网络地址" value={device.remoteIp || "尚未上报"} />
            <InfoRow label="最近连接" value={formatDate(device.connectedAt)} />
            <InfoRow
              label="最后上报"
              value={formatDate(reportedAt || device.lastSeenAt)}
            />
            <InfoRow
              label="App 版本"
              value={String(system.app_version || "尚未上报")}
            />
            <InfoRow
              label="系统"
              value={
                system.kernel
                  ? `${system.architecture} · ${system.kernel}`
                  : "尚未上报"
              }
            />
            <InfoRow
              label="运行时长"
              value={
                typeof system.uptime_seconds === "number"
                  ? `${Math.floor(system.uptime_seconds / 3600)} 小时 ${Math.floor(system.uptime_seconds / 60) % 60} 分钟`
                  : "尚未上报"
              }
            />
            <InfoRow
              label="空闲内存"
              value={formatBytes(
                typeof system.memory_free === "number"
                  ? system.memory_free
                  : undefined,
              )}
            />
            <InfoRow
              label="可用存储"
              value={formatBytes(
                typeof system.storage_free === "number"
                  ? system.storage_free
                  : undefined,
              )}
            />
          </dl>
          <p className="maintenance-footnote">
            <HardDrive size={12} />
            运行信息以设备上报为准。
          </p>
        </section>
      </div>
      <section className="card maintenance-upgrade">
        <CardHeading
          icon={ShieldCheck}
          title="组件与从机升级"
        />
        <div className="maintenance-upgrade-layout">
          <div className="maintenance-upgrade-intro">
            <span className="maintenance-upgrade-symbol">
              <Package size={29} strokeWidth={1.3} />
            </span>
            <h3>固件升级</h3>
            <p>选择与当前组件匹配的固件，升级进度会在下方持续更新。</p>
            <span className="quiet-chip">
              <ShieldCheck size={12} />
              文件校验后再应用
            </span>
          </div>
          <div className="maintenance-upgrade-form">
            <div
              className="maintenance-targets"
              role="group"
              aria-label="升级目标"
            >
              <button
                type="button"
                aria-pressed={target === "component-1"}
                className={target === "component-1" ? "selected" : ""}
                onClick={() => {
                  setTarget("component-1");
                  setSelectedId("");
                }}
              >
                <Package size={16} />
                扩展组件
              </button>
              <button
                type="button"
                aria-pressed={target === "slave-1"}
                className={target === "slave-1" ? "selected" : ""}
                onClick={() => {
                  setTarget("slave-1");
                  setSelectedId("");
                }}
              >
                <Cpu size={16} />
                从机
              </button>
            </div>
            <label className="portal-field">
              <span>
                选择固件{" "}
                <small>
                  {currentInfo
                    ? `${profileLabels[currentInfo.profile] || currentInfo.profile} · 当前 ${currentInfo.version}`
                    : "请先读取版本"}
                </small>
              </span>
              <select
                aria-label="选择升级固件"
                value={selected?.firmware_id || ""}
                disabled={!canManage || !currentInfo || !!busy || activeJobs}
                onChange={(event) => setSelectedId(event.target.value)}
              >
                <option value="">
                  {choices.length ? "选择匹配的固件版本" : "暂无匹配固件"}
                </option>
                {choices.map((item) => (
                  <option key={item.firmware_id} value={item.firmware_id}>
                    {item.version} · {item.project} ·{" "}
                    {formatBytes(item.size_bytes)}
                  </option>
                ))}
              </select>
            </label>
            <label
              className={`maintenance-file-choice ${!canManage || busy ? "is-disabled" : ""}`}
            >
              <Upload size={17} />
              <span>
                <strong>
                  {busy === "upload" ? "正在上传并校验固件" : "上传固件文件"}
                </strong>
                <small>最大 16 MB · 上传后不会自动升级</small>
              </span>
              <ArrowUpRight size={17} />
              <input
                type="file"
                aria-label="上传组件或从机固件"
                accept=".bin,.ota,application/octet-stream"
                disabled={!canManage || !!busy}
                onChange={(event) => {
                  void upload(event.target.files?.[0]);
                  event.target.value = "";
                }}
              />
            </label>
            {error && (
              <p className="portal-alert" role="alert">
                <CircleAlert size={15} />
                {error}
              </p>
            )}
            <div className="maintenance-upgrade-actions">
              <span>
                {mode === "demo"
                  ? "登录真实设备后可管理升级。"
                  : activeJobs
                    ? "当前设备有任务正在执行。"
                    : !device.online
                      ? "设备连接恢复后可升级。"
                      : "选择固件后，确认开始升级。"}
              </span>
              <button
                className="button primary"
                disabled={!selected || !canManage || !!busy || activeJobs}
                onClick={() => {
                  setConfirmError("");
                  setConfirm(true);
                }}
              >
                开始升级 <ArrowUpRight size={15} />
              </button>
            </div>
          </div>
        </div>
      </section>
      <section className="card maintenance-history">
        <div className="maintenance-history-heading">
          <CardHeading
            icon={Activity}
            title="最近维护记录"
          />
          <button
            type="button"
            className="icon-button"
            aria-label="刷新维护记录"
            disabled={mode !== "live" || !!busy}
            onClick={() => void load()}
          >
            <RefreshCw
              size={16}
              className={busy === "load" ? "spinning" : ""}
            />
          </button>
        </div>
        {jobsError && (
          <p className="portal-alert" role="alert">
            {jobsError}
          </p>
        )}
        {!jobs.length ? (
          <div className="maintenance-empty">
            <span>
              <Check size={20} />
            </span>
            <div>
              <strong>暂时没有维护记录</strong>
              <p>
                {mode === "demo"
                  ? "真实设备的升级与图片推送记录会显示在这里。"
                  : "升级与组件图片推送的执行结果会显示在这里。"}
              </p>
            </div>
          </div>
        ) : (
          <div className="maintenance-jobs">
            {jobs.slice(0, 10).map((job) => (
              <article
                className={`maintenance-job ${terminalJob(job.status) ? "is-terminal" : ""}`}
                key={job.job_id}
              >
                <span
                  className={`maintenance-job-icon ${job.status === "succeeded" ? "is-success" : job.status === "failed" ? "is-error" : ""}`}
                >
                  {job.status === "succeeded" ? (
                    <Check size={18} />
                  ) : job.status === "failed" ? (
                    <CircleAlert size={18} />
                  ) : terminalJob(job.status) ? (
                    <X size={18} />
                  ) : (
                    <LoaderCircle size={18} className="spinning" />
                  )}
                </span>
                <div className="maintenance-job-copy">
                  <strong>
                    {job.kind === "file"
                      ? job.source_name || "组件图片推送"
                      : `${job.component_id === "slave-1" ? "从机" : "组件"}升级${job.target?.version ? ` · ${job.target.version}` : ""}`}
                  </strong>
                  <small>
                    {formatDate(job.created_at)}
                    {job.status === "failed"
                      ? " · 设备未确认成功，可检查连接后重新操作。"
                      : ""}
                  </small>
                  {!terminalJob(job.status) && (
                    <progress
                      max="100"
                      value={jobPercent(job)}
                      aria-label={`${job.kind === "file" ? "图片推送" : "固件升级"}进度`}
                    />
                  )}
                </div>
                <span className={`maintenance-job-status ${job.status}`}>
                  {jobLabels[job.status] || "处理中"}
                  {!terminalJob(job.status) && (
                    <small>{jobPercent(job)}%</small>
                  )}
                </span>
                {!terminalJob(job.status) &&
                  ![
                    "activating",
                    "verifying",
                    "reconciling",
                    "cancel_requested",
                  ].includes(job.status) && (
                    <button
                      className="maintenance-cancel"
                      type="button"
                      disabled={!canManage || !!busy}
                      onClick={() => void cancel(job)}
                    >
                      取消
                    </button>
                  )}
              </article>
            ))}
          </div>
        )}
      </section>
      {confirm && selected && (
        <SnapshotDialog
          title="确认开始升级？"
          close={() => {
            if (!busy) setConfirm(false);
          }}
          closeLabel="关闭升级确认"
          note="升级过程中，请保持设备供电和网络连接。"
        >
          <dl className="maintenance-details">
            <InfoRow label="目标设备" value={`${name} · ${device.id}`} />
            <InfoRow
              label="升级对象"
              value={
                target === "slave-1"
                  ? "从机"
                  : profileLabels[currentInfo?.profile || ""] || "当前组件"
              }
            />
            <InfoRow
              label="当前版本"
              value={currentInfo?.version || "尚未读取"}
            />
            <InfoRow label="目标版本" value={selected.version} />
            <InfoRow
              label="固件大小"
              value={formatBytes(selected.size_bytes)}
            />
          </dl>
          {confirmError && (
            <p className="portal-alert" role="alert">
              {confirmError}
            </p>
          )}
          <div className="dialog-actions">
            <button
              className="button secondary"
              type="button"
              disabled={!!busy}
              onClick={() => setConfirm(false)}
            >
              暂不升级
            </button>
            <button
              className="button primary"
              type="button"
              disabled={!!busy || !canManage || activeJobs}
              onClick={() => void upgrade()}
            >
              {busy === "upgrade" ? (
                <LoaderCircle size={15} className="spinning" />
              ) : (
                <ArrowUpRight size={15} />
              )}
              确认升级
            </button>
          </div>
        </SnapshotDialog>
      )}
    </div>
  );
}
function messageOf(reason: unknown) {
  return reason instanceof Error ? reason.message : "暂时无法读取设备信息。";
}
function InfoRow({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <dt>{label}</dt>
      <dd>{value}</dd>
    </div>
  );
}
function CardHeading({
  icon: Icon,
  title,
}: {
  icon: typeof Cpu;
  title: string;
}) {
  return (
    <div className="maintenance-card-heading">
      <span className="maintenance-heading-icon">
        <Icon size={19} strokeWidth={1.5} />
      </span>
      <h3>{title}</h3>
    </div>
  );
}
