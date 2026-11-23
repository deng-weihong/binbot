import { useWorkspace } from "../../shared/state/workspaceContext";
import { useRef, useState, type Dispatch, type SetStateAction } from "react";
import { CalendarDays, Check, CloudUpload, Plus, Trash2 } from "lucide-react";
import { uid, type DeviceData } from "../../shared/model/data";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";
import { agendaWeek, demoDate, schedulesForDate } from "../../shared/model/schedules";

export default function HomeAgendaCard({
  data,
  setData,
  notify,
}: {
  data: DeviceData;
  setData: Dispatch<SetStateAction<DeviceData>>;
  notify: (message: string) => void;
}) {
  const workspace = useWorkspace();
  const timezone = String(workspace.snapshot?.data.timezone || "Asia/Shanghai");
  const today = workspace.live
    ? new Intl.DateTimeFormat("en-CA", {
        timeZone: timezone,
        year: "numeric",
        month: "2-digit",
        day: "2-digit",
      }).format(new Date())
    : demoDate;
  const center = new Date(today + "T12:00:00Z");
  const monday = new Date(center);
  monday.setUTCDate(center.getUTCDate() - ((center.getUTCDay() + 6) % 7));
  const week = workspace.live
    ? Array.from({ length: 7 }, (_, i) => {
        const d = new Date(monday);
        d.setUTCDate(d.getUTCDate() + i);
        return {
          date: d.toISOString().slice(0, 10),
          day: d.getUTCDate(),
          weekday: ["一", "二", "三", "四", "五", "六", "日"][i],
        };
      })
    : agendaWeek;
  const [selectedDate, setDate] = useState(today);
  const date = week.some((item) => item.date === selectedDate)
    ? selectedDate
    : today;
  const [adding, setAdding] = useState(false);
  const [name, setName] = useState("");
  const [time, setTime] = useState("15:00");
  const [note, setNote] = useState("");
  const [formError, setFormError] = useState("");
  const [saving, setSaving] = useState(false);
  const scheduleId = useRef("");
  const calendarSync = workspace.snapshot?.calendar_sync;
  const schedules = schedulesForDate(
    workspace.live ? workspace.snapshot?.calendar || [] : data.schedules,
    date,
  );
  const day = week.find((item) => item.date === date) || week[0];
  const title =
    date === today
      ? "今天的日程"
      : `${Number(date.slice(5, 7))} 月 ${day.day} 日的日程`;

  function openForm() {
    setName("");
    setTime("15:00");
    setNote("");
    setFormError("");
    scheduleId.current = uid();
    setAdding(true);
  }

  return (
    <section
      className="card agenda-card home-agenda"
      aria-labelledby="home-agenda-title"
    >
      <header className="card-heading">
        <h2 id="home-agenda-title">
          {title}
          <span className="count-badge" aria-label="未完成日程数">
            {schedules.filter((item) => !item.done).length}
          </span>
        </h2>
        <button
          type="button"
          className="button secondary agenda-add"
          onClick={openForm}
          aria-label="添加日程"
        >
          <Plus size={15} /> 添加
        </button>
      </header>
      <div className="home-schedule-panel" id="home-schedules">
        <div className="week-strip" role="group" aria-label="本周日程">
          {week.map((item) => (
            <button
              type="button"
              key={item.date}
              className={date === item.date ? "selected" : ""}
              onClick={() => setDate(item.date)}
              aria-label={`${item.date}，星期${item.weekday}`}
              aria-pressed={date === item.date}
            >
              <span>{item.weekday}</span>
              <strong>{item.day}</strong>
              {item.date === today && <i />}
            </button>
          ))}
        </div>
        <div className="agenda-list" aria-label={title}>
          {schedules.map((schedule) => (
            <div
              className={`agenda-item ${schedule.done ? "done" : ""}`}
              key={schedule.id}
            >
              <time dateTime={`${date}T${schedule.time}`}>{schedule.time}</time>
              <div>
                <strong title={schedule.name}>{schedule.name}</strong>
                {schedule.id.startsWith("feishu:") && <small>飞书日历</small>}
                {workspace.live && !schedule.id.startsWith("feishu:") && (
                  <small>设备日程</small>
                )}
                {schedule.note && <p title={schedule.note}>{schedule.note}</p>}
              </div>
              {workspace.live &&
                calendarSync?.enabled &&
                !schedule.id.startsWith("feishu:") &&
                !schedule.done && (
                  <button
                    type="button"
                    className="icon-button agenda-delete"
                    disabled={workspace.pending}
                    aria-label={`同步到飞书：${schedule.name}`}
                    title="同步到飞书"
                    onClick={() =>
                      void workspace
                        .saveSchedule(schedule)
                        .catch((reason) =>
                          notify(
                            reason instanceof Error
                              ? reason.message
                              : "飞书同步失败，请重试。",
                          ),
                        )
                    }
                  >
                    <CloudUpload size={14} />
                  </button>
                )}
              <button
                type="button"
                className={`task-check ${schedule.done ? "checked" : ""}`}
                aria-label={`${schedule.done ? "恢复" : "完成"}日程：${schedule.name}`}
                disabled={schedule.id.startsWith("feishu:")}
                aria-pressed={schedule.done}
                onClick={() =>
                  setData((previous) => ({
                    ...previous,
                    schedules: previous.schedules.map((item) =>
                      item.id === schedule.id
                        ? { ...item, done: !item.done }
                        : item,
                    ),
                  }))
                }
              >
                {schedule.done && <Check size={11} />}
              </button>
              <button
                type="button"
                className="icon-button agenda-delete"
                disabled={schedule.id.startsWith("feishu:")}
                aria-label={`删除日程：${schedule.name}`}
                onClick={() => {
                  setData((previous) => ({
                    ...previous,
                    schedules: previous.schedules.filter(
                      (item) => item.id !== schedule.id,
                    ),
                  }));
                  notify("日程已删除");
                }}
              >
                <Trash2 size={13} />
              </button>
            </div>
          ))}
          {!schedules.length && (
            <div className="agenda-empty">
              <CalendarDays size={24} strokeWidth={1.4} />
              <strong>这一天没有日程</strong>
            </div>
          )}
        </div>
        {workspace.live && calendarSync?.enabled && (
          <p className="feature-note" role="status">
            {!calendarSync.authorized
              ? "飞书需要重新授权，请在设备设置中连接账号。"
              : calendarSync.error
                ? `飞书同步暂不可用：${calendarSync.error}`
                : "飞书日历自动同步 · 新日程同时保存到飞书"}
          </p>
        )}
      </div>
      {adding && (
        <SnapshotDialog
          title="添加一条日程"
          closeLabel="关闭日程表单"
          close={() => {
            if (!saving) setAdding(false);
          }}
          note={
            workspace.live
              ? calendarSync?.enabled
                ? "保存到当前设备绑定的飞书日历，自动同步至设备；后续修改请在飞书中操作。"
                : `日程属于${data.deviceName}，自动同步至设备。连接飞书后可将日程同步到飞书。`
              : "演示日程保存在当前浏览器。"
          }
        >
          <form
            onSubmit={async (event) => {
              event.preventDefault();
              if (!name.trim() || !time || saving) return;
              const schedule = {
                id: scheduleId.current || uid(),
                name: name.trim(),
                date,
                time,
                note: note.trim(),
                enabled: true,
                done: false,
              };
              if (workspace.live) {
                setSaving(true);
                setFormError("");
                try {
                  await workspace.saveSchedule(schedule);
                  setAdding(false);
                } catch (reason) {
                  setFormError(
                    reason instanceof Error
                      ? reason.message
                      : "日程未能保存，请重试。",
                  );
                } finally {
                  setSaving(false);
                }
                return;
              }
              setData((previous) => ({
                ...previous,
                schedules: [...previous.schedules, schedule],
              }));
              setAdding(false);
              notify("演示日程已保存");
            }}
          >
            <div className="form-stack">
              <label className="field-label">
                做点什么
                <input
                  autoFocus
                  required
                  maxLength={60}
                  disabled={saving}
                  value={name}
                  onChange={(event) => setName(event.target.value)}
                  placeholder="比如，读几页书，起来走走"
                />
              </label>
              <div className="form-columns">
                <div className="field-label">
                  日期
                  <div className="static-input">
                    <CalendarDays size={16} /> {Number(date.slice(5, 7))} 月{" "}
                    {day.day} 日 · 星期
                    {day.weekday}
                  </div>
                </div>
                <label className="field-label">
                  时间
                  <input
                    required
                    type="time"
                    disabled={saving}
                    value={time}
                    onChange={(event) => setTime(event.target.value)}
                  />
                </label>
              </div>
              <label className="field-label">
                补充一句话 <span className="optional">可选</span>
                <textarea
                  rows={2}
                  maxLength={160}
                  disabled={saving}
                  value={note}
                  onChange={(event) => setNote(event.target.value)}
                  placeholder="还有什么需要记住的？"
                />
              </label>
            </div>
            <div className="dialog-actions">
              {formError && (
                <p className="feature-note" role="alert">
                  {formError}
                </p>
              )}
              <button
                type="button"
                className="button secondary"
                disabled={saving}
                onClick={() => setAdding(false)}
              >
                取消
              </button>
              <button
                type="submit"
                className="button primary"
                disabled={!name.trim() || !time || saving || workspace.pending}
              >
                {saving ? "正在同步" : "保存日程"} <Check size={15} />
              </button>
            </div>
          </form>
        </SnapshotDialog>
      )}
    </section>
  );
}
