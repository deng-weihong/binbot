import { AUTH_EXPIRED } from "../../shared/api/consoleApi";

export type CameraPhase = "connecting" | "playing" | "error";
export type CameraLease = { session_id: string; heartbeat_seconds: number };
export interface CameraTransport {
  open(deviceId: string): Promise<CameraLease>;
  refresh(
    deviceId: string,
    sessionId: string,
    signal: AbortSignal,
  ): Promise<void>;
  negotiate(
    deviceId: string,
    sessionId: string,
    offer: string,
    signal: AbortSignal,
  ): Promise<string>;
  close(deviceId: string, sessionId: string): Promise<void>;
}

export const cameraSessionPath = (deviceId: string, sessionId?: string) => {
  const path = `/api/v1/admin/devices/${encodeURIComponent(deviceId)}/camera/sessions`;
  return sessionId ? `${path}/${encodeURIComponent(sessionId)}` : path;
};

async function cameraJson(
  response: Response,
): Promise<Record<string, unknown>> {
  if (
    response.status === 401 &&
    typeof window !== "undefined" &&
    typeof window.dispatchEvent === "function"
  )
    window.dispatchEvent(new Event(AUTH_EXPIRED));
  if (response.status === 401 || response.status === 403)
    throw new Error("需要登录并获得这台设备的观看权限。");
  if (!response.headers.get("Content-Type")?.includes("application/json"))
    throw new Error(
      response.status === 503 ? "摄像头服务尚未启用。" : "摄像头服务尚未连接。",
    );
  const payload: unknown = await response.json();
  if (!payload || typeof payload !== "object" || Array.isArray(payload))
    throw new Error("摄像头服务返回了无效响应。");
  const value = payload as Record<string, unknown>;
  if (!response.ok)
    throw new Error(
      typeof value.error === "string"
        ? value.error
        : "视频连接暂不可用，请重试。",
    );
  return value;
}

export function createCameraTransport(
  request: typeof fetch = fetch,
): CameraTransport {
  return {
    async open(deviceId) {
      const result = await cameraJson(
        await request(cameraSessionPath(deviceId), {
          method: "POST",
          credentials: "same-origin",
          headers: { Accept: "application/json" },
        }),
      );
      if (
        typeof result.session_id !== "string" ||
        !result.session_id ||
        typeof result.heartbeat_seconds !== "number" ||
        !Number.isFinite(result.heartbeat_seconds) ||
        result.heartbeat_seconds < 1 ||
        result.heartbeat_seconds > 60
      )
        throw new Error("摄像头观看会话无效，请重新连接。");
      return {
        session_id: result.session_id,
        heartbeat_seconds: result.heartbeat_seconds,
      };
    },
    async refresh(deviceId, sessionId, signal) {
      await cameraJson(
        await request(cameraSessionPath(deviceId, sessionId), {
          method: "PUT",
          credentials: "same-origin",
          headers: { Accept: "application/json" },
          signal,
        }),
      );
    },
    async negotiate(deviceId, sessionId, offer, signal) {
      const response = await request(
        `${cameraSessionPath(deviceId, sessionId)}/webrtc`,
        {
          method: "POST",
          credentials: "same-origin",
          headers: { "Content-Type": "application/sdp" },
          body: offer,
          signal,
        },
      );
      if (
        !response.ok ||
        !response.headers.get("Content-Type")?.includes("application/sdp")
      ) {
        await cameraJson(response);
        throw new Error("视频连接暂不可用，请重试。");
      }
      const answer = await response.text();
      if (!answer.startsWith("v=0") || answer.length > 65536)
        throw new Error("摄像头视频响应无效，请重新连接。");
      return answer;
    },
    async close(deviceId, sessionId) {
      await request(cameraSessionPath(deviceId, sessionId), {
        method: "DELETE",
        credentials: "same-origin",
        keepalive: true,
      });
    },
  };
}

function browserPeer() {
  if (!window.isSecureContext || !window.RTCPeerConnection)
    throw new Error("请通过 HTTPS 或本机地址打开页面后连接摄像头。");
  return new RTCPeerConnection({ iceServers: [] });
}

async function gatherCandidates(peer: RTCPeerConnection, signal: AbortSignal) {
  if (signal.aborted || peer.iceGatheringState === "complete") return;
  await new Promise<void>((resolve) => {
    const finish = () => {
      clearTimeout(timeout);
      peer.removeEventListener("icegatheringstatechange", changed);
      signal.removeEventListener("abort", finish);
      resolve();
    };
    const changed = () => {
      if (peer.iceGatheringState === "complete") finish();
    };
    const timeout = setTimeout(finish, 4000);
    peer.addEventListener("icegatheringstatechange", changed);
    signal.addEventListener("abort", finish, { once: true });
    if (signal.aborted) finish();
  });
}

