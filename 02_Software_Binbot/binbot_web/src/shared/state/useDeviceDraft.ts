import { useEffect, useRef, useState } from "react";
import { equal } from "../model/workspaceModel";

/** Follow external device changes while preserving an edit already in progress. */
export function useDeviceDraft<T>(saved: T) {
  const [draft, setDraft] = useState(saved);
  const previous = useRef(saved);
  useEffect(() => {
    const before = previous.current;
    previous.current = saved;
    setDraft((value) =>
      equal(value, before) && !equal(value, saved) ? saved : value,
    );
  }, [saved]);
  return [draft, setDraft] as const;
}
