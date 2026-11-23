import { validateWord, wordKey, type WordCard } from "../../shared/model/wordbooks";

/** CSV quoting, TSV and the console's exported Chinese headers are supported. */
export function parseWordImport(source: string): WordCard[] {
  if (source.length > 2 * 1024 * 1024)
    throw new Error("请导入不超过 2 MB 的词表。");
  const text = source.replace(/^\uFEFF/, ""),
    separator = text.split(/\r?\n/, 1)[0].includes("\t") ? "\t" : ",";
  const rows: string[][] = [];
  let row: string[] = [],
    cell = "",
    quoted = false;
  for (let i = 0; i < text.length; i++) {
    const char = text[i];
    if (char === '"') {
      if (quoted && text[i + 1] === '"') {
        cell += '"';
        i++;
      } else if (quoted || !cell) quoted = !quoted;
      else throw new Error("词表中的引号格式不正确。");
    } else if (
      !quoted &&
      (char === separator || char === "\n" || char === "\r")
    ) {
      row.push(cell);
      cell = "";
      if (char !== separator) {
        if (row.some((value) => value.trim())) rows.push(row);
        row = [];
        if (char === "\r" && text[i + 1] === "\n") i++;
      }
    } else cell += char;
  }
  if (quoted) throw new Error("词表中有未结束的引号。");
  row.push(cell);
  if (row.some((value) => value.trim())) rows.push(row);
  if (!rows.length) throw new Error("词表里没有单词。");
  const names: Record<keyof WordCard, string[]> = {
    word: ["word", "单词", "英文单词"],
    meaning: ["meaning", "释义", "中文释义"],
    phonetic: ["phonetic", "音标"],
    example: ["example", "英文例句", "例句"],
    translation: ["translation", "例句翻译"],
  };
  const header = rows[0].map((value) => value.trim().toLowerCase());
  const hasHeader = names.word.some((name) => header.includes(name));
  const defaults: Record<keyof WordCard, number> = {
    word: 0,
    meaning: 1,
    phonetic: 2,
    example: 3,
    translation: 4,
  };
  const indexes = Object.fromEntries(
    Object.entries(names).map(([key, aliases]) => [
      key,
      hasHeader
        ? header.findIndex((value) => aliases.includes(value))
        : defaults[key as keyof WordCard],
    ]),
  ) as Record<keyof WordCard, number>;
  if (indexes.word < 0 || indexes.meaning < 0)
    throw new Error("词表需要包含单词和释义两列。");
  const seen = new Set<string>(),
    cards: WordCard[] = [];
  for (const [index, values] of rows.slice(hasHeader ? 1 : 0).entries()) {
    try {
      const card = validateWord(
        Object.fromEntries(
          Object.entries(indexes).map(([key, column]) => [
            key,
            column < 0 ? "" : values[column] || "",
          ]),
        ) as WordCard,
      );
      if (!seen.has(wordKey(card.word))) {
        seen.add(wordKey(card.word));
        cards.push(card);
      }
    } catch (error) {
      throw new Error(
        `第 ${index + (hasHeader ? 2 : 1)} 行：${error instanceof Error ? error.message : "格式不正确"}`,
      );
    }
    if (cards.length > 10000) throw new Error("每次最多导入 10000 个单词。");
  }
  return cards;
}
