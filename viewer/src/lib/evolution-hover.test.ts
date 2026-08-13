import { afterEach, describe, expect, it, vi } from "vitest";

import {
  createDelayedPreviewController,
  EVOLUTION_HOVER_OPEN_DELAY_MS,
} from "./evolution-hover";

afterEach(() => {
  vi.useRealTimers();
});

describe("Evolution delayed hover preview", () => {
  it("opens only after the pointer delay", () => {
    vi.useFakeTimers();
    const opened: string[] = [];
    const controller = createDelayedPreviewController<string>({
      onOpen: (target) => opened.push(target),
      onClose: () => undefined,
    });

    controller.pointerEnter("station-a");
    vi.advanceTimersByTime(EVOLUTION_HOVER_OPEN_DELAY_MS - 1);
    expect(opened).toEqual([]);
    vi.advanceTimersByTime(1);
    expect(opened).toEqual(["station-a"]);
  });

  it("cancels a pending preview when the pointer leaves early", () => {
    vi.useFakeTimers();
    const opened: string[] = [];
    const closed: Array<string | null> = [];
    const controller = createDelayedPreviewController<string>({
      onOpen: (target) => opened.push(target),
      onClose: (target) => closed.push(target),
    });

    controller.pointerEnter("trajectory-a");
    vi.advanceTimersByTime(EVOLUTION_HOVER_OPEN_DELAY_MS / 2);
    controller.pointerLeave("trajectory-a");
    vi.runAllTimers();

    expect(opened).toEqual([]);
    expect(closed).toEqual(["trajectory-a"]);
  });

  it("opens keyboard focus immediately and disposes pending work", () => {
    vi.useFakeTimers();
    const opened: string[] = [];
    const controller = createDelayedPreviewController<string>({
      onOpen: (target) => opened.push(target),
      onClose: () => undefined,
    });

    controller.pointerEnter("pointer-target");
    controller.openNow("keyboard-target");
    controller.dispose();
    vi.runAllTimers();

    expect(opened).toEqual(["keyboard-target"]);
  });

  it("closes the active preview when a replacement hover is aborted", () => {
    vi.useFakeTimers();
    let active: string | null = null;
    const controller = createDelayedPreviewController<string>({
      onOpen: (target) => { active = target; },
      onClose: (target) => {
        if (target === null || active === target) active = null;
      },
    });

    controller.pointerEnter("station-a");
    vi.advanceTimersByTime(EVOLUTION_HOVER_OPEN_DELAY_MS);
    expect(active).toBe("station-a");
    controller.pointerLeave("station-a");
    controller.pointerEnter("trajectory-b");
    vi.advanceTimersByTime(EVOLUTION_HOVER_OPEN_DELAY_MS / 2);
    controller.pointerLeave("trajectory-b");
    vi.runAllTimers();

    expect(active).toBeNull();
  });
});
