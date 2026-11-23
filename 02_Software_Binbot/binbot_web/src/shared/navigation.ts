export function navigate(path: string, replace = false) {
  window.history[replace ? "replaceState" : "pushState"]({}, "", path);
  window.dispatchEvent(new PopStateEvent("popstate"));
}

export function currentAddress() {
  return `${window.location.pathname}${window.location.search || ""}${window.location.hash || ""}`;
}

/** Return only to console pages on this origin, including a device's fragment. */
export function safeReturnPath(value: string | null | undefined): string {
  if (!value || value.length > 1024 || /[\\\u0000-\u0020]/.test(value))
    return "/devices";
  if (/^\/services\/?(?:[?#].*)?$/.test(value)) return value;
  const match = value.match(
    /^\/devices(?:\/([A-Za-z0-9._:%-]+)(?:\/[a-z]+)?)?\/?(?:[?#].*)?$/,
  );
  if (match) {
    try {
      const id = decodeURIComponent(match[1] || "");
      if (
        !match[1] ||
        (/^[A-Za-z0-9._:-]{1,64}$/.test(id) && ![".", ".."].includes(id))
      )
        return value;
    } catch {
      // Malformed identifiers cannot be used as a return address.
    }
  }
  return "/devices";
}

export function loginPath(returnTo = currentAddress()) {
  return `/login?returnTo=${encodeURIComponent(safeReturnPath(returnTo))}`;
}

export const devicePath = (id: string, page = "today") =>
  `/devices/${encodeURIComponent(id)}/${page}`;

export function consoleRoute(path: string) {
  if (path === "/login" || path === "/login/") return "login";
  if (path === "/" || path === "/devices" || path === "/devices/")
    return "devices";
  if (path === "/services" || path === "/services/") return "services";
  return "workspace";
}
