import {
    BrowserWindow,
    type MouseInputEvent,
    type NativeImage,
    type Rectangle
} from 'electron';
import EventEmitter from 'node:events';

import type { IngameOverlay, OverlayEventEmitter } from '../overlay';
import { preloadPath } from '../page';
import {
    type ElectronModifier,
    keybindToScancodes,
    scancodeToElectronKey,
    sdlModsToElectron
} from './keys';
import {
    EV,
    type OverlayInputEvent,
    SharedOverlay,
    sharedPathFor
} from './shared';

const POLL_INTERVAL_MS = 8;
const ALIVE_CHECK_MS = 1000;
const DEFAULT_SIZE = { width: 1280, height: 720 };

const MOUSE_BUTTONS: Record<number, MouseInputEvent['button']> = {
    1: 'left',
    2: 'middle',
    3: 'right'
};

/**
 * Linux in-game overlay.
 *
 * The game runs with libtosu-overlay.so (LD_PRELOAD + Vulkan layer, see
 * native/linux). This side renders the overlay page offscreen and publishes
 * every painted frame through /dev/shm; the library composites it in the game
 * and sends input back while configuration mode is active.
 */
export class LinuxOverlayProcess implements IngameOverlay {
    readonly event: OverlayEventEmitter = new EventEmitter();

    private readonly pollTimer: NodeJS.Timeout;
    private readonly aliveTimer: NodeJS.Timeout;
    private width = 0;
    private height = 0;
    private configMode = false;
    private connected = false;
    private modifiers: ElectronModifier[] = [];
    private lastMouse = { x: 0, y: 0 };
    private pressedButtons = new Set<number>();
    private destroyed = false;

    private constructor(
        private readonly pid: number,
        private readonly shared: SharedOverlay,
        readonly window: BrowserWindow
    ) {
        window.webContents.on('paint', (_event, _dirty: Rectangle, image) =>
            this.onPaint(image)
        );

        this.pollTimer = setInterval(() => this.poll(), POLL_INTERVAL_MS);
        this.aliveTimer = setInterval(() => this.checkAlive(), ALIVE_CHECK_MS);
    }

    static async initialize(pid: number): Promise<LinuxOverlayProcess> {
        const shared = new SharedOverlay(sharedPathFor(pid));

        const window = new BrowserWindow({
            webPreferences: {
                offscreen: true,
                transparent: true,
                backgroundThrottling: false,
                preload: preloadPath
            },
            transparent: true,
            frame: false,
            show: false
        });
        window.setSize(DEFAULT_SIZE.width, DEFAULT_SIZE.height, false);

        console.log(
            `warn: Waiting for osu! (pid ${pid}) to load the overlay library. ` +
                'Start osu! through tosu-overlay-run if it is not showing up.'
        );

        return new LinuxOverlayProcess(pid, shared, window);
    }

    setKeybind(keys: string[]) {
        const groups = keybindToScancodes(keys);
        if (!groups) {
            console.error(
                `Unsupported in-game overlay keybind: ${keys.join(' + ')}`
            );
            return;
        }
        this.shared.setKeybind(groups);
    }

    private onPaint(image: NativeImage) {
        if (this.destroyed || image.isEmpty()) return;

        const { width, height } = image.getSize();
        // chromium bitmaps are BGRA premultiplied, exactly what the game side expects
        this.shared.writeFrame(image.toBitmap(), width, height);
    }

    private poll() {
        if (this.destroyed) return;

        const state = this.shared.gameState();
        if (state.connected && !this.connected) {
            this.connected = true;
            console.log(`warn: In-game overlay attached to pid ${this.pid}`);
        }

        if (
            state.width > 0 &&
            state.height > 0 &&
            (state.width !== this.width || state.height !== this.height)
        ) {
            this.width = state.width;
            this.height = state.height;
            console.debug(
                `game framebuffer resized: ${state.width}x${state.height}`
            );
            this.window.setSize(state.width, state.height, false);
            this.window.webContents.invalidate();
        }

        for (const event of this.shared.readEvents()) {
            this.handleInput(event);
        }

        // the game may also leave configuration mode on its own (focus loss)
        if (state.configMode !== this.configMode) {
            this.setConfigMode(state.configMode);
        }
    }

    private handleInput(event: OverlayInputEvent) {
        const contents = this.window.webContents;

        switch (event.type) {
            case EV.CONFIG:
                this.setConfigMode(event.down);
                return;

            case EV.MOTION:
                this.lastMouse = { x: event.x, y: event.y };
                contents.sendInputEvent({
                    type: 'mouseMove',
                    x: Math.round(event.x),
                    y: Math.round(event.y),
                    modifiers: this.modifiers
                });
                return;

            case EV.BUTTON: {
                const button = MOUSE_BUTTONS[event.code];
                if (!button) return;
                this.lastMouse = { x: event.x, y: event.y };
                if (event.down) this.pressedButtons.add(event.code);
                else this.pressedButtons.delete(event.code);

                contents.sendInputEvent({
                    type: event.down ? 'mouseDown' : 'mouseUp',
                    button,
                    clickCount: 1,
                    x: Math.round(event.x),
                    y: Math.round(event.y),
                    modifiers: this.modifiers
                });
                return;
            }

            case EV.WHEEL:
                contents.sendInputEvent({
                    type: 'mouseWheel',
                    // SDL reports notches (positive = up/right), chromium uses 120 per notch
                    deltaX: -event.dx * 120,
                    deltaY: event.dy * 120,
                    canScroll: true,
                    x: Math.round(event.x || this.lastMouse.x),
                    y: Math.round(event.y || this.lastMouse.y),
                    modifiers: this.modifiers
                });
                return;

            case EV.KEY: {
                this.modifiers = sdlModsToElectron(event.mods);
                const keyCode = scancodeToElectronKey(event.code);
                if (!keyCode) return;
                contents.sendInputEvent({
                    type: event.down ? 'keyDown' : 'keyUp',
                    keyCode,
                    modifiers: this.modifiers
                });
                return;
            }

            case EV.TEXT:
                for (const ch of event.text) {
                    contents.sendInputEvent({
                        type: 'char',
                        keyCode: ch,
                        modifiers: this.modifiers
                    });
                }
        }
    }

    private setConfigMode(enabled: boolean) {
        if (enabled === this.configMode) return;
        this.configMode = enabled;

        if (enabled) {
            this.window.webContents.send('inputCaptureStart');
            this.window.focusOnWebView();
            return;
        }

        // release anything still held so the page does not keep dragging
        for (const code of this.pressedButtons) {
            const button = MOUSE_BUTTONS[code];
            if (!button) continue;
            this.window.webContents.sendInputEvent({
                type: 'mouseUp',
                button,
                clickCount: 1,
                x: Math.round(this.lastMouse.x),
                y: Math.round(this.lastMouse.y)
            });
        }
        this.pressedButtons.clear();
        this.modifiers = [];

        this.window.webContents.send('inputCaptureEnd');
        this.window.blurWebView();
    }

    private checkAlive() {
        try {
            process.kill(this.pid, 0);
        } catch (exc) {
            if ((exc as NodeJS.ErrnoException).code === 'ESRCH') {
                console.log(`osu! process ${this.pid} exited`);
                this.destroy();
            }
        }
    }

    destroy() {
        if (this.destroyed) return;
        this.destroyed = true;

        clearInterval(this.pollTimer);
        clearInterval(this.aliveTimer);
        this.shared.close();
        if (!this.window.isDestroyed()) this.window.destroy();
        this.event.emit('destroyed');
    }
}
