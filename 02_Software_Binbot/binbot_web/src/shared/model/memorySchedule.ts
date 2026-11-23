// A practical expanding schedule inspired by Ebbinghaus, not a fitted personal
// forgetting model. Intervals run from each successful, due review.
const minute = 60_000;
const day = 24 * 60 * minute;
export const reviewIntervals = [
  { label: "20 分钟", milliseconds: 20 * minute },
  { label: "1 小时", milliseconds: 60 * minute },
  { label: "1 天", milliseconds: day },
  { label: "2 天", milliseconds: 2 * day },
  { label: "4 天", milliseconds: 4 * day },
  { label: "7 天", milliseconds: 7 * day },
  { label: "15 天", milliseconds: 15 * day },
  { label: "30 天", milliseconds: 30 * day },
] as const;
export const relearnInterval = 5 * minute;
const historyLimit = 32;
const maxReviews = 1_000_000;

export type ReviewGrade = "known" | "fuzzy" | "forgot";
type MemoryState = {
  version: 1;
  difficulty: number;
  halfLifeDays: number;
  lastRecallAt: string;
  longTermAt: string;
};

export type ReviewEvent = {
  at: string;
  remembered: boolean;
  stage: number;
  nextReviewAt: string;
  grade?: ReviewGrade;
  retry?: boolean;
};
export type MemorySchedule = {
  firstReviewedAt?: string | null;
  stage: number;
  lastReviewedAt: string | null;
  nextReviewAt: string | null;
  reviewCount: number;
  lapseCount: number;
  history: ReviewEvent[];
  memory?: MemoryState;
};
export type ScheduledWord = { review: MemorySchedule };
export type ReviewStatus = "new" | "due" | "scheduled";

export function createMemorySchedule(): MemorySchedule {
  return {
    stage: 0,
    lastReviewedAt: null,
    nextReviewAt: null,
    reviewCount: 0,
    lapseCount: 0,
    history: [],
  };
}

const object = (value: unknown): Record<string, unknown> =>
  value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
function timestamp(value: unknown): string | null {
  if (typeof value !== "string") return null;
  const date = Date.parse(value);
  if (
    !Number.isFinite(date) ||
    date < Date.UTC(2000, 0, 1) ||
    date >= Date.UTC(2100, 0, 1) ||
    new Date(date).toISOString() !== value
  )
    return null;
  return new Date(date).toISOString();
}
function stageNumber(value: unknown): value is number {
  return (
    Number.isInteger(value) &&
    (value as number) >= 0 &&
    (value as number) <= reviewIntervals.length
  );
}
function validGap(stage: number, at: string, next: string) {
  const difference = Date.parse(next) - Date.parse(at);
  const limit = stage
    ? reviewIntervals[stage - 1].milliseconds
    : relearnInterval;
  return difference > 0 && difference <= limit;
}

