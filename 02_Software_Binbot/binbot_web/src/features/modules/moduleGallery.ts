import { createConsoleTransport, type UploadProgress } from "../../shared/api/consoleApi";
const galleryApi = createConsoleTransport();
const liveDevice = (scope: string) =>
  scope.startsWith("live:") ? scope.slice(5) : "";
export const MAX_IMAGE_BYTES = 16 * 1024 * 1024;
export type GalleryAsset = {
  key: string;
  scope: string;
  id: string;
  name: string;
  blob: Blob;
  sourceUrl?: string;
  serverId?: string;
  createdAt: number;
  delivery?: {
    commandId: string;
    status: string;
    message?: string;
    at: number;
  };
  push?: {
    jobId: string;
    status: string;
    percent: number;
    written?: number;
    total?: number;
    message?: string;
    at: number;
  };
};
export const galleryScope = (mode: "demo" | "live", deviceId: string) =>
  `${mode}:${deviceId}`;

export async function validateImageFile(file: Blob) {
  if (!file.size || file.size > MAX_IMAGE_BYTES)
    throw new Error("请选择不超过 16 MB 的图片。");
  const bytes = new Uint8Array(await file.slice(0, 16).arrayBuffer());
  const text = String.fromCharCode(...bytes);
  if (bytes[0] === 0x89 && text.slice(1, 8) === "PNG\r\n\x1a\n")
    return "image/png";
  if (text.startsWith("GIF87a") || text.startsWith("GIF89a"))
    return "image/gif";
  if (bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff)
    return "image/jpeg";
  if (text.startsWith("RIFF") && text.slice(8, 12) === "WEBP")
    return "image/webp";
  throw new Error("请选择有效的 PNG、GIF、JPEG 或 WebP 图片。");
}

/** Check decoding before retaining a user supplied image; always release its URL. */
export function verifyImagePreview(
  file: Blob,
  signal?: AbortSignal,
): Promise<void> {
  if (signal?.aborted) return Promise.reject(signal.reason);
  return new Promise((resolve, reject) => {
    const image = new Image();
    const url = URL.createObjectURL(file);
    const finish = (error?: unknown) => {
      clearTimeout(timer);
      signal?.removeEventListener("abort", abort);
      image.onload = image.onerror = null;
      image.src = "";
      URL.revokeObjectURL(url);
      if (error) reject(error);
      else resolve();
    };
    const abort = () => finish(signal?.reason || new Error("图片读取已取消。"));
    const timer = setTimeout(
      () => finish(new Error("图片读取超时，请换一张图片。")),
      15000,
    );
    image.onload = () =>
      finish(
        image.naturalWidth && image.naturalHeight
          ? undefined
          : new Error("这张图片无法读取，请换一张试试。"),
      );
    image.onerror = () =>
      finish(new Error("这张图片已损坏或无法读取，请换一张试试。"));
    signal?.addEventListener("abort", abort, { once: true });
    image.src = url;
  });
}

let databasePromise: Promise<IDBDatabase> | undefined;
function database(): Promise<IDBDatabase> {
  if (!databasePromise)
    databasePromise = new Promise((resolve, reject) => {
      if (typeof indexedDB === "undefined") {
        reject(new Error("当前浏览器不支持保存图片。"));
        return;
      }
      const request = indexedDB.open("binbot-module-gallery-v1", 1);
      request.onupgradeneeded = () => {
        const store = request.result.createObjectStore("images", {
          keyPath: "key",
        });
        store.createIndex("scope", "scope");
      };
      request.onsuccess = () => {
        const db = request.result;
        db.onversionchange = () => {
          db.close();
          databasePromise = undefined;
        };
        resolve(db);
      };
      request.onerror = () => {
        databasePromise = undefined;
        reject(new Error("浏览器无法打开图片库，请检查存储设置。"));
      };
      request.onblocked = () => {
        databasePromise = undefined;
        reject(new Error("图片库正在被其他窗口更新，请关闭旧窗口后重试。"));
      };
    });
  return databasePromise;
}

export async function listGalleryAssets(
  scope: string,
): Promise<GalleryAsset[]> {
  const device = liveDevice(scope);
  if (device) {
    const result = await galleryApi.gallery(device);
    if (result.device_id !== device) throw new Error("图库与当前设备不一致。");
    return result.images.map(
      (row) =>
        ({
          ...row,
          id: String(row.id),
          serverId: String(row.assetId),
          name: String(row.name),
          createdAt: Number(row.createdAt),
          scope,
          key: `${scope}:${row.id}`,
          blob: new Blob(),
          sourceUrl: `/api/v1/admin/devices/${encodeURIComponent(device)}/gallery/${encodeURIComponent(String(row.id))}/source`,
        }) as GalleryAsset,
    );
  }
  const db = await database();
  return new Promise((resolve, reject) => {
    const transaction = db.transaction("images", "readonly");
    const request = transaction
      .objectStore("images")
      .index("scope")
      .getAll(scope);
    transaction.oncomplete = () =>
      resolve(
        (request.result as GalleryAsset[])
          .filter((item) => item.scope === scope && item.blob instanceof Blob)
          .sort((a, b) => b.createdAt - a.createdAt),
      );
    transaction.onabort = transaction.onerror = () =>
      reject(new Error("图片库读取失败，请重试。"));
  });
}

export async function putGalleryAsset(
  asset: GalleryAsset,
  signal?: AbortSignal,
  onUpload?: UploadProgress,
) {
  const device = liveDevice(asset.scope);
  if (device) {
    const result = await galleryApi.storeGallery(
      device,
      asset.id,
      asset.blob,
      asset.name,
      signal,
      onUpload,
    );
    asset.serverId = String(result.assetId);
    asset.sourceUrl = `/api/v1/admin/devices/${encodeURIComponent(device)}/gallery/${encodeURIComponent(asset.id)}/source`;
    return;
  }
  const db = await database();
  return new Promise<void>((resolve, reject) => {
    const transaction = db.transaction("images", "readwrite");
    transaction.objectStore("images").put(asset);
    transaction.oncomplete = () => resolve();
    transaction.onabort = transaction.onerror = () =>
      reject(new Error("图片未能保存，浏览器存储可能已满。"));
  });
}

export async function patchGalleryAsset(
  scope: string,
  id: string,
  patch: Partial<Pick<GalleryAsset, "name" | "delivery" | "push">>,
) {
  const device = liveDevice(scope);
  if (device) {
    await galleryApi.patchGallery(device, id, patch);
    return;
  }
  const db = await database();
  return new Promise<void>((resolve, reject) => {
    const transaction = db.transaction("images", "readwrite"),
      store = transaction.objectStore("images");
    const request = store.get(`${scope}:${id}`);
    request.onsuccess = () => {
      if (request.result?.scope === scope)
        store.put({ ...request.result, ...patch });
    };
    transaction.oncomplete = () => resolve();
    transaction.onabort = transaction.onerror = () =>
      reject(new Error("图片信息保存失败，请重试。"));
  });
}

export async function removeGalleryAsset(scope: string, id: string) {
  const device = liveDevice(scope);
  if (device) {
    await galleryApi.removeGallery(device, id);
    return;
  }
  const db = await database();
  return new Promise<void>((resolve, reject) => {
    const transaction = db.transaction("images", "readwrite");
    transaction.objectStore("images").delete(`${scope}:${id}`);
    transaction.oncomplete = () => resolve();
    transaction.onabort = transaction.onerror = () =>
      reject(new Error("图片移除失败，请重试。"));
  });
}
