import { parseWordImport } from "./wordImport";
import { useWorkspace } from "../../shared/state/workspaceContext";
import { useRef, useState } from "react";
import { uid } from "../../shared/model/data";
import { BookOpen, Check, ChevronDown, Plus, Search } from "lucide-react";
import { SnapshotDialog } from "../../shared/ui/FeatureControls";
import { vocabularyCards } from "../../shared/model/deviceFeatures";
import {
  validateWord,
  wordKey,
  type Wordbook,
  type WordCard,
  type BookWord,
} from "../../shared/model/wordbooks";
import {
  formatReviewTime,
  scheduleAnswer,
  type ReviewGrade,
} from "../../shared/model/memorySchedule";

export function AddBookWordsDialog({
  book,
  close,
  save,
  saveError = "",
}: {
  book: Wordbook;
  close: () => void;
  save: (cards: WordCard[]) => boolean;
  saveError?: string;
}) {
  const workspace = useWorkspace();
  const [mode, setMode] = useState<"pick" | "manual">("pick");
  const [search, setSearch] = useState("");
  const [selected, setSelected] = useState<string[]>([]);
  const [error, setError] = useState("");
  const [card, setCard] = useState<WordCard>({
    word: "",
    phonetic: "",
    meaning: "",
    example: "",
    translation: "",
  });
  const existing = new Set(book.words.map((entry) => wordKey(entry.word)));
  const matches = vocabularyCards.filter((entry) =>
    `${entry.word} ${entry.meaning}`
      .toLowerCase()
      .includes(search.trim().toLowerCase()),
  );
  function field(
    key: keyof WordCard,
    label: string,
    placeholder: string,
    required = false,
  ) {
    return (
      <label className="field-label" key={key}>
        {label}
        {!required && <span className="optional">可选</span>}
        <input
          value={card[key]}
          required={required}
          maxLength={
            key === "word"
              ? 80
              : key === "meaning"
                ? 300
                : key === "phonetic"
                  ? 120
                  : 500
          }
          autoFocus={key === "word"}
          placeholder={placeholder}
          onChange={(event) => {
            setCard((previous) => ({ ...previous, [key]: event.target.value }));
            setError("");
          }}
        />
      </label>
    );
  }
  return (
    <SnapshotDialog
      title={`收进「${book.name}」`}
      close={close}
      closeLabel="关闭添加单词"
      note={
        workspace.live
          ? "词本保存在服务器，选中的词本会同步到设备学习。"
          : "演示词本保存在当前浏览器。"
      }
    >
      <label className="button secondary" style={{ marginBottom: 16 }}>
        导入 CSV / TSV 词表
        <input
          type="file"
          className="sr-only"
          accept=".csv,.tsv,.txt,text/csv,text/plain"
          onChange={async (event) => {
            const file = event.target.files?.[0];
            event.target.value = "";
            if (!file) return;
            try {
              if (file.size > 2 * 1024 * 1024)
                throw new Error("请导入不超过 2 MB 的词表。");
              const cards = parseWordImport(await file.text());
              if (save(cards)) close();
            } catch (reason) {
              setError(
                reason instanceof Error ? reason.message : "词表读取失败",
              );
            }
          }}
        />
      </label>
      <p className="feature-note">
        支持导出的词表；无表头时按单词、释义、音标、英文例句、例句翻译排列。
      </p>
      <div
        className="wordbook-dialog-tabs"
        role="group"
        aria-label="添加单词的方式"
      >
        <button
          type="button"
          aria-pressed={mode === "pick"}
          onClick={() => {
            setMode("pick");
            setError("");
          }}
        >
          从内置词库选
        </button>
        <button
          type="button"
          aria-pressed={mode === "manual"}
          onClick={() => {
            setMode("manual");
            setError("");
          }}
        >
          手动添加
        </button>
      </div>
      <form
        onSubmit={(event) => {
          event.preventDefault();
          try {
            if (mode === "pick") {
              const cards = vocabularyCards.filter((entry) =>
                selected.includes(entry.word),
              );
              if (!cards.length) return;
              if (save(cards)) close();
            } else {
              const checked = validateWord(card);
              if (existing.has(wordKey(checked.word))) {
                setError("这个单词已在词本里，可以直接查看。");
                return;
              }
              if (save([checked])) close();
            }
          } catch (reason) {
            setError(
              reason instanceof Error ? reason.message : "请检查单词内容。",
            );
          }
        }}
      >
        {mode === "pick" ? (
          <>
            <label className="feature-search wordbook-pick-search">
              <Search size={15} />
              <input
                type="search"
                aria-label="查找要加入词本的单词"
                value={search}
                onChange={(event) => setSearch(event.target.value)}
                placeholder="搜索单词或释义"
              />
            </label>
            <div
              className="wordbook-picker"
              role="group"
              aria-label="选择加入词本的单词"
            >
              {matches.map((entry) => (
                <label
                  key={entry.word}
                  className={
                    existing.has(wordKey(entry.word)) ? "is-added" : ""
                  }
                >
                  <input
                    type="checkbox"
                    checked={
                      existing.has(wordKey(entry.word)) ||
                      selected.includes(entry.word)
                    }
                    disabled={existing.has(wordKey(entry.word))}
                    onChange={() =>
                      setSelected((previous) =>
                        previous.includes(entry.word)
                          ? previous.filter((word) => word !== entry.word)
                          : [...previous, entry.word],
                      )
                    }
                  />
                  <span>
                    <strong>{entry.word}</strong>
                    <small>{entry.meaning}</small>
                  </span>
                  {existing.has(wordKey(entry.word)) && <em>已加入</em>}
                </label>
              ))}
              {!matches.length && (
                <p className="wordbook-dialog-hint">
                  没有找到，可以切到「手动添加」。
                </p>
              )}
            </div>
          </>
        ) : (
          <div className="form-stack">
            <div className="form-columns">
              {field("word", "英文单词或短语", "比如：wander", true)}
              {field("meaning", "中文释义", "比如：漫步；闲逛", true)}
            </div>
            <details className="wordbook-extra">
              <summary>
                音标与例句 <ChevronDown size={14} />
              </summary>
              <div className="form-stack">
                {field("phonetic", "音标", "比如：/ˈwɑːndər/")}
                {field("example", "英文例句", "写一个便于记忆的句子")}
                {field("translation", "例句翻译", "补充这句话的中文意思")}
              </div>
            </details>
          </div>
        )}
        {(error || saveError) && (
          <p className="wordbook-error" role="alert">
            {error || saveError}
          </p>
        )}
        <div className="dialog-actions">
          <button type="button" className="button secondary" onClick={close}>
            取消
          </button>
          <button
            className="button primary"
            type="submit"
            disabled={
              mode === "pick"
                ? !selected.length
                : !card.word.trim() || !card.meaning.trim()
            }
          >
            <Plus size={15} />
            {mode === "pick" ? `加入 ${selected.length} 个单词` : "加入单词本"}
          </button>
        </div>
      </form>
    </SnapshotDialog>
  );
}

