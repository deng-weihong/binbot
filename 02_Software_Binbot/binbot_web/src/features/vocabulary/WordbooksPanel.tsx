import { useWorkspace } from "../../shared/state/workspaceContext";
import { useEffect, useMemo, useState } from "react";
import {
  ArrowRight,
  Bookmark,
  BookOpen,
  Check,
  Download,
  Pencil,
  Plus,
  RotateCw,
  Search,
  Trash2,
} from "lucide-react";
import { uid } from "../../shared/model/data";
import { createConsoleTransport } from "../../shared/api/consoleApi";
import type { FeaturePageProps } from "../../shared/ui/featureTypes";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";
import { AddBookWordsDialog, ReviewBookDialog } from "./WordbookDialogs";
import { MemoryReviewPlan, WordReviewTiming } from "./MemoryReviewPlan";
import { WordPagination, wordPageSize } from "./WordPagination";
import {
  formatReviewTime,
  reviewStatus,
  dailyLearningPlan,
  scheduledReviewWords,
} from "../../shared/model/memorySchedule";
import {
  addBook,
  addBookWords,
  answerBookWord,
  builtinBookId,
  cet4BookId,
  isBuiltinBook,
  deleteBook,
  hasBookWord,
  maxWordbooks,
  removeBookWord,
  renameBook,
  savedBookId,
  wordbookCsv,
  wordKey,
  type Wordbook,
  type BookWord,
  type Wordbooks,
  type WordCard,
} from "../../shared/model/wordbooks";

type Props = Pick<FeaturePageProps, "device" | "data" | "setData" | "notify">;
const statusLabels = {
  new: "新词待学",
  due: "到期复习",
  scheduled: "已安排复习",
};
const api = createConsoleTransport();

export function SaveWordButton({
  data,
  setData,
  notify,
  card,
  iconOnly = false,
}: Pick<Props, "data" | "setData" | "notify"> & {
  card: WordCard;
  iconOnly?: boolean;
}) {
  const saved = hasBookWord(data.wordbooks, savedBookId, card.word);
  return (
    <button
      type="button"
      className={`word-save ${saved ? "is-saved" : ""} ${iconOnly ? "icon-only" : ""}`}
      aria-pressed={saved}
      aria-label={`${saved ? "从生词本移除" : "收藏到生词本"} ${card.word}`}
      title={saved ? "移出生词本" : "收藏到我的生词本"}
      onClick={() => {
        try {
          const update = (previous: Wordbooks) =>
            saved
              ? removeBookWord(previous, savedBookId, card.word)
              : addBookWords(previous, savedBookId, [card]);
          update(data.wordbooks);
          setData((previous) => {
            try {
              return { ...previous, wordbooks: update(previous.wordbooks) };
            } catch {
              return previous;
            }
          });
          notify(saved ? "已移出生词本" : "已收进我的生词本");
        } catch (reason) {
          notify(reason instanceof Error ? reason.message : "暂时无法收藏。");
        }
      }}
    >
      <Bookmark size={15} fill={saved ? "currentColor" : "none"} />
      {!iconOnly && (saved ? "已收进生词本" : "收进生词本")}
    </button>
  );
}

