import { useConsole } from "../../shared/state/consoleContext";
import { useEffect, useMemo, useState, type CSSProperties } from "react";
import {
  ArrowRight,
  ArrowUpRight,
  BookOpen,
  Check,
  CircleCheck,
  Download,
  RotateCw,
  Search,
  Sparkles,
} from "lucide-react";
import { vocabularyCards as demoVocabularyCards } from "../../shared/model/deviceFeatures";
import type { FeaturePageProps as FeatureProps } from "../../shared/ui/featureTypes";
import WordbooksPanel, { SaveWordButton } from "./WordbooksPanel";
import { WordPagination, wordPageSize } from "./WordPagination";
import { dailyLearningPlan, reviewStatus } from "../../shared/model/memorySchedule";

export function VocabularyPage({
  device,
  data,
  setData,
  notify,
}: Pick<FeatureProps, "device" | "data" | "setData" | "notify">) {
  const { mode } = useConsole();
  const progress = data.features.vocabulary;
  const activeBook =
    data.wordbooks.books.find(
      (book) => book.id === data.wordbooks.activeBookId,
    ) || data.wordbooks.books[0];
  const demoDaily = mode === "demo" && activeBook.id !== "cet4";
  const vocabularyCards = demoDaily ? demoVocabularyCards : activeBook.words;
  const bookName = demoDaily ? "日常英语" : activeBook.name;
  const now = Date.now();
  const plan = dailyLearningPlan(
    activeBook.words,
    data.wordbooks.dailyNewLimit,
    data.timezone || "Asia/Shanghai",
    now,
  );
  const [view, setView] = useState<"progress" | "books">(() =>
    typeof window !== "undefined" && window.location.hash === "#wordbooks"
      ? "books"
      : "progress",
  );
  const [filter, setFilter] = useState<"all" | "known" | "review">("all");
  const [search, setSearch] = useState("");
  const [selected, setSelected] = useState(progress.currentWord);
  const [pageIndex, setPageIndex] = useState(0);
  useEffect(() => {
    const update = () =>
      setView(window.location.hash === "#wordbooks" ? "books" : "progress");
    window.addEventListener("popstate", update);
    window.addEventListener("hashchange", update);
    return () => {
      window.removeEventListener("popstate", update);
      window.removeEventListener("hashchange", update);
    };
  }, []);
  useEffect(() => {
    setPageIndex(0);
    setSelected("");
  }, [activeBook.id, filter, search]);
  const known = useMemo(
    () =>
      new Set(
        demoDaily
          ? progress.knownWords
          : activeBook.words
              .filter((card) => card.remembered)
              .map((card) => card.word),
      ),
    [demoDaily, progress.knownWords, activeBook.words],
  );
  const learned = demoDaily ? known.size : plan.learned;
  const dueWords = new Set(
    demoDaily
      ? vocabularyCards
          .filter((card) => !known.has(card.word))
          .map((card) => card.word)
      : activeBook.words
          .filter((card) => reviewStatus(card, now) === "due")
          .map((card) => card.word),
  );
  const words = vocabularyCards.filter(
    (word) =>
      (filter === "all" ||
        (filter === "known"
          ? known.has(word.word)
          : dueWords.has(word.word))) &&
      `${word.word} ${word.meaning}`
        .toLowerCase()
        .includes(search.trim().toLowerCase()),
  );
  const page = Math.min(
    pageIndex,
    Math.max(0, Math.ceil(words.length / wordPageSize) - 1),
  );
  const pageWords = words.slice(page * wordPageSize, (page + 1) * wordPageSize);
  const card = words.find((word) => word.word === selected) || pageWords[0];
  const percentage = Math.round(
    (learned / Math.max(1, vocabularyCards.length)) * 100,
  );
  const exportProgress = () => {
    const rows = [
      ["设备", "单词", "音标", "释义", "本轮状态", "数据类型"],
      ...vocabularyCards.map((word) => [
        device.id,
        word.word,
        word.phonetic,
        word.meaning,
        known.has(word.word) ? "已掌握" : "待复习",
        mode === "live" ? "学习记录" : "演示数据",
      ]),
    ];
    const csv =
      "\uFEFF" +
      rows
        .map((row) =>
          row.map((cell) => `"${cell.replaceAll('"', '""')}"`).join(","),
        )
        .join("\r\n");
    const url = URL.createObjectURL(
      new Blob([csv], { type: "text/csv;charset=utf-8" }),
    );
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${device.id}-vocabulary.csv`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  };
  return (
    <div className="vocabulary-page feature-page">
      <nav className="section-nav" aria-label="单词学习分类">
        {(
          [
            { id: "progress", label: "学习情况", icon: CircleCheck },
            { id: "books", label: "单词本", icon: BookOpen },
          ] as const
        ).map(({ id, label, icon: Icon }) => (
          <button
            key={id}
            type="button"
            aria-pressed={view === id}
            onClick={() => {
              setView(id);
              window.history.replaceState(
                {},
                "",
                `/devices/${device.id}/vocabulary${id === "books" ? "#wordbooks" : ""}`,
              );
            }}
          >
            <Icon size={16} strokeWidth={1.5} />
            {label}
          </button>
        ))}
      </nav>
      {view === "books" ? (
        <WordbooksPanel
          device={device}
          data={data}
          setData={setData}
          notify={notify}
        />
      ) : (
        <>
          <div className="learning-overview">
            <section className="learning-progress card">
              <div
                className="learning-ring"
                role="img"
                aria-label={`累计学习 ${learned} 个，共 ${vocabularyCards.length} 个单词`}
                style={{ "--progress": `${percentage}%` } as CSSProperties}
              >
                <div>
                  <strong>
                    {percentage}
                    <small>%</small>
                  </strong>
                  <span>全库进度</span>
                </div>
              </div>
              <div>
                <h2>累计学习</h2>
                <p>
                  已学习 <strong>{learned}</strong> / {vocabularyCards.length}{" "}
                  个词
                </p>
                <button
                  type="button"
                  className="text-button"
                  onClick={() => setFilter("review")}
                >
                  查看待复习单词 <ArrowRight size={14} />
                </button>
              </div>
            </section>
            {[
              {
                label: demoDaily ? "本轮作答" : "今日新学",
                value: demoDaily ? progress.reviewedCount : plan.learnedToday,
                unit: demoDaily ? "次" : "词",
                note: demoDaily
                  ? "包含重复复习"
                  : `每日目标 ${data.wordbooks.dailyNewLimit} 词`,
                icon: BookOpen,
              },
              {
                label: "到期复习",
                value: dueWords.size,
                unit: "词",
                note: "优先复习，不占新词额度",
                icon: CircleCheck,
              },
              {
                label: "当前记住",
                value: known.size,
                unit: "词",
                note: "来自网页与设备自评",
                icon: RotateCw,
              },
            ].map(({ label, value, unit, note, icon: Icon }) => (
              <section className="card learning-stat" key={label}>
                <span>
                  <Icon size={18} strokeWidth={1.5} />
                  {label}
                </span>
                <strong>
                  {value}
                  <small>{unit}</small>
                </strong>
                <p>{note}</p>
              </section>
            ))}
          </div>
          <div className="learning-body">
            <section className="card word-library">
              <div className="feature-card-heading">
                <div>
                  <h2>英语单词背诵情况</h2>
                  <p>
                    {bookName} ·{" "}
                    {vocabularyCards.length.toLocaleString("zh-CN")} 词
                  </p>
                </div>
                <button
                  type="button"
                  className="button secondary"
                  onClick={exportProgress}
                >
                  <Download size={15} />
                  导出记录
                </button>
              </div>
              <div className="feature-toolbar">
                <div className="feature-segments" aria-label="词卡状态">
                  {(
                    [
                      {
                        id: "all",
                        label: "全部",
                        count: vocabularyCards.length,
                      },
                      { id: "review", label: "已到期", count: dueWords.size },
                      { id: "known", label: "已记住", count: known.size },
                    ] as const
                  ).map((item) => (
                    <button
                      type="button"
                      key={item.id}
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
                    placeholder="搜索单词或释义"
                    aria-label="搜索单词或释义"
                    value={search}
                    onChange={(event) => setSearch(event.target.value)}
                  />
                </label>
              </div>
              <div className="word-table-wrap">
                <table className="word-table">
                  <thead>
                    <tr>
                      <th scope="col">单词</th>
                      <th scope="col">中文释义</th>
                      <th scope="col">本轮状态</th>
                      <th scope="col">
                        <span className="sr-only">详情</span>
                      </th>
                    </tr>
                  </thead>
                  <tbody>
                    {pageWords.map((word) => (
                      <tr
                        key={word.word}
                        className={selected === word.word ? "is-selected" : ""}
                      >
                        <td>
                          <button
                            type="button"
                            onClick={() => setSelected(word.word)}
                            aria-label={`查看 ${word.word} 的词卡`}
                          >
                            <strong>{word.word}</strong>
                            <small>{word.phonetic}</small>
                          </button>
                        </td>
                        <td>{word.meaning}</td>
                        <td>
                          <span
                            className={`word-status ${known.has(word.word) ? "known" : "review"}`}
                          >
                            {known.has(word.word) ? (
                              <Check size={12} />
                            ) : (
                              <RotateCw size={12} />
                            )}
                            {known.has(word.word) ? "已掌握" : "待复习"}
                          </span>
                        </td>
                        <td>
                          <div className="word-row-actions">
                            <SaveWordButton
                              data={data}
                              setData={setData}
                              notify={notify}
                              card={word}
                              iconOnly
                            />
                            <button
                              type="button"
                              className="icon-button subtle"
                              onClick={() => setSelected(word.word)}
                              aria-label={`查看 ${word.word} 详情`}
                            >
                              <ArrowUpRight size={15} />
                            </button>
                          </div>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
                {words.length === 0 && (
                  <div className="feature-empty">
                    <Search size={25} />
                    <h3>没有找到这个词</h3>
                    <p>试试其他单词，或切换词卡状态。</p>
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
              <p className="feature-note" role="status">
                显示 {words.length} 个单词 · 当前为{data.deviceName}
                的学习记录
              </p>
            </section>
            <aside className="word-aside">
              {card && (
                <section className="card word-detail">
                  <div className="feature-card-heading">
                    <span className="feature-muted">单词详情</span>
                    <SaveWordButton
                      data={data}
                      setData={setData}
                      notify={notify}
                      card={card}
                      iconOnly
                    />
                  </div>
                  <span
                    className={`word-status ${known.has(card.word) ? "known" : "review"}`}
                  >
                    {known.has(card.word) ? "本轮已掌握" : "留待复习"}
                  </span>
                  <h2>{card.word}</h2>
                  <span className="word-phonetic">{card.phonetic}</span>
                  <h3>{card.meaning}</h3>
                  <div className="word-example">
                    <strong>例句</strong>
                    <p lang="en">{card.example}</p>
                    <small>{card.translation}</small>
                  </div>
                  <div className="word-detail-footer">
                    <span className="status-dot" />
                    来自 App 的同一组词卡
                  </div>
                  <SaveWordButton
                    data={data}
                    setData={setData}
                    notify={notify}
                    card={card}
                  />
                </section>
              )}
              <section className="learning-note">
                <Sparkles size={22} strokeWidth={1.5} />
                <h3>学习记录说明</h3>
                <p>
                  「已掌握」表示这一轮选择过记住；再次选择复习后，会回到待复习列表。
                </p>
                <p>
                  词卡与 App 同步，复习按遗忘曲线安排，离线记录会在联网后补传。
                </p>
              </section>
            </aside>
          </div>
        </>
      )}
    </div>
  );
}
