/**
 * Stand-in for the Electron overlay host, for testing libtosu-overlay.so.
 *
 *   node tests/fake_host.ts <game pid> [seconds]
 *
 * Publishes an animated test pattern sized to the game's framebuffer,
 * sets the Control + Shift + Space keybind and logs game state / input events.
 */
import { keybindToScancodes } from '../../../src/linux/keys.ts';
import { EV, SharedOverlay, sharedPathFor } from '../../../src/linux/shared.ts';

const pid = Number(process.argv[2]);
const seconds = Number(process.argv[3] ?? 10);
if (!pid) {
    console.error('usage: node fake_host.ts <pid> [seconds]');
    process.exit(1);
}

const shared = new SharedOverlay(sharedPathFor(pid));
shared.setKeybind(keybindToScancodes(['Control', 'Shift', 'Space'])!);

function pattern(width: number, height: number, t: number) {
    const buf = Buffer.alloc(width * height * 4); // transparent
    const put = (
        x: number,
        y: number,
        b: number,
        g: number,
        r: number,
        a: number
    ) => {
        const i = (y * width + x) * 4;
        // premultiplied BGRA
        buf[i] = (b * a) / 255;
        buf[i + 1] = (g * a) / 255;
        buf[i + 2] = (r * a) / 255;
        buf[i + 3] = a;
    };
    const rect = (
        x0: number,
        y0: number,
        w: number,
        h: number,
        b: number,
        g: number,
        r: number,
        a: number
    ) => {
        for (let y = Math.max(0, y0); y < Math.min(height, y0 + h); y++)
            for (let x = Math.max(0, x0); x < Math.min(width, x0 + w); x++)
                put(x, y, b, g, r, a);
    };

    rect(20, 20, 200, 100, 0, 0, 255, 255); // opaque red, top-left (orientation + channel order)
    rect(width - 220, height - 120, 200, 100, 0, 255, 0, 128); // half transparent green, bottom-right
    const x = Math.floor(((t * 200) % (width - 60)) + 30); // moving white bar, proves updates
    rect(x, Math.floor(height / 2) - 10, 30, 20, 255, 255, 255, 255);
    return buf;
}

let lastSize = '';
let frames = 0;
const start = Date.now();

const timer = setInterval(() => {
    const state = shared.gameState();
    const size = `${state.width}x${state.height}`;
    if (size !== lastSize) {
        console.log(`host: game connected=${state.connected} size=${size}`);
        lastSize = size;
    }

    for (const ev of shared.readEvents()) {
        const name = Object.entries(EV).find(([, v]) => v === ev.type)?.[0];
        console.log(
            `host: event ${name} code=${ev.code} down=${ev.down} x=${ev.x.toFixed(0)} y=${ev.y.toFixed(0)} text=${JSON.stringify(ev.text)}`
        );
    }

    if (state.width > 0 && state.height > 0) {
        shared.writeFrame(
            pattern(state.width, state.height, (Date.now() - start) / 1000),
            state.width,
            state.height
        );
        frames++;
    }

    if (Date.now() - start > seconds * 1000) {
        console.log(`host: published ${frames} frames, exiting`);
        clearInterval(timer);
        shared.close();
    }
}, 33);
