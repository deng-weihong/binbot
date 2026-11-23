import { useWorkspace } from "../../shared/state/workspaceContext";
import { useRef, useState, type Dispatch, type SetStateAction } from "react";
import { Check, ChevronDown, ListChecks, Plus, Trash2, X } from "lucide-react";
import { uid, type DeviceData } from "../../shared/model/data";
import { maxTodos, validTodoText, type Todo } from "../../shared/model/todos";

export default function HomeTodoList({
  data,
  setData,
  notify,
}: {
  data: DeviceData;
  setData: Dispatch<SetStateAction<DeviceData>>;
  notify: (message: string) => void;
}) {
  const workspace = useWorkspace();
  const inputRef = useRef<HTMLInputElement | null>(null);
  const [text, setText] = useState("");
  const [editing, setEditing] = useState<string | null>(null);
  const [editedText, setEditedText] = useState("");
  const [error, setError] = useState("");
  const pending = data.todos.filter((item) => !item.done);
  const completed = data.todos.filter((item) => item.done);
  const full = data.todos.length >= maxTodos;

  function row(todo: Todo) {
    return (
      <li
        className={`home-todo-row ${todo.done ? "is-done" : ""}`}
        key={todo.id}
      >
        <label className="home-todo-check">
          <input
            type="checkbox"
            checked={todo.done}
            aria-label={`${todo.done ? "恢复" : "完成"}待办：${todo.text}`}
            onChange={() => {
              setData((previous) => ({
                ...previous,
                todos: previous.todos.map((item) =>
                  item.id === todo.id ? { ...item, done: !item.done } : item,
                ),
              }));
              if (editing === todo.id) setEditing(null);
            }}
          />
          <span>{todo.done && <Check size={12} />}</span>
        </label>
        {editing === todo.id ? (
          <form
            className="home-todo-edit"
            onSubmit={(event) => {
              event.preventDefault();
              if (!validTodoText(editedText)) {
                setError(
                  editedText.trim()
                    ? "内容有点长，请缩短一些。"
                    : "写下要做的事再保存。",
                );
                return;
              }
              setData((previous) => ({
                ...previous,
                todos: previous.todos.map((item) =>
                  item.id === todo.id
                    ? { ...item, text: editedText.trim() }
                    : item,
                ),
              }));
              setEditing(null);
              setError("");
            }}
          >
            <input
              autoFocus
              aria-label={`修改待办：${todo.text}`}
              value={editedText}
              maxLength={71}
              onChange={(event) => {
                setEditedText(event.target.value);
                setError("");
              }}
              onKeyDown={(event) => {
                if (
                  event.nativeEvent.isComposing ||
                  event.nativeEvent.keyCode === 229
                ) {
                  if (event.key === "Enter") event.preventDefault();
                  return;
                }
                if (event.key === "Escape") {
                  setEditing(null);
                  setError("");
                }
              }}
            />
            <button
              type="submit"
              aria-label="保存待办修改"
              disabled={!editedText.trim()}
            >
              <Check size={14} />
            </button>
            <button
              type="button"
              aria-label="取消待办修改"
              onClick={() => {
                setEditing(null);
                setError("");
              }}
            >
              <X size={14} />
            </button>
          </form>
        ) : (
          <button
            type="button"
            className="home-todo-text"
            aria-label={`修改待办：${todo.text}`}
            title="点击修改"
            onClick={() => {
              setEditing(todo.id);
              setEditedText(todo.text);
              setError("");
            }}
          >
            {todo.text}
          </button>
        )}
        <button
          type="button"
          className="home-todo-delete"
          aria-label={`删除待办：${todo.text}`}
          onClick={() => {
            setData((previous) => ({
              ...previous,
              todos: previous.todos.filter((item) => item.id !== todo.id),
            }));
            if (editing === todo.id) setEditing(null);
            setError("");
            notify("待办已删除");
          }}
        >
          <Trash2 size={13} />
        </button>
      </li>
    );
  }

  return (
    <section
      className="card home-todo-panel"
      id="home-todos"
      aria-labelledby="home-todos-title"
    >
      <header className="card-heading">
        <h2 id="home-todos-title">
          待办事项{" "}
          <span className="count-badge" aria-label="未完成待办数">
            {pending.length}
          </span>
        </h2>
      </header>
      <form
        className="home-todo-add"
        onSubmit={(event) => {
          event.preventDefault();
          if (full) return;
          if (!validTodoText(text)) {
            setError(
              text.trim() ? "内容有点长，请缩短一些。" : "先写下一件想做的事。",
            );
            return;
          }
          const todo = { id: uid(), text: text.trim(), done: false };
          setData((previous) => ({
            ...previous,
            todos:
              previous.todos.length < maxTodos
                ? [...previous.todos, todo]
                : previous.todos,
          }));
          setText("");
          setError("");
          inputRef.current?.focus();
        }}
      >
        <input
          ref={inputRef}
          aria-label="新待办内容"
          aria-describedby={error ? "home-todo-error" : undefined}
          placeholder={full ? "清单已满，删除旧事项后再添加" : "添加一条待办"}
          value={text}
          maxLength={71}
          disabled={full}
          onChange={(event) => {
            setText(event.target.value);
            setError("");
          }}
          onKeyDown={(event) => {
            if (
              event.key === "Enter" &&
              (event.nativeEvent.isComposing ||
                event.nativeEvent.keyCode === 229)
            )
              event.preventDefault();
          }}
        />
        <button
          type="submit"
          disabled={full || !text.trim()}
          aria-label="添加待办"
        >
          <Plus size={14} />
        </button>
      </form>
      {error && (
        <p className="home-todo-error" role="alert" id="home-todo-error">
          {error}
        </p>
      )}
      <div className="home-todo-scroll">
        {pending.length ? (
          <ul className="home-todo-items" aria-label="未完成待办">
            {pending.map(row)}
          </ul>
        ) : (
          <div className="home-todo-empty">
            <ListChecks size={24} strokeWidth={1.4} />
            <strong>{completed.length ? "待办已全部完成" : "暂无待办"}</strong>
          </div>
        )}
        {!!completed.length && (
          <details className="home-todo-completed">
            <summary>
              已完成 <span>{completed.length}</span>
              <ChevronDown size={13} />
            </summary>
            <ul className="home-todo-items" aria-label="已完成待办">
              {completed.map(row)}
            </ul>
          </details>
        )}
      </div>
      <p className="home-todo-note">
        {workspace.live ? "与设备同步" : "保存在当前浏览器"} ·{" "}
        {data.todos.length}/{maxTodos} 项
      </p>
    </section>
  );
}
