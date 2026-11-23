import React from "react";
import ReactDOM from "react-dom/client";
import App from "./app/App";
import "./styles/styles.css";
import "./styles/shell.css";
import "./styles/features.css";
import "./styles/workspace-polish.css";
import "./styles/white-noise.css";
import "./styles/device-workspace.css";
import "./styles/assistant.css";
import "./styles/service-settings.css";
import "./styles/home-todos.css";
import "./styles/wordbooks.css";
import "./styles/memory-review.css";
import "./styles/portal.css";
import "./styles/device-management.css";

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