/** Legacy booleans have no review time; keep them in the word and start a new plan. */
export function normalizeMemorySchedule(value: unknown): MemorySchedule {
  const source = object(value);
  const last = timestamp(source.lastReviewedAt),
    next = timestamp(source.nextReviewAt);
  const state = object(source.memory);
  const lastRecall = timestamp(state.lastRecallAt);
  const longTerm = timestamp(state.longTermAt);
  const memory: MemoryState | undefined =
    state.version === 1 &&
    typeof state.difficulty === "number" &&
    Number.isInteger(state.difficulty) &&
    state.difficulty >= 1 &&
    state.difficulty <= 18 &&
    typeof state.halfLifeDays === "number" &&
    state.halfLifeDays >= 0.1 &&
    state.halfLifeDays < 1600 &&
    lastRecall &&
    longTerm &&
    last &&
    next &&
    lastRecall <= last &&
    next <= longTerm &&
    lastRecall < next
      ? {
          version: 1,
          difficulty: state.difficulty,
          halfLifeDays: state.halfLifeDays,
          lastRecallAt: lastRecall,
          longTermAt: longTerm,
        }
      : undefined;
  if (
    !last ||
    !next ||
    !stageNumber(source.stage) ||
    (!memory && !validGap(source.stage, last, next)) ||
    !Number.isInteger(source.reviewCount) ||
    (source.reviewCount as number) < 1 ||
    (source.reviewCount as number) > maxReviews ||
    (source.reviewCount as number) < source.stage ||
    !Number.isInteger(source.lapseCount) ||
    (source.lapseCount as number) < 0 ||
    (source.lapseCount as number) > (source.reviewCount as number)
  )
    return createMemorySchedule();
  const history = (Array.isArray(source.history) ? source.history : [])
    .flatMap((raw): ReviewEvent[] => {
      const event = object(raw),
        at = timestamp(event.at),
        due = timestamp(event.nextReviewAt);
      const grade =
        event.grade === "known" ||
        event.grade === "fuzzy" ||
        event.grade === "forgot"
          ? event.grade
          : undefined;
      if (
        !at ||
        !due ||
        !stageNumber(event.stage) ||
        typeof event.remembered !== "boolean" ||
        Date.parse(at) > Date.parse(last) ||
        (grade
          ? event.retry !== true && at >= due
          : (!event.remembered && event.stage !== 0) ||
            !validGap(event.stage, at, due))
      )
        return [];
      return [
        {
          at,
          nextReviewAt: due,
          remembered: event.remembered,
          stage: event.stage,
          ...(grade ? { grade, retry: event.retry === true } : {}),
        },
      ];
    })
    .sort((a, b) => Date.parse(a.at) - Date.parse(b.at))
    .slice(-Math.min(historyLimit, source.reviewCount as number));
  return {
    stage: source.stage,
    firstReviewedAt:
      timestamp(source.firstReviewedAt) ||
      (history.length === source.reviewCount ? history[0]?.at : null),
    lastReviewedAt: last,
    nextReviewAt: next,
    reviewCount: source.reviewCount as number,
    lapseCount: source.lapseCount as number,
    history,
    ...(memory ? { memory } : {}),
  };
}

export function reviewStatus(
  word: ScheduledWord,
  now = Date.now(),
): ReviewStatus {
  if (!word.review.reviewCount || !word.review.nextReviewAt) return "new";
  return Date.parse(word.review.nextReviewAt) <= now ? "due" : "scheduled";
}

export function scheduleAnswer(
  previous: MemorySchedule,
  remembered: boolean,
  now = Date.now(),
): MemorySchedule {
  if (
    !Number.isFinite(now) ||
    now < Date.UTC(2000, 0, 1) ||
    now >= Date.UTC(2099, 11, 1)
  )
    throw new Error("请检查当前时间后再复习。");
  const at = timestamp(new Date(now).toISOString());
  if (!at) throw new Error("请检查当前时间后再复习。");
  if (previous.lastReviewedAt && now < Date.parse(previous.lastReviewedAt))
    throw new Error("当前时间早于上次复习，请校准时间后重试。");
  const early =
    !!previous.nextReviewAt && Date.parse(previous.nextReviewAt) > now;
  const stage = remembered
    ? early
      ? previous.stage
      : Math.min(previous.stage + 1, reviewIntervals.length)
    : 0;
  // An early success neither skips a stage nor pushes out an existing deadline.
  const nextReviewAt =
    remembered && early
      ? previous.nextReviewAt!
      : new Date(
          now +
            (remembered
              ? reviewIntervals[stage - 1].milliseconds
              : relearnInterval),
        ).toISOString();
  const event: ReviewEvent = { at, remembered, stage, nextReviewAt };
  return {
    stage,
    firstReviewedAt:
      previous.firstReviewedAt ||
      (previous.history.length === previous.reviewCount
        ? previous.history[0]?.at
        : null) ||
      at,
    lastReviewedAt: at,
    nextReviewAt,
    reviewCount: Math.min(previous.reviewCount + 1, maxReviews),
    lapseCount: Math.min(previous.lapseCount + Number(!remembered), maxReviews),
    history: [...previous.history, event].slice(-historyLimit),
  };
}

