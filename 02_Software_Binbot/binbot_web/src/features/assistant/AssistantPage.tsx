import { useEffect, useRef, useState } from "react";
import {
  ArrowUp,
  ArrowUpRight,
  LoaderCircle,
  RefreshCw,
  Smile,
  Sparkles,
} from "lucide-react";
import type { Device, DeviceData } from "../../shared/model/data";
import { useConsole } from "../../shared/state/consoleContext";
import { createConsoleTransport } from "../../shared/api/consoleApi";
import {
  ConversationController,
  emptyConversation,
  type ChatMessage,
} from "../../shared/model/assistantConversation";

const suggestions = ["陪我练一句英语", "帮我理理今天的事", "讲个轻松的小故事"];
const chatRefreshMs = 3000;

function demoReply(text: string) {
  if (/英语|英文|单词/.test(text))
    return "试试这一句：Take your time. 意思是「慢慢来」。你会在什么场景用到它？";
  if (/故事/.test(text))
    return "小机器人每天给窗台的花浇水，却一直没等到它开花。有一天，它终于发现，那其实是一盆薄荷。原来有些陪伴，不用等开花，就已经很美好了。";
  if (/今天|安排|计划/.test(text))
    return "先选一件今天最想完成的小事吧。把它告诉我，我们一起拆成几个简单的步骤。";
  return "我在听。可以再多说一点，你现在最想聊的是什么？";
}

export default function AssistantPage({
  device,
  data,
}: {
  device: Device;
  data: DeviceData;
}) {
  const { mode } = useConsole();
  const live = mode === "live";
  const [input, setInput] = useState("");
  const [remote, setRemote] = useState(emptyConversation);
  const [messages, setMessages] = useState<ChatMessage[]>([]);
  const controller = useRef<ConversationController | null>(null);
  const conversation = useRef<HTMLDivElement>(null);
  const followLatest = useRef(true);
  const name = data.assistant.name || data.deviceName;
  const shownMessages = live ? remote.messages : messages;
  const loading = live && remote.loading;
  const sending = live && remote.sending;
  const syncError = live && remote.syncError;

  useEffect(() => {
    if (!live) return;
    const value = new ConversationController(
      device.id,
      createConsoleTransport(),
      setRemote,
    );
    controller.current = value;
    setRemote(emptyConversation());
    const refresh = () => {
      if (!document.hidden) void value.load();
    };
    refresh();
    const timer = window.setInterval(refresh, chatRefreshMs);
    document.addEventListener("visibilitychange", refresh);
    return () => {
      window.clearInterval(timer);
      document.removeEventListener("visibilitychange", refresh);
      value.dispose();
      controller.current = null;
    };
  }, [device.id, live]);

  useEffect(() => {
    const element = conversation.current;
    if (element && followLatest.current)
      element.scrollTop = element.scrollHeight;
  }, [shownMessages, sending]);

  function send(text = input) {
    const trimmed = text.trim();
    if (!trimmed || loading || sending) return;
    followLatest.current = true;
    if (live) {
      const current = controller.current;
      if (!current) return;
      setInput(trimmed);
      void current.send(trimmed).then((sent) => {
        if (sent && controller.current === current) setInput("");
      });
      return;
    }
    setMessages((previous) => [
      ...previous,
      { role: "user", content: trimmed },
      { role: "assistant", content: demoReply(trimmed) },
    ]);
    setInput("");
  }

  return (
    <section
      className="card assistant-chat"
      aria-label={`和${name}聊天`}
      data-device-id={device.id}
    >
      <header className="assistant-chat-header">
        <span className="assistant-chat-avatar" aria-hidden="true">
          <Smile size={23} strokeWidth={1.5} />
        </span>
        <h2>{name}</h2>
        <span className="assistant-chat-location">{device.location}</span>
        {!live && <span className="assistant-chat-demo">演示对话</span>}
      </header>
      {syncError && (
        <div className="assistant-sync-error" role="status">
          <p>暂时无法同步聊天记录。</p>
          <button
            type="button"
            className="button secondary"
            disabled={sending}
            onClick={() => void controller.current?.load()}
          >
            <RefreshCw size={14} /> 重试
          </button>
        </div>
      )}
      <div
        className="assistant-chat-body"
        ref={conversation}
        tabIndex={0}
        onScroll={(event) => {
          const { scrollTop, scrollHeight, clientHeight } = event.currentTarget;
          followLatest.current = scrollHeight - scrollTop - clientHeight < 40;
        }}
      >
        {loading && (
          <div className="assistant-history-empty" role="status">
            <LoaderCircle size={22} className="spinning" />
            <p>正在读取聊天记录…</p>
          </div>
        )}
        {!shownMessages.length && !loading && !syncError && (
          <div className="assistant-chat-welcome">
            <span className="assistant-chat-welcome-icon" aria-hidden="true">
              <Sparkles size={27} strokeWidth={1.4} />
            </span>
            <h3>今天想聊点什么？</h3>
            <div className="assistant-chat-suggestions">
              {suggestions.map((text) => (
                <button key={text} onClick={() => send(text)}>
                  {text}
                  <ArrowUpRight size={14} aria-hidden="true" />
                </button>
              ))}
            </div>
          </div>
        )}
        <div
          className="assistant-chat-messages"
          role="log"
          aria-label="聊天记录"
          aria-live="polite"
          aria-relevant="additions text"
        >
          {shownMessages.map(({ role, content }, index) => (
            <div className={`assistant-chat-message ${role}`} key={index}>
              {role === "assistant" && (
                <span
                  className="assistant-chat-message-icon"
                  aria-hidden="true"
                >
                  <Sparkles size={15} strokeWidth={1.5} />
                </span>
              )}
              <p>{content}</p>
            </div>
          ))}
          {sending && (
            <div className="assistant-chat-message assistant" role="status">
              <LoaderCircle size={16} className="spinning" />
              <p>正在回复…</p>
            </div>
          )}
        </div>
      </div>
      <div className="assistant-chat-bottom">
        {live && remote.error && (
          <p className="assistant-chat-error" role="alert">
            {remote.error}
          </p>
        )}
        <form
          className="assistant-chat-composer"
          onSubmit={(event) => {
            event.preventDefault();
            send();
          }}
        >
          <input
            value={input}
            onChange={(event) => setInput(event.target.value)}
            onKeyDown={(event) => {
              if (
                event.key === "Enter" &&
                (event.nativeEvent.isComposing ||
                  event.nativeEvent.keyCode === 229)
              )
                event.preventDefault();
            }}
            maxLength={4000}
            disabled={loading || sending}
            aria-label="和助手说点什么"
            placeholder="输入消息…"
          />
          <button
            type="submit"
            aria-label="发送消息"
            title="发送消息"
            disabled={!input.trim() || loading || sending}
          >
            <ArrowUp size={19} aria-hidden="true" />
          </button>
        </form>
      </div>
    </section>
  );
}
