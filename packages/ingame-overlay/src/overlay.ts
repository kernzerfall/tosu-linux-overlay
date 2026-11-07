import type { BrowserWindow } from 'electron';
import type EventEmitter from 'node:events';

export type OverlayEventEmitter = EventEmitter<{
    destroyed: [];
}>;

/**
 * An overlay attached to one game process.
 * Implemented by `OverlayProcess` (Windows, asdf-overlay) and
 * `LinuxOverlayProcess` (Linux, libtosu-overlay.so).
 */
export interface IngameOverlay {
    readonly window: BrowserWindow;
    readonly event: OverlayEventEmitter;

    /** @param keys DOM key names, e.g. ['Control', 'Shift', 'Space'] */
    setKeybind(keys: string[]): void;
    destroy(): void;
}

/**
 * Attach to a game process using the implementation for the current platform.
 * Loaded lazily so the Windows-only asdf-overlay addon is never required on Linux.
 */
export async function attachOverlay(pid: number): Promise<IngameOverlay> {
    if (process.platform === 'linux') {
        const { LinuxOverlayProcess } = await import('./linux/process');
        return LinuxOverlayProcess.initialize(pid);
    }

    const { OverlayProcess } = await import('./process');
    return OverlayProcess.initialize(pid);
}
