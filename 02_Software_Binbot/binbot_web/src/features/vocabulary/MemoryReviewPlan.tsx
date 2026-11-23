import { useId, useMemo, type ReactNode } from "react";
import { CalendarClock, ChevronDown, Clock3 } from "lucide-react";
import {
  formatReviewTime,
  reviewIntervals,
  reviewStatus,
  dailyLearningPlan,
} from "../../shared/model/memorySchedule";
import type { BookWord, Wordbook } from "../../shared/model/wordbooks";
import { useWorkspace } from "../../shared/state/workspaceContext";

export function MemoryReviewPlan({
  book,
  now,
  dailyNewLimit = 20,
  timezone = "Asia/Shanghai",
  changeDailyLimit,
  action,
}: {
  book: Wordbook;
  now: number;
  dailyNewLimit?: number;
  timezone?: string;
  changeDailyLimit?: (limit: number) => void;
  action?: ReactNode;
}) {
  const titleId = useId();
  const { live } = useWorkspace();
  const title = live ? "自适应复习计划" : "艾宾浩斯复习计划";
  const summary = useMemo(
    () => dailyLearningPlan(book.words, dailyNewLimit, timezone, now),
    [book.words, dailyNewLimit, timezone, now],
  );
  return (
    <section className="card memory-review-plan" aria-label={title}>
      <header className="card-heading memory-plan-header">
        <h2><CalendarClock size={18} strokeWidth={1.5} />{title}</h2>
        {action}
      </header>
      <div className="memory-plan-copy">
        <dl className="memory-plan-counts">
          <div className={summary.due ? "has-due" : ""}>
            <dt>到期待复习</dt>
            <dd>
              {summary.due}
              <small>词</small>
            </dd>
          </div>
          <div>
            <dt>新词待学</dt>
            <dd>
              {summary.fresh}
              <small>词</small>
            </dd>
          </div>
          <div>
            <dt>等待下次</dt>
            <dd>
              {summary.scheduled}
              <small>词</small>
            </dd>
          </div>
        </dl>
        <div className="daily-learning-plan">
          <label>
            每日新词
            <select
              aria-label="每日新词量"
              value={dailyNewLimit}
              disabled={!changeDailyLimit}
              onChange={(event) =>
                changeDailyLimit?.(Number(event.target.value))
              }
            >
              {[...new Set([10, 20, 30, 50, 100, dailyNewLimit])]
                .sort((a, b) => a - b)
                .map((limit) => (
                  <option key={limit} value={limit}>
                    {limit} 词
                  </option>
                ))}
            </select>
          </label>
          <span>
            今日新学 <strong>{summary.learnedToday}</strong> / {dailyNewLimit}{" "}
            词 · 还可学 {summary.remainingNew} 词
          </span>
          <span>
            累计学习 {summary.learned.toLocaleString("zh-CN")} /{" "}
            {book.words.length.toLocaleString("zh-CN")} 词
          </span>
          <progress
            aria-label="词本累计学习进度"
            value={summary.learned}
            max={book.words.length || 1}
          />
          <small>复习积压时少学新词，按设备时区安排。</small>
        </div>
        <p className="memory-next-session" role="status">
          <Clock3 size={13} />
          {summary.due
            ? "有单词等待复习"
            : summary.nextReviewAt
              ? `下一次：${formatReviewTime(summary.nextReviewAt, now)}`
              : summary.fresh
                ? "新词首次学习后，会自动安排复习时间。"
                : "添加单词后可开始学习。"}
        </p>
      </div>
      <div className="memory-plan-visual">
        <div className="memory-curve-heading">
          <span>遗忘曲线</span>
          <small>原理示意</small>
        </div>
        <svg viewBox="0 0 360 126" role="img" aria-labelledby={titleId}>
          <title id={titleId}>
            间隔复习原理示意：不复习时记忆逐渐减弱，重复复习后间隔逐步拉长；曲线不是个人记忆测量。
          </title>
          <path
            d="M 12 22 H 348 M 12 62 H 348 M 12 102 H 348"
            className="memory-curve-grid"
          />
          <path
            d="M 12 22 C 25 44, 33 70, 65 86 S 200 104, 348 108"
            className="memory-curve-forget"
          />
          <path
            d="M 12 22 Q 24 47 43 74 L 43 22 Q 67 43 94 68 L 94 22 Q 128 41 172 61 L 172 22 Q 217 38 270 52 L 270 22 Q 311 33 348 40"
            className="memory-curve-review"
          />
          {[12, 43, 94, 172, 270].map((x) => (
            <circle
              key={x}
              cx={x}
              cy="22"
              r="3"
              className="memory-curve-point"
            />
          ))}
          <text x="12" y="122">
            首次学习
          </text>
          <text x="348" y="122" textAnchor="end">
            复习间隔逐渐拉长 →
          </text>
        </svg>
        <div className="memory-curve-legend">
          <span className="review">间隔复习</span>
          <span className="forget">不再复习</span>
          <small>不代表个人记忆测量</small>
        </div>
      </div>
      <details className="memory-plan-details">
        <summary>
          复习规则 <ChevronDown size={14} />
        </summary>
        {live ? (
          <p>
            模糊和忘记的词会在本轮再出现一次。跨天间隔由当天首次有效反馈和实际间隔决定。
          </p>
        ) : (
          <>
            <ol>
              {reviewIntervals.map((step) => (
                <li key={step.label}>{step.label}</li>
              ))}
            </ol>
            <p>
              每次到期后选择「认识」，进入下一档间隔。选「模糊」或「忘记」可以重学。
            </p>
          </>
        )}
      </details>
    </section>
  );
}

export function WordReviewTiming({
  word,
  now,
}: {
  word: BookWord;
  now: number;
}) {
  const { review } = word;
  const status = reviewStatus(word, now);
  return (
    <div className="word-memory-timing" aria-label={`${word.word} 的复习计划`}>
      <div>
        <Clock3 size={14} />
        <span>
          {status === "new"
            ? "等待首次学习"
            : status === "due"
              ? "现在该复习了"
              : `下次：${formatReviewTime(review.nextReviewAt, now)}`}
        </span>
      </div>
      {review.reviewCount ? (
        <>
          <p>
            {review.memory
              ? review.nextReviewAt === review.memory.longTermAt
                ? "按掌握情况安排"
                : "安排短时重测"
              : review.stage
                ? `当前间隔 · ${reviewIntervals[review.stage - 1].label}`
                : "重新学习 · 5 分钟间隔"}
            <span>已复习 {review.reviewCount} 次</span>
          </p>
          {!review.memory && (
            <ol aria-label="复习间隔节点">
              {reviewIntervals.map((step, index) => (
                <li
                  key={step.label}
                  className={
                    index < review.stage - 1
                      ? "is-past"
                      : index === review.stage - 1
                        ? "is-current"
                        : ""
                  }
                  aria-label={`${step.label}${index === review.stage - 1 ? "，当前间隔" : ""}`}
                  title={step.label}
                />
              ))}
            </ol>
          )}
          <small>
            {status === "due"
              ? `原定：${new Date(review.nextReviewAt!).toLocaleString("zh-CN", { month: "numeric", day: "numeric", hour: "2-digit", minute: "2-digit", hour12: false })}`
              : "按实际作答时间自动安排"}
          </small>
        </>
      ) : (
        <p>学过一次后，系统就会安排下一次相见。</p>
      )}
    </div>
  );
}
