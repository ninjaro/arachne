export const EVOLUTION_HOVER_OPEN_DELAY_MS = 90;
export const EVOLUTION_HOVER_CLOSE_DELAY_MS = 120;

export interface DelayedPreviewController<T> {
  pointerEnter(target: T): void;
  pointerLeave(target: T): void;
  openNow(target: T): void;
  keepOpen(): void;
  closeNow(): void;
  dispose(): void;
}

/**
 * Small presentation-only hover state machine. Pointer entry is delayed so a
 * cursor crossing a dense map does not flash tooltips; keyboard focus can use
 * `openNow` and remains immediate.
 */
export function createDelayedPreviewController<T>({
  onOpen,
  onClose,
  openDelay = EVOLUTION_HOVER_OPEN_DELAY_MS,
  closeDelay = EVOLUTION_HOVER_CLOSE_DELAY_MS,
  isSameTarget = Object.is,
}: {
  onOpen: (target: T) => void;
  onClose: (target: T | null) => void;
  openDelay?: number;
  closeDelay?: number;
  isSameTarget?: (left: T, right: T) => boolean;
}): DelayedPreviewController<T> {
  let openingTimer: ReturnType<typeof setTimeout> | null = null;
  let openingTarget: T | null = null;
  let closingTimer: ReturnType<typeof setTimeout> | null = null;
  let closingTarget: T | null = null;
  let activeTarget: T | null = null;

  const cancelOpening = () => {
    if (openingTimer !== null) clearTimeout(openingTimer);
    openingTimer = null;
    openingTarget = null;
  };
  const cancelClosing = () => {
    if (closingTimer !== null) clearTimeout(closingTimer);
    closingTimer = null;
    closingTarget = null;
  };

  return {
    pointerEnter(target) {
      cancelOpening();
      if (closingTarget !== null && isSameTarget(closingTarget, target)) {
        cancelClosing();
      }
      if (activeTarget !== null && isSameTarget(activeTarget, target)) return;
      openingTarget = target;
      openingTimer = setTimeout(() => {
        openingTimer = null;
        openingTarget = null;
        cancelClosing();
        activeTarget = target;
        onOpen(target);
      }, Math.max(0, openDelay));
    },
    pointerLeave(target) {
      if (openingTarget !== null && isSameTarget(openingTarget, target)) {
        cancelOpening();
      }
      if (closingTarget !== null && !isSameTarget(closingTarget, target)) {
        return;
      }
      cancelClosing();
      closingTarget = target;
      closingTimer = setTimeout(() => {
        closingTimer = null;
        closingTarget = null;
        if (activeTarget !== null && isSameTarget(activeTarget, target)) {
          activeTarget = null;
        }
        onClose(target);
      }, Math.max(0, closeDelay));
    },
    openNow(target) {
      cancelOpening();
      cancelClosing();
      activeTarget = target;
      onOpen(target);
    },
    keepOpen() {
      cancelClosing();
    },
    closeNow() {
      cancelOpening();
      cancelClosing();
      activeTarget = null;
      onClose(null);
    },
    dispose() {
      cancelOpening();
      cancelClosing();
      activeTarget = null;
    },
  };
}