const reviewGrades = [
  { grade: "forgot", label: "忘记" },
  { grade: "fuzzy", label: "模糊" },
  { grade: "known", label: "认识" },
] as const;

export function ReviewBookDialog({
  book,
  words,
  answer,
  close,
}: {
  book: Wordbook;
  words: BookWord[];
  answer: (
    word: string,
    grade: ReviewGrade,
    at: number,
    eventId: string,
    retry: boolean,
  ) => boolean | void | Promise<string | null>;
  close: () => void;
}) {
  const workspace = useWorkspace();
  const [index, setIndex] = useState(0);
  const [revealed, setRevealed] = useState(false);
  const [retests, setRetests] = useState<BookWord[]>([]);
  const [results, setResults] = useState<
    Record<string, { grade: ReviewGrade; next: string | null }>
  >({});
  const [error, setError] = useState("");
  const submittedIndex = useRef(-1);
  const attempt = useRef<{
    index: number;
    id: string;
    grade: ReviewGrade;
    at: number;
  } | null>(null);
  const [saving, setSaving] = useState(false);
  const retry = index >= words.length;
  const card = retry ? retests[index - words.length] : words[index];
  const nextReview = Object.values(results)
    .flatMap(({ next }) => (next ? [next] : []))
    .sort()[0];
  const remembered = Object.values(results).filter(
    ({ grade }) => grade === "known",
  ).length;
  async function respond(grade: ReviewGrade) {
    if (
      workspace.pending ||
      !card ||
      !revealed ||
      submittedIndex.current === index
    )
      return;
    if (attempt.current?.index !== index)
      attempt.current = { index, id: uid(), grade, at: Date.now() };
    if (attempt.current.grade !== grade) return;
    const { id, at } = attempt.current;
    submittedIndex.current = index;
    setSaving(true);
    try {
      const result = await answer(card.word, grade, at, id, retry);
      if (result === false) throw new Error("这次复习暂未保存，请重试。");
      const next =
        typeof result === "string"
          ? result
          : workspace.live
            ? null
            : scheduleAnswer(card.review, grade === "known", at).nextReviewAt;
      setResults((previous) => ({ ...previous, [card.word]: { grade, next } }));
    } catch (reason) {
      submittedIndex.current = -1;
      setError(
        reason instanceof Error ? reason.message : "这次复习暂未保存，请重试。",
      );
      return;
    } finally {
      setSaving(false);
    }
    setError("");
    if (!retry && grade !== "known")
      setRetests((previous) => [...previous, card]);
    setIndex((previous) => previous + 1);
    setRevealed(false);
  }
  return (
    <SnapshotDialog
      title={book.name}
      close={close}
      closeLabel="结束词本复习"
      note={
        workspace.live
          ? "作答会同步到设备。模糊和忘记的词会在本轮再出现一次。"
          : "演示复习计划保存在当前浏览器。"
      }
    >
      {card ? (
        <div className="wordbook-review">
          <div className="wordbook-review-progress">
            <span>{retry ? "本轮重测" : "按记忆计划复习"}</span>
            <strong>
              {retry ? index - words.length + 1 : index + 1} /{" "}
              {retry ? retests.length : words.length}
            </strong>
          </div>
          <div className="wordbook-review-meter" aria-hidden="true">
            <i
              style={{
                width: `${retry ? ((index - words.length) / retests.length) * 100 : (index / words.length) * 100}%`,
              }}
            />
          </div>
          <h2 lang="en" key={`${index}-word`}>
            {card.word}
          </h2>
          {card.phonetic && <p className="word-phonetic">{card.phonetic}</p>}
          {revealed ? (
            <div className="wordbook-review-answer" aria-live="polite">
              <h3>{card.meaning}</h3>
              {card.example && <p lang="en">{card.example}</p>}
              {card.translation && <small>{card.translation}</small>}
              <div className="dialog-actions">
                {reviewGrades.map(({ grade, label }) => (
                  <button
                    key={grade}
                    type="button"
                    className={`button ${grade === "known" ? "primary" : "secondary"}`}
                    disabled={
                      saving ||
                      workspace.pending ||
                      (attempt.current?.index === index &&
                        attempt.current.grade !== grade)
                    }
                    onClick={() => void respond(grade)}
                  >
                    {label}
                  </button>
                ))}
              </div>
            </div>
          ) : (
            <button
              type="button"
              className="button primary wordbook-reveal"
              onClick={() => setRevealed(true)}
            >
              想一想，再看释义
            </button>
          )}
          {error && (
            <p className="wordbook-review-error" role="alert">
              {error}
            </p>
          )}
        </div>
      ) : (
        <div className="wordbook-review-finished" role="status">
          <span>
            <BookOpen size={30} strokeWidth={1.4} />
          </span>
          <h2>本批完成</h2>
          <p>
            学习了 {words.length} 个词，认识 {remembered} 个。
          </p>
          <small>
            {nextReview
              ? `下一次：${formatReviewTime(nextReview)}`
              : "本次没有需要复习的词。"}
          </small>
          <button type="button" className="button primary" onClick={close}>
            回到单词本 <Check size={15} />
          </button>
        </div>
      )}
    </SnapshotDialog>
  );
}
