import catalog from "./catalogs/vocabulary.json";
import cet4Catalog from "./catalogs/cet4.json";
import {
  createMemorySchedule,
  normalizeMemorySchedule,
  scheduleAnswer,
  scheduledReviewWords,
  type MemorySchedule,
} from "./memorySchedule";

export type WordCard = (typeof catalog)[number];
export type BookWord = WordCard & {
  id?: number;
  remembered: boolean;
  review: MemorySchedule;
};
export type Wordbook = { id: string; name: string; words: BookWord[] };
export type Wordbooks = {
  activeBookId: string;
  dailyNewLimit: number;
  books: Wordbook[];
};
export const builtinBookId = "daily-english";
export const cet4BookId = "cet4";
export const savedBookId = "saved-words";
export const isBuiltinBook = (id: string) =>
  id === builtinBookId || id === cet4BookId;
export const bookWordKey = (id: string, word: string) =>
  id === cet4BookId
    ? word.normalize("NFKC").trim().replace(/\s+/g, " ")
    : wordKey(word);
export const maxWordbooks = 25;
const maxBookWords = 10000;
export const wordKey = (word: string) =>
  word.normalize("NFKC").trim().replace(/\s+/g, " ").toLowerCase();
const object = (value: unknown): Record<string, unknown> =>
  value && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};

export function createWordbooks(): Wordbooks {
  return {
    activeBookId: savedBookId,
    dailyNewLimit: 20,
    books: [
      {
        id: builtinBookId,
        name: "日常英语",
        words: catalog.map((card) => ({
          ...card,
          remembered: false,
          review: createMemorySchedule(),
        })),
      },
      { id: savedBookId, name: "我的生词本", words: [] },
      {
        id: cet4BookId,
        name: "大学英语四级",
        words: cet4Catalog.map((card) => ({
          ...card,
          example: "",
          translation: "",
          remembered: false,
          review: createMemorySchedule(),
        })),
      },
    ],
  };
}

export function validateBookName(name: string) {
  if (!name.trim()) throw new Error("给单词本起个名字吧。");
  if (name.trim().length > 24)
    throw new Error("词本名称请控制在 24 个字以内。");
  return name.trim();
}

export function validateWord(card: WordCard): WordCard {
  const word = card.word.trim(),
    meaning = card.meaning.trim();
  if (!word || !/[a-z]/i.test(word)) throw new Error("请填写英文单词或短语。");
  if (!meaning) throw new Error("请补充中文释义。");
  if (word.length > 80 || meaning.length > 300)
    throw new Error("单词或释义太长，请缩短一些。");
  if (
    card.phonetic.length > 120 ||
    card.example.length > 500 ||
    card.translation.length > 500
  )
    throw new Error("音标或例句太长，请缩短一些。");
  return {
    word,
    meaning,
    phonetic: card.phonetic.trim(),
    example: card.example.trim(),
    translation: card.translation.trim(),
  };
}

