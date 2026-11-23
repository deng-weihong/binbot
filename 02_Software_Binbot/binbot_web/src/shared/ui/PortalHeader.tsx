import { useState } from "react";
import { LogOut } from "lucide-react";
import { useConsole } from "../state/consoleContext";
import { navigate } from "../navigation";

export function PortalHeader({ label }: { label: string }) {
  const { mode, logout } = useConsole();
  const [busy, setBusy] = useState(false),
    [error, setError] = useState("");
  return (
    <>
      <header className="portal-header">
        <a
          className="wordmark"
          href="/devices"
          onClick={(event) => {
            event.preventDefault();
            navigate("/devices");
          }}
        >
          binbot<span>®</span>
        </a>
        <span className="portal-header-divider" />
        <span className="portal-header-label">{label}</span>
        <div className="portal-header-actions">
          <span className={`portal-mode ${mode === "demo" ? "is-demo" : ""}`}>
            <i />
            {mode === "demo" ? "界面预览" : "管理控制台"}
          </span>
          <button
            className="portal-logout"
            disabled={busy}
            onClick={async () => {
              setBusy(true);
              setError("");
              try {
                await logout();
              } catch (reason) {
                setError(
                  reason instanceof Error
                    ? reason.message
                    : "退出失败，请重试。",
                );
              } finally {
                setBusy(false);
              }
            }}
          >
            <LogOut size={16} />
            <span>{mode === "demo" ? "退出预览" : "退出登录"}</span>
          </button>
        </div>
      </header>
      {error && (
        <p className="portal-alert" role="alert">
          {error}
        </p>
      )}
    </>
  );
}