/** One viewer owns exactly one device-scoped lease and releases it on exit. */
export class CameraConnection {
  private readonly transport: CameraTransport;
  private readonly createPeer: () => RTCPeerConnection;
  private readonly controller = new AbortController();
  private peer: RTCPeerConnection | null = null;
  private sessionId = "";
  private started = false;
  private disposed = false;
  private heartbeat?: ReturnType<typeof setInterval>;
  private deadline?: ReturnType<typeof setTimeout>;
  private disconnected?: ReturnType<typeof setTimeout>;

  constructor(
    private readonly deviceId: string,
    private readonly callbacks: {
      phase: (phase: CameraPhase, message: string) => void;
      stream: (stream: MediaStream | null) => void;
    },
    options: {
      transport?: CameraTransport;
      createPeer?: () => RTCPeerConnection;
    } = {},
  ) {
    this.transport = options.transport ?? createCameraTransport();
    this.createPeer = options.createPeer ?? browserPeer;
  }

  async start() {
    if (this.started || this.disposed) return;
    this.started = true;
    this.callbacks.phase("connecting", "");
    this.deadline = setTimeout(
      () => this.fail(new Error("等待视频超时，请重新连接。")),
      30000,
    );
    try {
      const peer = this.createPeer();
      this.peer = peer;
      const transceiver = peer.addTransceiver("video", {
        direction: "recvonly",
      });
      const codecs =
        typeof RTCRtpReceiver !== "undefined"
          ? RTCRtpReceiver.getCapabilities?.("video")?.codecs.filter(
              (codec) => codec.mimeType.toLowerCase() === "video/h264",
            )
          : undefined;
      if (codecs?.length && transceiver.setCodecPreferences)
        transceiver.setCodecPreferences(codecs);
      peer.ontrack = (event) => {
        if (!this.disposed)
          this.callbacks.stream(
            event.streams[0] || new MediaStream([event.track]),
          );
      };
      peer.onconnectionstatechange = () => {
        if (this.disposed) return;
        if (
          peer.connectionState === "failed" ||
          peer.connectionState === "closed"
        )
          this.fail(new Error("视频连接已断开，请重新连接。"));
        else if (peer.connectionState === "disconnected" && !this.disconnected)
          this.disconnected = setTimeout(
            () => this.fail(new Error("视频连接已断开，请重新连接。")),
            6000,
          );
        else if (peer.connectionState === "connected") {
          clearTimeout(this.disconnected);
          this.disconnected = undefined;
        }
      };
      const session = await this.transport.open(this.deviceId);
      if (this.disposed) {
        await this.transport
          .close(this.deviceId, session.session_id)
          .catch(() => {});
        return;
      }
      this.sessionId = session.session_id;
      this.heartbeat = setInterval(() => {
        void this.transport
          .refresh(this.deviceId, session.session_id, this.controller.signal)
          .catch((error: unknown) => this.fail(error));
      }, session.heartbeat_seconds * 1000);
      await peer.setLocalDescription(await peer.createOffer());
      if (this.disposed) return;
      await gatherCandidates(peer, this.controller.signal);
      if (this.disposed) return;
      const offer = peer.localDescription?.sdp;
      if (!offer) throw new Error("无法建立视频请求，请重试。");
      const answer = await this.transport.negotiate(
        this.deviceId,
        session.session_id,
        offer,
        this.controller.signal,
      );
      if (!this.disposed)
        await peer.setRemoteDescription({ type: "answer", sdp: answer });
    } catch (error) {
      this.fail(error);
    }
  }

  markPlaying() {
    if (this.disposed) return;
    clearTimeout(this.deadline);
    this.callbacks.phase("playing", "");
  }

  fail(error: unknown) {
    if (this.disposed) return;
    this.dispose();
    this.callbacks.stream(null);
    this.callbacks.phase(
      "error",
      error instanceof Error ? error.message : "视频连接暂不可用，请重试。",
    );
  }

  dispose() {
    if (this.disposed) return;
    this.disposed = true;
    clearInterval(this.heartbeat);
    clearTimeout(this.deadline);
    clearTimeout(this.disconnected);
    this.controller.abort();
    if (this.peer) {
      this.peer.ontrack = null;
      this.peer.onconnectionstatechange = null;
      this.peer.close();
    }
    if (this.sessionId) {
      const sessionId = this.sessionId;
      this.sessionId = "";
      void this.transport.close(this.deviceId, sessionId).catch(() => {});
    }
  }
}
