export type ChatMessage = { role: "assistant" | "user"; content: string };

function object(value: unknown): Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value)
    ? (value as Record<string, unknown>)
    : {};
}

export function parseDeviceChat(
  value: unknown,
  deviceId: string,
): ChatMessage[] {
  const row = object(value);
  if (row.device_id !== deviceId)
    throw new Error("返回的对话与当前设备不一致。");
  if (!Array.isArray(row.messages)) throw new Error("对话记录格式不正确。");
  return row.messages.map((item) => {
    const { role, content } = object(item);
    if (
      (role !== "user" && role !== "assistant") ||
      typeof content !== "string"
    )
      throw new Error("对话记录格式不正确。");
    return { role, content };
  });
}

export type AssistantTransport = {
  chatHistory(deviceId: string, signal?: AbortSignal): Promise<ChatMessage[]>;
  sendChat(
    deviceId: string,
    text: string,
    requestId: string,
    signal?: AbortSignal,
  ): Promise<ChatMessage[]>;
};

export type ConversationState = {
  messages: ChatMessage[];
  loading: boolean;
  sending: boolean;
  syncError: boolean;
  error: string;
};
export const emptyConversation = (): ConversationState => ({
  messages: [],
  loading: true,
  sending: false,
  syncError: false,
  error: "",
});

export class ConversationController {
  private value = emptyConversation();
  private request?: AbortController;
  private disposed = false;
  private pending?: { text: string; id: string };

  constructor(
    private readonly deviceId: string,
    private readonly transport: AssistantTransport,
    private readonly changed: (value: ConversationState) => void,
  ) {}

  private publish(patch: Partial<ConversationState>) {
    this.value = { ...this.value, ...patch };
    this.changed(this.value);
  }

  async load() {
    if (this.disposed || this.request) return;
    const request = new AbortController();
    this.request = request;
    try {
      const messages = await this.transport.chatHistory(
        this.deviceId,
        request.signal,
      );
      if (request.signal.aborted) return;
      const previous = this.value.messages;
      const unchanged =
        previous.length === messages.length &&
        previous.every(
          (message, index) =>
            message.role === messages[index].role &&
            message.content === messages[index].content,
        );
      if (!unchanged || this.value.loading || this.value.syncError)
        this.publish({
          messages: unchanged ? previous : messages,
          loading: false,
          syncError: false,
        });
    } catch {
      if (!request.signal.aborted)
        this.publish({ loading: false, syncError: true });
    } finally {
      if (this.request === request) this.request = undefined;
    }
  }

  async send(text: string): Promise<boolean> {
    text = text.trim();
    if (this.disposed || this.value.loading || this.value.sending || !text)
      return false;
    if (text.length > 4000) {
      this.publish({ error: "消息不能超过 4000 字。" });
      return false;
    }
    this.request?.abort();
    const request = new AbortController();
    this.request = request;
    const messages = this.value.messages;
    if (this.pending?.text !== text)
      this.pending = { text, id: crypto.randomUUID() };
    this.publish({
      sending: true,
      error: "",
      messages: [...messages, { role: "user", content: text }],
    });
    try {
      const reply = await this.transport.sendChat(
        this.deviceId,
        text,
        this.pending.id,
        request.signal,
      );
      if (request.signal.aborted) return false;
      this.pending = undefined;
      this.publish({ messages: reply, syncError: false });
      return true;
    } catch (error) {
      if (!request.signal.aborted)
        this.publish({
          messages,
          error:
            error instanceof Error ? error.message : "消息未能发送，请重试。",
        });
      return false;
    } finally {
      if (this.request === request) this.request = undefined;
      if (!request.signal.aborted) this.publish({ sending: false });
    }
  }

  dispose() {
    this.disposed = true;
    this.request?.abort();
  }
}