export function normalizeWordbooks(value: unknown): Wordbooks {
  const source = object(value),
    defaults = createWordbooks();
  const rawBooks = Array.isArray(source.books) ? source.books : [];
  function words(raw: unknown): BookWord[] {
    if (!Array.isArray(raw)) return [];
    const seen = new Set<string>();
    const result: BookWord[] = [];
    for (const entry of raw) {
      const item = object(entry);
      try {
        const card = validateWord({
          word: typeof item.word === "string" ? item.word : "",
          meaning: typeof item.meaning === "string" ? item.meaning : "",
          phonetic: typeof item.phonetic === "string" ? item.phonetic : "",
          example: typeof item.example === "string" ? item.example : "",
          translation:
            typeof item.translation === "string" ? item.translation : "",
        });
        const key = wordKey(card.word);
        if (seen.has(key)) continue;
        seen.add(key);
        result.push({
          ...card,
          remembered: item.remembered === true,
          review: normalizeMemorySchedule(item.review),
        });
        if (result.length >= maxBookWords) break;
      } catch {
        // Ignore an invalid card without losing the rest of a saved book.
      }
    }
    return result;
  }
  for (const builtin of defaults.books.filter((book) =>
    isBuiltinBook(book.id),
  )) {
    const stored = object(
      rawBooks.find((book) => object(book).id === builtin.id),
    );
    const entries = Array.isArray(stored.progress)
      ? stored.progress
      : Array.isArray(stored.words)
        ? stored.words
        : [];
    const byWord = new Map(
      entries.flatMap((entry): [string, Record<string, unknown>][] => {
        const card = object(entry);
        return typeof card.word === "string"
          ? [[bookWordKey(builtin.id, card.word), card]]
          : [];
      }),
    );
    builtin.words = builtin.words.map((card) => {
      const saved = byWord.get(bookWordKey(builtin.id, card.word));
      return {
        ...card,
        remembered: saved?.remembered === true,
        review: normalizeMemorySchedule(saved?.review),
      };
    });
  }
  const storedSaved = object(
    rawBooks.find((book) => object(book).id === savedBookId),
  );
  defaults.books[1].words = words(storedSaved.words);
  const ids = new Set([builtinBookId, cet4BookId, savedBookId]);
  for (const raw of rawBooks) {
    const book = object(raw);
    if (
      typeof book.id !== "string" ||
      !book.id.trim() ||
      book.id.length > 100 ||
      ids.has(book.id)
    )
      continue;
    if (
      typeof book.name !== "string" ||
      !book.name.trim() ||
      book.name.trim().length > 24
    )
      continue;
    ids.add(book.id);
    defaults.books.push({
      id: book.id,
      name: book.name.trim(),
      words: words(book.words),
    });
    if (defaults.books.length >= maxWordbooks) break;
  }
  defaults.activeBookId =
    typeof source.activeBookId === "string" && ids.has(source.activeBookId)
      ? source.activeBookId
      : savedBookId;
  if (
    Number.isInteger(source.dailyNewLimit) &&
    Number(source.dailyNewLimit) >= 1 &&
    Number(source.dailyNewLimit) <= 100
  )
    defaults.dailyNewLimit = Number(source.dailyNewLimit);
  return defaults;
}

export function addBook(state: Wordbooks, id: string, name: string): Wordbooks {
  const title = validateBookName(name);
  if (state.books.length >= maxWordbooks)
    throw new Error("单词本已满，可以先整理不再使用的词本。");
  if (!id || state.books.some((book) => book.id === id))
    throw new Error("请重新创建单词本。");
  if (
    state.books.some((book) => book.name.toLowerCase() === title.toLowerCase())
  )
    throw new Error("已经有同名的单词本了。");
  return {
    ...state,
    activeBookId: id,
    books: [...state.books, { id, name: title, words: [] }],
  };
}

export function renameBook(
  state: Wordbooks,
  id: string,
  name: string,
): Wordbooks {
  const title = validateBookName(name);
  if (isBuiltinBook(id) || id === savedBookId) return state;
  if (
    state.books.some(
      (book) =>
        book.id !== id && book.name.toLowerCase() === title.toLowerCase(),
    )
  )
    throw new Error("已经有同名的单词本了。");
  return {
    ...state,
    books: state.books.map((book) =>
      book.id === id ? { ...book, name: title } : book,
    ),
  };
}

export function deleteBook(state: Wordbooks, id: string): Wordbooks {
  if (isBuiltinBook(id) || id === savedBookId) return state;
  return {
    ...state,
    activeBookId: state.activeBookId === id ? savedBookId : state.activeBookId,
    books: state.books.filter((book) => book.id !== id),
  };
}

export function addBookWords(
  state: Wordbooks,
  id: string,
  cards: WordCard[],
): Wordbooks {
  if (isBuiltinBook(id)) return state;
  const checked = cards.map(validateWord);
  return {
    ...state,
    books: state.books.map((book) => {
      if (book.id !== id) return book;
      const keys = new Set(book.words.map((card) => wordKey(card.word)));
      const added = checked.filter((card) => {
        const key = wordKey(card.word);
        if (keys.has(key)) return false;
        keys.add(key);
        return true;
      });
      if (keys.size > maxBookWords)
        throw new Error("这个词本已经装了很多词，请新建一个词本。");
      return {
        ...book,
        words: [
          ...book.words,
          ...added.map((card) => ({
            ...card,
            remembered: false,
            review: createMemorySchedule(),
          })),
        ],
      };
    }),
  };
}

