// PakonX — vanilla DOM factories on window.PakonX. No framework required.

export interface WindowProps {
  /** Title bar text, e.g. "Pakon F-135+ — Scan". */
  title: string;
  /** Renders the unfocused title bar. */
  inactive?: boolean;
  /** Caption buttons (minimize, maximize, close). Pass false for panels docked inside the app window. */
  controls?: boolean;
  /** Status-bar segments, left to right. Omit for no status bar. */
  status?: string[];
  /** Element to append the window to. */
  mount?: HTMLElement;
}
export declare function Window(props: WindowProps): { root: HTMLElement; body: HTMLElement };

export interface ButtonProps {
  label: string;
  /** "go" for the single primary action of a window; "danger" for destructive ones. */
  variant?: "default" | "go" | "danger";
  disabled?: boolean;
  onClick?: (e: MouseEvent) => void;
}
export declare function Button(props: ButtonProps): HTMLButtonElement;

export interface StatusChipProps {
  /** The word that carries the meaning — colour never does alone. */
  label: string;
  tone?: "neutral" | "ok" | "warn" | "danger" | "film";
}
export declare function StatusChip(props: StatusChipProps): HTMLSpanElement;

export interface ScanProgressProps {
  /** 0–100. */
  value?: number;
  /** "film" while the lamp sweeps; default green for transfers/saves. */
  tone?: "go" | "film";
  /** Mono caption under the bar, left side, e.g. "Frame 14 / 24". */
  left?: string;
  /** Right caption; defaults to the percentage. */
  right?: string;
  label?: string;
}
export declare function ScanProgress(props: ScanProgressProps): {
  root: HTMLElement;
  set(value: number, left?: string, right?: string): void;
};

export interface FilmFrame {
  /** Frame number as printed on the film edge, e.g. "14" or "14A". */
  no: string;
  /** Thumbnail URL. Without it the frame shows the neutral loupe grey. */
  src?: string;
  /** Dust/exposure warning badge. */
  flagged?: boolean;
}
export interface FilmStripProps {
  frames: FilmFrame[];
  selected?: number;
  onSelect?: (index: number) => void;
  label?: string;
  /** Lets the user mark several frames: Ctrl/Cmd-click or Space toggles, Shift-click marks a run, a plain click clears. */
  multiple?: boolean;
  /** The marked frames, ascending, after every change. */
  onMark?: (indices: number[]) => void;
  /** Contact sheet: strips of this many frames, one under another (Up/Down move a strip). */
  perRow?: number;
  /** Double-click or Enter on a frame. */
  onOpen?: (index: number) => void;
}
export declare function FilmStrip(props: FilmStripProps): {
  root: HTMLElement;
  select(index: number): void;
  /** Replace the marked set (also calls onMark). */
  mark(indices: number[]): void;
};

export interface Tab {
  id: string;
  /** Sentence case, a noun: "Scan", "Roll", "Light table". */
  label: string;
}
export interface TabsProps {
  tabs: Tab[];
  selected?: string;
  /** Called when the user picks a tab (click or arrow keys), not on select(). */
  onSelect?: (id: string) => void;
  label?: string;
}
export declare function Tabs(props: TabsProps): { root: HTMLElement; select(id: string): void };