export default function WordbooksPanel({
  device,
  data,
  setData,
  notify,
}: Props) {
  const workspace = useWorkspace();
  const library = data.wordbooks;
  const book =
    library.books.find((item) => item.id === library.activeBookId) ||
    library.books[0];
  const [selected, setSelected] = useState("");
  const [pageIndex, setPageIndex] = useState(0);
  const [search, setSearch] = useState("");
  const [now, setNow] = useState(Date.now);
  const [filter, setFilter] = useState<"all" | "due" | "new" | "scheduled">(
    "all",
  );
  const [naming, setNaming] = useState<"new" | "rename" | null>(null);
  const [name, setName] = useState("");
  const [error, setError] = useState("");
  const [adding, setAdding] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [review, setReview] = useState<{
    book: Wordbook;
    words: BookWord[];
  } | null>(null);
  const [starting, setStarting] = useState(false);
  const savedWords = new Set(
    library.books
      .find((item) => item.id === savedBookId)
      ?.words.map((card) => wordKey(card.word)),
  );
  const words = useMemo(
    () =>
      book.words.filter(
        (card) =>
          (filter === "all" || reviewStatus(card, now) === filter) &&
          `${card.word} ${card.meaning}`
            .toLowerCase()
            .includes(search.trim().toLowerCase()),
      ),
    [book.words, filter, search, now],
  );
  const page = Math.min(
    pageIndex,
    Math.max(0, Math.ceil(words.length / wordPageSize) - 1),
  );
  const pageWords = words.slice(page * wordPageSize, (page + 1) * wordPageSize);
  const card = words.find((entry) => entry.word === selected) || pageWords[0];
  const timezone = data.timezone || "Asia/Shanghai";
  const summary = useMemo(
    () => dailyLearningPlan(book.words, library.dailyNewLimit, timezone, now),
    [book.words, library.dailyNewLimit, timezone, now],
  );
  const isBuiltin = isBuiltinBook(book.id);
  const editable = !isBuiltin && book.id !== savedBookId;

  useEffect(() => {
    setPageIndex(0);
    setSelected("");
  }, [book.id, search, filter]);

  useEffect(() => {
    const refresh = () => setNow(Date.now());
    const dueIn = summary.nextReviewAt
      ? Date.parse(summary.nextReviewAt) - now
      : 30000;
    const timer = window.setTimeout(
      refresh,
      Math.max(50, Math.min(30000, dueIn)),
    );
    window.addEventListener("focus", refresh);
    document.addEventListener("visibilitychange", refresh);
    return () => {
      window.clearTimeout(timer);
      window.removeEventListener("focus", refresh);
      document.removeEventListener("visibilitychange", refresh);
    };
  }, [now, summary.nextReviewAt]);

  function change(update: (previous: Wordbooks) => Wordbooks) {
    // Validate against the current library before scheduling the state update.
    try {
      update(library);
      setData((previous) => {
        try {
          return { ...previous, wordbooks: update(previous.wordbooks) };
        } catch {
          return previous;
        }
      });
      return true;
    } catch (reason) {
      setError(
        reason instanceof Error ? reason.message : "暂时无法保存单词本。",
      );
      return false;
    }
  }
  function choose(id: string) {
    change((previous) => ({ ...previous, activeBookId: id }));
    setSearch("");
    setFilter("all");
    setSelected("");
    setError("");
  }
  async function startReview() {
    setStarting(true);
    setError("");
    try {
      const at = Date.now();
      const queue = workspace.live
        ? (await api.studyBatch(device.id, book.id)).map((word) => {
            const card = book.words.find((item) => item.word === word);
            if (!card) throw new Error("词本有更新，请刷新后开始复习。");
            return card;
          })
        : scheduledReviewWords(
            book.words,
            at,
            dailyLearningPlan(book.words, library.dailyNewLimit, timezone, at)
              .remainingNew,
            30,
          );
      setNow(at);
      if (queue.length) setReview({ book, words: queue });
    } catch (reason) {
      setError(
        reason instanceof Error ? reason.message : "暂时无法读取复习计划。",
      );
    } finally {
      setStarting(false);
    }
  }
  function download() {
    const url = URL.createObjectURL(
      new Blob([wordbookCsv(book)], { type: "text/csv;charset=utf-8" }),
    );
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${device.id}-${book.id}.csv`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  return (
    <div className="wordbooks-panel">
      <div className="wordbooks-heading">
        <h2>选择单词本</h2>
        <button
          type="button"
          className="button primary"
          disabled={library.books.length >= maxWordbooks}
          onClick={() => {
            setName("");
            setError("");
            setNaming("new");
          }}
        >
          <Plus size={15} />
          新建单词本
        </button>
      </div>
      <div className="wordbook-shelf" role="group" aria-label="选择单词本">
        {library.books.map((item) => (
          <button
            type="button"
            key={item.id}
            aria-pressed={item.id === book.id}
            className={`wordbook-cover ${item.id === book.id ? "is-selected" : ""}`}
            onClick={() => choose(item.id)}
          >
            <span
              className={`wordbook-cover-icon ${item.id === savedBookId ? "is-saved" : ""}`}
            >
              {item.id === savedBookId ? (
                <Bookmark size={23} strokeWidth={1.5} />
              ) : (
                <BookOpen size={23} strokeWidth={1.5} />
              )}
            </span>
            <span className="wordbook-cover-copy">
              <strong>{item.name}</strong>
              <small>
                {item.id === builtinBookId
                  ? "App 内置"
                  : item.id === cet4BookId
                    ? "完整词库 · 英美发音已备齐"
                    : item.id === savedBookId
                      ? "收藏与手动添加"
                      : "自建词本"}{" "}
                · {item.words.length} 词
              </small>
            </span>
            <span className="wordbook-cover-choice">
              {item.id === book.id ? (
                <Check size={14} />
              ) : (
                <ArrowRight size={14} />
              )}
            </span>
          </button>
        ))}
      </div>
      {error && !naming && (
        <p className="wordbook-error" role="alert">
          {error}
        </p>
      )}
      <MemoryReviewPlan
        book={book}
        now={now}
        dailyNewLimit={library.dailyNewLimit}
        timezone={data.timezone || "Asia/Shanghai"}
        changeDailyLimit={(limit) =>
          change((previous) => ({ ...previous, dailyNewLimit: limit }))
        }
        action={
          <button
            type="button"
            className="button accent"
            disabled={
              starting ||
              workspace.loading ||
              workspace.pending ||
              (!summary.due && !summary.remainingNew)
            }
            onClick={() => void startReview()}
          >
            {starting
              ? "正在读取…"
              : summary.due || summary.remainingNew || !book.words.length
                ? "开始复习"
                : summary.fresh
                  ? "今日学习已完成"
                  : "等待下次复习"}
            <ArrowRight size={14} />
          </button>
        }
      />
      <div className="learning-body wordbook-body">
        <section className="card word-library">
          <div className="feature-card-heading">
            <div>
              <h2>{book.name}</h2>
              <p>{book.words.length} 个单词 · 按到期时间安排复习</p>
            </div>
            <div className="wordbook-heading-actions">
              {editable && (
                <>
                  <button
                    type="button"
                    className="icon-button subtle"
                    aria-label={`重命名${book.name}`}
                    onClick={() => {
                      setName(book.name);
                      setError("");
                      setNaming("rename");
                    }}
                  >
                    <Pencil size={15} />
                  </button>
                  <button
                    type="button"
                    className="icon-button subtle"
                    aria-label={`删除词本${book.name}`}
                    onClick={() => setDeleting(true)}
                  >
                    <Trash2 size={15} />
                  </button>
                </>
              )}
              <button
                type="button"
                className="icon-button subtle"
                disabled={!book.words.length}
                aria-label={`导出${book.name}`}
                onClick={download}
              >
                <Download size={16} />
              </button>
              {!isBuiltin && (
                <button
                  type="button"
                  className="button secondary"
                  onClick={() => {
                    setError("");
                    setAdding(true);
                  }}
                >
                  <Plus size={14} />
                  添加单词
                </button>
              )}
            </div>
          </div>
          <div className="feature-toolbar">
            <div className="feature-segments" aria-label="词本复习安排">
              {(
                [
                  { id: "all", label: "全部", count: book.words.length },
                  {
                    id: "due",
                    label: "已到期",
                    count: summary.due,
                  },
                  { id: "new", label: "新词", count: summary.fresh },
                  {
                    id: "scheduled",
                    label: "已安排",
                    count: summary.scheduled,
                  },
                ] as const
              ).map((item) => (
                <button
                  key={item.id}
                  type="button"
                  aria-pressed={filter === item.id}
                  className={filter === item.id ? "selected" : ""}
                  onClick={() => setFilter(item.id)}
                >
                  {item.label}
                  <span>{item.count}</span>
                </button>
              ))}
            </div>
            <label className="feature-search">
              <Search size={15} />
              <input
                type="search"
                value={search}
                placeholder="在这本里找一个词"
                aria-label="搜索当前词本"
                onChange={(event) => setSearch(event.target.value)}
              />
            </label>
          </div>
          <div className="word-table-wrap">
            <table className="word-table wordbook-table">
              <thead>
                <tr>
                  <th scope="col">单词</th>
                  <th scope="col">中文释义</th>
                  <th scope="col">复习安排</th>
                  <th scope="col">
                    <span className="sr-only">词本操作</span>
                  </th>
                </tr>
              </thead>
              <tbody>
                {pageWords.map((entry) => (
                  <tr
                    key={entry.word}
                    className={card?.word === entry.word ? "is-selected" : ""}
                  >
                    <td>
                      <button
                        type="button"
                        aria-label={`查看词本单词 ${entry.word}`}
                        onClick={() => setSelected(entry.word)}
                      >
                        <strong>{entry.word}</strong>
                        <small>{entry.phonetic}</small>
                      </button>
                    </td>
                    <td>{entry.meaning}</td>
                    <td>
                      <span
                        className={`word-status ${reviewStatus(entry, now)}`}
                      >
                        {reviewStatus(entry, now) === "scheduled" ? (
                          <Check size={12} />
                        ) : (
                          <RotateCw size={12} />
                        )}
                        {statusLabels[reviewStatus(entry, now)]}
                      </span>
                      <time
                        className="wordbook-review-time"
                        dateTime={entry.review.nextReviewAt || undefined}
                      >
                        {formatReviewTime(entry.review.nextReviewAt, now)}
                      </time>
                    </td>
                    <td>
                      {isBuiltin ? (
                        <SaveWordButton
                          card={entry}
                          data={data}
                          setData={setData}
                          notify={notify}
                          iconOnly
                        />
                      ) : (
                        <button
                          type="button"
                          className="icon-button subtle"
                          aria-label={`从${book.name}移除 ${entry.word}`}
                          onClick={() =>
                            change((previous) =>
                              removeBookWord(previous, book.id, entry.word),
                            )
                          }
                        >
                          <Trash2 size={14} />
                        </button>
                      )}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
            {!words.length && (
              <div className="feature-empty wordbook-empty">
                <BookOpen size={29} strokeWidth={1.4} />
                <h3>{book.words.length ? "未找到符合条件的单词" : "词本暂无单词"}</h3>
                <p>
                  {book.words.length
                    ? "换个关键词，或看看其他复习安排。"
                    : "从内置词库选几个，或写下刚遇到的新词。"}
                </p>
                {!book.words.length && !isBuiltin && (
                  <button
                    type="button"
                    className="button secondary"
                    onClick={() => setAdding(true)}
                  >
                    <Plus size={14} />
                    加入第一个单词
                  </button>
                )}
              </div>
            )}
          </div>
          <WordPagination
            total={words.length}
            page={page}
            change={(next) => {
              setPageIndex(next);
              setSelected("");
            }}
          />
          <p className="feature-note">
            {data.deviceName}的单词本 ·{" "}
            {workspace.live
              ? workspace.snapshot?.control.ready &&
                (
                  workspace.snapshot.reported.vocabulary as
                    | { bookId?: string }
                    | undefined
                )?.bookId === book.id
                ? "当前词本已同步到设备"
                : "等待设备同步当前词本"
              : "演示数据保存在当前浏览器"}
          </p>
        </section>
        <aside className="word-aside">
          {card ? (
            <section className="card word-detail">
              <div className="feature-card-heading">
                <span className="feature-muted">单词详情</span>
                <SaveWordButton
                  card={card}
                  data={data}
                  setData={setData}
                  notify={notify}
                  iconOnly
                />
              </div>
              <span className={`word-status ${reviewStatus(card, now)}`}>
                {statusLabels[reviewStatus(card, now)]}
              </span>
              <h2 lang="en">{card.word}</h2>
              {card.phonetic && (
                <span className="word-phonetic">{card.phonetic}</span>
              )}
              <h3>{card.meaning}</h3>
              {(card.example || card.translation) && (
                <div className="word-example">
                  <strong>例句</strong>
                  {card.example && <p lang="en">{card.example}</p>}
                  {card.translation && <small>{card.translation}</small>}
                </div>
              )}
              <WordReviewTiming word={card} now={now} />
              <div className="word-detail-footer">
                <span className="status-dot" />
                {savedWords.has(wordKey(card.word))
                  ? "已在我的生词本"
                  : "点击书签，收进我的生词本"}
              </div>
            </section>
          ) : (
            <section className="card wordbook-placeholder">
              <Bookmark size={30} strokeWidth={1.3} />
              <h3>单词详情</h3>
              <p>添加单词后可查看释义和例句。</p>
            </section>
          )}
        </aside>
      </div>
      {naming && (
        <SnapshotDialog
          title={naming === "new" ? "给新词本起个名字" : "修改词本名称"}
          closeLabel="关闭词本名称表单"
          close={() => setNaming(null)}
          note="每台设备有自己的单词本。"
        >
          <form
            onSubmit={(event) => {
              event.preventDefault();
              const id = naming === "new" ? uid() : book.id;
              if (
                change((previous) =>
                  naming === "new"
                    ? addBook(previous, id, name)
                    : renameBook(previous, id, name),
                )
              ) {
                setNaming(null);
                setSearch("");
                setFilter("all");
                setSelected("");
                notify(naming === "new" ? "新单词本已创建" : "词本名称已更新");
              }
            }}
          >
            <label className="field-label">
              词本名称
              <input
                autoFocus
                required
                maxLength={24}
                value={name}
                placeholder="比如：阅读里遇到的词"
                onChange={(event) => {
                  setName(event.target.value);
                  setError("");
                }}
              />
            </label>
            {error && (
              <p className="wordbook-error" role="alert">
                {error}
              </p>
            )}
            <div className="dialog-actions">
              <button
                type="button"
                className="button secondary"
                onClick={() => setNaming(null)}
              >
                取消
              </button>
              <button
                type="submit"
                className="button primary"
                disabled={!name.trim()}
              >
                <Check size={15} />
                {naming === "new" ? "创建单词本" : "保存名称"}
              </button>
            </div>
          </form>
        </SnapshotDialog>
      )}
      {adding && (
        <AddBookWordsDialog
          book={book}
          saveError={error}
          close={() => setAdding(false)}
          save={(cards) => {
            const result = change((previous) =>
              addBookWords(previous, book.id, cards),
            );
            if (result) notify("单词已加入词本");
            return result;
          }}
        />
      )}
      {deleting && (
        <SnapshotDialog
          title={`删除「${book.name}」？`}
          closeLabel="取消删除词本"
          close={() => setDeleting(false)}
          note="其他词本里的单词和设备学习记录会保留。"
        >
          <p className="wordbook-dialog-hint">
            这个词本的 {book.words.length} 个单词及网页复习进度会一并删除。
          </p>
          <div className="dialog-actions">
            <button
              type="button"
              className="button secondary"
              onClick={() => setDeleting(false)}
            >
              保留词本
            </button>
            <button
              type="button"
              className="button primary"
              onClick={() => {
                change((previous) => deleteBook(previous, book.id));
                setDeleting(false);
                setSearch("");
                setFilter("all");
                setSelected("");
              }}
            >
              删除词本
            </button>
          </div>
        </SnapshotDialog>
      )}
      {review && (
        <ReviewBookDialog
          book={review.book}
          words={review.words}
          close={() => setReview(null)}
          answer={(word, grade, at, eventId, retry) => {
            if (workspace.live) {
              return workspace
                .reviewWord(review.book.id, word, grade, eventId, retry)
                .then((next) => {
                  setNow(Date.now());
                  return next;
                });
            }
            const saved = change((previous) =>
              answerBookWord(
                previous,
                review.book.id,
                word,
                grade === "known",
                at,
              ),
            );
            setNow(at);
            return saved;
          }}
        />
      )}
    </div>
  );
}