export function removeBookWord(
  state: Wordbooks,
  id: string,
  word: string,
): Wordbooks {
  if (isBuiltinBook(id)) return state;
  return {
    ...state,
    books: state.books.map((book) =>
      book.id === id
        ? {
            ...book,
            words: book.words.filter(
              (card) => wordKey(card.word) !== wordKey(word),
            ),
          }
        : book,
    ),
  };
}

export function answerBookWord(
  state: Wordbooks,
  id: string,
  word: string,
  remembered: boolean,
  now = Date.now(),
): Wordbooks {
  return {
    ...state,
    books: state.books.map((book) =>
      book.id === id
        ? {
            ...book,
            words: book.words.map((card) =>
              bookWordKey(id, card.word) === bookWordKey(id, word)
                ? {
                    ...card,
                    remembered,
                    review: scheduleAnswer(card.review, remembered, now),
                  }
                : card,
            ),
          }
        : book,
    ),
  };
}

export function hasBookWord(state: Wordbooks, id: string, word: string) {
  return !!state.books
    .find((book) => book.id === id)
    ?.words.some((card) => wordKey(card.word) === wordKey(word));
}

export function reviewWords(book: Wordbook, now = Date.now()): BookWord[] {
  return scheduledReviewWords(book.words, now);
}

export function wordbookCsv(book: Wordbook) {
  const safe = (value: string) => {
    const cell = /^[\s\uFEFF]*[=+\-@\t\r\n]/.test(value) ? `'${value}` : value;
    return `"${cell.replaceAll('"', '""')}"`;
  };
  return (
    "\uFEFF" +
    [
      [
        "单词",
        "音标",
        "释义",
        "英文例句",
        "例句翻译",
        "网页复习状态",
        "复习阶段",
        "下次复习时间",
        "复习次数",
        "遗忘次数",
      ],
      ...book.words.map((card) => [
        card.word,
        card.phonetic,
        card.meaning,
        card.example,
        card.translation,
        card.remembered ? "记住了" : "待复习",
        String(card.review.stage),
        card.review.nextReviewAt || "尚未学习",
        String(card.review.reviewCount),
        String(card.review.lapseCount),
      ]),
    ]
      .map((row) => row.map(safe).join(","))
      .join("\r\n")
  );
}

/** Keep queued edits local, then encode only the difference from the last server version. */
export function wordbookChanges(previous: Wordbooks, next: Wordbooks) {
  const changes: Record<string, unknown> = {};
  if (previous.activeBookId !== next.activeBookId)
    changes.activeBookId = next.activeBookId;
  if (previous.dailyNewLimit !== next.dailyNewLimit)
    changes.dailyNewLimit = next.dailyNewLimit;
  const before = new Map(previous.books.map((book) => [book.id, book]));
  const after = new Set(next.books.map((book) => book.id));
  const books: Record<string, unknown>[] = [];
  for (const book of previous.books)
    if (!after.has(book.id)) books.push({ id: book.id, deleted: true });
  for (const book of next.books) {
    const old = before.get(book.id);
    if (old === book) continue;
    const change: Record<string, unknown> = { id: book.id };
    if (book.name !== old?.name) change.name = book.name;
    const oldWords = new Map(
      (old?.words || []).map((card) => [bookWordKey(book.id, card.word), card]),
    );
    const nextWords = new Set(
      book.words.map((card) => bookWordKey(book.id, card.word)),
    );
    const remove = (old?.words || [])
      .filter((card) => !nextWords.has(bookWordKey(book.id, card.word)))
      .map((card) => card.word);
    const upsert: WordCard[] = [];
    for (const card of book.words) {
      const saved = oldWords.get(bookWordKey(book.id, card.word));
      if (saved === card) continue;
      const { word, meaning, phonetic, example, translation } = card;
      if (
        !saved ||
        word !== saved.word ||
        meaning !== saved.meaning ||
        phonetic !== saved.phonetic ||
        example !== saved.example ||
        translation !== saved.translation
      )
        upsert.push({ word, meaning, phonetic, example, translation });
    }
    if (remove.length) change.remove = remove;
    if (upsert.length) change.upsert = upsert;
    if (Object.keys(change).length > 1) books.push(change);
  }
  if (books.length) changes.books = books;
  return changes;
}
