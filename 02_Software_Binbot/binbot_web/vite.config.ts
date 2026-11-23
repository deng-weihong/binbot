import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, ".", "VITE_BINBOT_");
  const target = env.VITE_BINBOT_API_TARGET || "http://127.0.0.1:8765";
  const proxy = {
    "/api": target,
    "/v1": { target, ws: true },
    "/healthz": target,
    "/readyz": target,
  };
  return {
    plugins: [react()],
    server: { proxy },
    preview: { proxy },
    build: {
      rollupOptions: {
        output: {
          manualChunks: (id) => {
            if (id.endsWith("/catalogs/cet4.json")) return "cet4";
            if (id.includes("/three/")) return "three";
          },
        },
      },
    },
  };
});