export function reviewSummary(words: ScheduledWord[], now = Date.now()) {
  let due = 0,
    fresh = 0,
    scheduled = 0,
    nextReviewAt: string | null = null;
  for (const word of words) {
    const status = reviewStatus(word, now);
    if (status === "new") fresh++;
    else if (status === "due") due++;
    else {
      scheduled++;
      if (
        !nextReviewAt ||
        Date.parse(word.review.nextReviewAt!) < Date.parse(nextReviewAt)
      )
        nextReviewAt = word.review.nextReviewAt;
    }
  }
  return { due, fresh, scheduled, nextReviewAt };
}

/** Only due reviews and new words enter a session; overdue words come first. */
export function scheduledReviewWords<T extends ScheduledWord>(
  words: T[],
  now = Date.now(),
  newLimit = Infinity,
  batchSize = Infinity,
): T[] {
  let fresh = 0;
  return words
    .filter((word) => reviewStatus(word, now) !== "scheduled")
    .sort((a, b) => {
      const aDue = reviewStatus(a, now) === "due",
        bDue = reviewStatus(b, now) === "due";
      if (aDue !== bDue) return aDue ? -1 : 1;
      return aDue && bDue
        ? Date.parse(a.review.nextReviewAt!) -
            Date.parse(b.review.nextReviewAt!)
        : 0;
    })
    .filter((word) => reviewStatus(word, now) === "due" || fresh++ < newLimit)
    .slice(0, batchSize)
    .map((word) => structuredClone(word));
}

/** Same daily workload calculation as the server, in the device's timezone. */
export function dailyLearningPlan(
  words: ScheduledWord[],
  dailyNewLimit: number,
  timezone: string,
  now = Date.now(),
) {
  const format = new Intl.DateTimeFormat("en-CA", {
    timeZone: timezone,
    year: "numeric",
    month: "2-digit",
    day: "2-digit",
  });
  const today = format.format(now);
  let learnedToday = 0;
  let reviewLoad = 0;
  for (const { review } of words) {
    const first =
      review.firstReviewedAt ||
      (review.history.length === review.reviewCount
        ? review.history[0]?.at
        : null);
    if (first && format.format(Date.parse(first)) === today) learnedToday++;
    else if (
      (review.nextReviewAt && Date.parse(review.nextReviewAt) <= now) ||
      (review.lastReviewedAt &&
        format.format(Date.parse(review.lastReviewedAt)) === today)
    )
      reviewLoad++;
  }
  const summary = reviewSummary(words, now);
  const remainingNew = Math.min(
    summary.fresh,
    Math.max(
      0,
      dailyNewLimit - learnedToday - Math.max(0, reviewLoad - dailyNewLimit),
    ),
  );
  return {
    ...summary,
    dailyNewLimit,
    learnedToday,
    remainingNew,
    learned: words.length - summary.fresh,
  };
}

export function formatReviewTime(value: string | null, now = Date.now()) {
  if (!value) return "首次学习后安排";
  const at = Date.parse(value),
    remaining = at - now;
  if (remaining <= 0) return "已到复习时间";
  if (remaining < 60 * minute) return `${Math.ceil(remaining / minute)} 分钟后`;
  const date = new Date(at),
    today = new Date(now);
  const clock = date.toLocaleTimeString("zh-CN", {
    hour: "2-digit",
    minute: "2-digit",
    hour12: false,
  });
  if (date.toDateString() === today.toDateString()) return `今天 ${clock}`;
  const tomorrow = new Date(now);
  tomorrow.setDate(tomorrow.getDate() + 1);
  if (date.toDateString() === tomorrow.toDateString()) return `明天 ${clock}`;
  return `${date.getMonth() + 1} 月 ${date.getDate()} 日 ${clock}`;
}

export function answerIntervalLabel(
  review: MemorySchedule,
  remembered: boolean,
  now = Date.now(),
) {
  if (!remembered) return "5 分钟后再学";
  if (review.nextReviewAt && Date.parse(review.nextReviewAt) > now)
    return "保持原定时间";
  return `${reviewIntervals[Math.min(review.stage, reviewIntervals.length - 1)].label}后再见`;
}
