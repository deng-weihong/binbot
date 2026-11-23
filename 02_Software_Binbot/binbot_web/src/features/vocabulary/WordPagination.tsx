import { ChevronLeft, ChevronRight } from "lucide-react";

export const wordPageSize = 30;

export function WordPagination({
  total,
  page,
  change,
}: {
  total: number;
  page: number;
  change: (page: number) => void;
}) {
  const pages = Math.max(1, Math.ceil(total / wordPageSize));
  return (
    <nav className="word-pagination" aria-label="单词列表分页">
      <span>
        共 {total.toLocaleString("zh-CN")} 词 · 第 {page + 1} / {pages} 页
      </span>
      <div>
        <button
          type="button"
          className="button secondary"
          disabled={page === 0}
          onClick={() => change(page - 1)}
          aria-label="上一页单词"
        >
          <ChevronLeft size={14} />
          上一页
        </button>
        <button
          type="button"
          className="button secondary"
          disabled={page >= pages - 1}
          onClick={() => change(page + 1)}
          aria-label="下一页单词"
        >
          下一页
          <ChevronRight size={14} />
        </button>
      </div>
    </nav>
  );
}
