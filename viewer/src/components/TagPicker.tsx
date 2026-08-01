import {
  useDeferredValue,
  useEffect,
  useId,
  useMemo,
  useState,
} from "react";
import type { FocusEvent, KeyboardEvent } from "react";
import type { EntityId } from "../lib/types";
import type { EvolutionTag } from "../lib/evolution";
import { humanize } from "../lib/format";

function queryRank(label: string, query: string): number {
  const normalized = label.toLocaleLowerCase();
  if (!query) return 3;
  if (normalized === query) return 0;
  if (normalized.startsWith(query)) return 1;
  if (normalized.includes(query)) return 2;
  return Number.MAX_SAFE_INTEGER;
}

export function TagPicker({
  label,
  placeholder,
  mode,
  options,
  selectedIds,
  blockedIds,
  onAdd,
  onRemove,
}: {
  label: string;
  placeholder: string;
  mode: "include" | "exclude";
  options: readonly EvolutionTag[];
  selectedIds: readonly EntityId[];
  blockedIds: readonly EntityId[];
  onAdd: (id: EntityId) => void;
  onRemove: (id: EntityId) => void;
}) {
  const generatedId = useId();
  const inputId = `metro-tag-input-${generatedId}`;
  const listboxId = `metro-tag-list-${generatedId}`;
  const [query, setQuery] = useState("");
  const [open, setOpen] = useState(false);
  const [activeIndex, setActiveIndex] = useState(0);
  const deferredQuery = useDeferredValue(query.trim().toLocaleLowerCase());
  const selected = useMemo(() => new Set(selectedIds), [selectedIds]);
  const blocked = useMemo(() => new Set(blockedIds), [blockedIds]);
  const optionById = useMemo(
    () => new Map(options.map((option) => [option.id, option])),
    [options],
  );
  const results = useMemo(
    () =>
      options
        .map((option) => ({ option, rank: queryRank(option.label, deferredQuery) }))
        .filter(
          ({ option, rank }) =>
            rank !== Number.MAX_SAFE_INTEGER &&
            !selected.has(option.id) &&
            !blocked.has(option.id),
        )
        .sort(
          (left, right) =>
            left.rank - right.rank ||
            right.option.datedWorkCount - left.option.datedWorkCount ||
            left.option.label.localeCompare(right.option.label) ||
            left.option.id.localeCompare(right.option.id),
        )
        .slice(0, 40)
        .map(({ option }) => option),
    [blocked, deferredQuery, options, selected],
  );

  useEffect(() => setActiveIndex(0), [deferredQuery, results.length]);

  function choose(option: EvolutionTag) {
    onAdd(option.id);
    setQuery("");
    setOpen(false);
  }

  function onKeyDown(event: KeyboardEvent<HTMLInputElement>) {
    if (event.key === "ArrowDown") {
      event.preventDefault();
      setOpen(true);
      if (results.length) {
        setActiveIndex((current) => Math.min(results.length - 1, current + 1));
      }
    } else if (event.key === "ArrowUp") {
      event.preventDefault();
      setOpen(true);
      setActiveIndex((current) => Math.max(0, current - 1));
    } else if (event.key === "Enter" && open && results[activeIndex]) {
      event.preventDefault();
      choose(results[activeIndex]!);
    } else if (event.key === "Escape") {
      event.stopPropagation();
      setOpen(false);
    }
  }

  function onBlur(event: FocusEvent<HTMLDivElement>) {
    if (!event.currentTarget.contains(event.relatedTarget as Node | null)) {
      setOpen(false);
    }
  }

  return (
    <div className={`metro-tag-picker ${mode}`} onBlur={onBlur}>
      <label htmlFor={inputId}>{label}</label>
      <div className="metro-tag-picker-entry">
        <div className="metro-tag-chips">
          {selectedIds.map((id) => {
            const option = optionById.get(id);
            return option ? (
              <span className="metro-tag-chip" key={id}>
                <span>{option.label}</span>
                <button
                  type="button"
                  onClick={() => onRemove(id)}
                  aria-label={`Remove ${option.label} from ${label.toLocaleLowerCase()}`}
                >
                  ×
                </button>
              </span>
            ) : null;
          })}
          <input
            id={inputId}
            type="search"
            value={query}
            placeholder={selectedIds.length ? "Add another tag" : placeholder}
            role="combobox"
            aria-autocomplete="list"
            aria-haspopup="listbox"
            aria-expanded={open}
            aria-controls={listboxId}
            aria-activedescendant={
              open && results[activeIndex]
                ? `${listboxId}-option-${activeIndex}`
                : undefined
            }
            onFocus={() => setOpen(true)}
            onChange={(event) => {
              setQuery(event.target.value);
              setOpen(true);
            }}
            onKeyDown={onKeyDown}
          />
        </div>
      </div>
      {open ? (
        <div id={listboxId} className="metro-tag-results" role="listbox">
          {results.length ? (
            results.map((option, index) => (
              <button
                id={`${listboxId}-option-${index}`}
                type="button"
                role="option"
                aria-selected={index === activeIndex}
                className={index === activeIndex ? "active" : ""}
                key={option.id}
                onMouseDown={(event) => event.preventDefault()}
                onMouseEnter={() => setActiveIndex(index)}
                onClick={() => choose(option)}
              >
                <span>{option.label}</span>
                <small>
                  {humanize(option.conceptType)} · {option.datedWorkCount.toLocaleString()}
                </small>
              </button>
            ))
          ) : (
            <span className="metro-tag-results-empty">No available tags match.</span>
          )}
        </div>
      ) : null}
    </div>
  );
}
