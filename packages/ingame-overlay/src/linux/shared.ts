/**
 * Host side of the shared memory channel with libtosu-overlay.so.
 * Layout must match native/linux/src/shared.h.
 *
 * Kept free of Electron imports so it can be exercised from plain Node.
 */
import {
    closeSync,
    ftruncateSync,
    openSync,
    readSync,
    unlinkSync,
    writeSync
} from 'node:fs';

export const MAGIC = 0x4f534f54;
export const VERSION = 1;

export const SLOTS = 3;
export const MAX_DIM = 4096;
export const NO_SLOT = 0xffffffff;
export const MAX_KEYBIND = 8;

export const HEADER_BYTES = 4096;
export const EVENT_COUNT = 256;
export const EVENT_BYTES = 64;
export const EVENTS_OFFSET = HEADER_BYTES;
export const SLOTS_OFFSET = EVENTS_OFFSET + EVENT_COUNT * EVENT_BYTES;
export const SLOT_BYTES = MAX_DIM * MAX_DIM * 4;
export const TOTAL_BYTES = SLOTS_OFFSET + SLOTS * SLOT_BYTES;

export const H = {
    MAGIC: 0,
    VERSION: 4,
    WRITER_PID: 8,
    SEQ: 16,
    ACTIVE_SLOT: 20,
    SLOT_WIDTH: 24,
    SLOT_HEIGHT: 36,
    VISIBLE: 48,
    KEYBIND_COUNT: 52,
    KEYBIND: 56,
    EVENT_READ: 192,

    READER_PID: 128,
    READING_SLOT: 132,
    GAME_WIDTH: 136,
    GAME_HEIGHT: 140,
    CONFIG_MODE: 144,
    EVENT_WRITE: 148,
    HEARTBEAT: 152
} as const;

export const EV = {
    KEY: 1,
    MOTION: 2,
    BUTTON: 3,
    WHEEL: 4,
    TEXT: 5,
    CONFIG: 6
} as const;

export interface OverlayInputEvent {
    type: number;
    code: number;
    down: boolean;
    mods: number;
    x: number;
    y: number;
    dx: number;
    dy: number;
    text: string;
}

export interface GameState {
    connected: boolean;
    width: number;
    height: number;
    configMode: boolean;
}

export function sharedPathFor(pid: number) {
    return `/dev/shm/tosu-overlay-${pid}`;
}

export class SharedOverlay {
    readonly path: string;
    private fd: number;
    private seq = 0;
    private active = NO_SLOT;
    private readonly u32 = Buffer.alloc(4);
    private readonly header = Buffer.alloc(256);
    private readonly events = Buffer.alloc(EVENT_COUNT * EVENT_BYTES);
    private closed = false;

    constructor(path: string) {
        this.path = path;
        // the reader validates MAGIC, so it is written last
        this.fd = openSync(path, 'w+', 0o600);
        ftruncateSync(this.fd, TOTAL_BYTES);
        this.writeU32(H.WRITER_PID, process.pid);
        this.writeU32(H.ACTIVE_SLOT, NO_SLOT);
        this.writeU32(H.READING_SLOT, NO_SLOT);
        this.writeU32(H.VISIBLE, 1);
        this.writeU32(H.VERSION, VERSION);
        this.writeU32(H.MAGIC, MAGIC);
    }

    private writeU32(offset: number, value: number) {
        this.u32.writeUInt32LE(value >>> 0, 0);
        writeSync(this.fd, this.u32, 0, 4, offset);
    }

    private readHeader() {
        readSync(this.fd, this.header, 0, this.header.length, 0);
        return this.header;
    }

    /**
     * Publish a BGRA, premultiplied, top-down, tightly packed frame.
     */
    writeFrame(bitmap: Buffer, width: number, height: number) {
        if (this.closed) return;

        width = Math.min(width, MAX_DIM);
        height = Math.min(height, MAX_DIM);
        const bytes = width * height * 4;
        if (width <= 0 || height <= 0 || bitmap.length < bytes) return;

        const reading = this.readHeader().readUInt32LE(H.READING_SLOT);
        let slot = 0;
        while (slot === this.active || slot === reading) slot++;

        writeSync(this.fd, bitmap, 0, bytes, SLOTS_OFFSET + slot * SLOT_BYTES);
        this.writeU32(H.SLOT_WIDTH + slot * 4, width);
        this.writeU32(H.SLOT_HEIGHT + slot * 4, height);
        this.writeU32(H.ACTIVE_SLOT, slot);
        this.seq = (this.seq + 1) >>> 0;
        this.writeU32(H.SEQ, this.seq);
        this.active = slot;
    }

    setVisible(visible: boolean) {
        if (!this.closed) this.writeU32(H.VISIBLE, visible ? 1 : 0);
    }

    /**
     * @param groups each entry lists SDL scancodes that satisfy one key (e.g. left/right ctrl)
     */
    setKeybind(groups: number[][]) {
        if (this.closed) return;
        const count = Math.min(groups.length, MAX_KEYBIND);
        // disable while updating so a half-written keybind never matches
        this.writeU32(H.KEYBIND_COUNT, 0);
        for (let i = 0; i < count; i++) {
            this.writeU32(H.KEYBIND + i * 8, groups[i][0] ?? 0);
            this.writeU32(H.KEYBIND + i * 8 + 4, groups[i][1] ?? 0);
        }
        this.writeU32(H.KEYBIND_COUNT, count);
    }

    gameState(): GameState {
        const header = this.readHeader();
        return {
            connected: header.readUInt32LE(H.READER_PID) !== 0,
            width: header.readUInt32LE(H.GAME_WIDTH),
            height: header.readUInt32LE(H.GAME_HEIGHT),
            configMode: header.readUInt32LE(H.CONFIG_MODE) !== 0
        };
    }

    /** Drain input events queued by the game. */
    readEvents(): OverlayInputEvent[] {
        if (this.closed) return [];

        const header = this.readHeader();
        const write = header.readUInt32LE(H.EVENT_WRITE);
        let read = header.readUInt32LE(H.EVENT_READ);
        if (write === read) return [];

        // the game drops events when the ring is full, so never more than EVENT_COUNT pending
        if ((write - read) >>> 0 > EVENT_COUNT)
            read = (write - EVENT_COUNT) >>> 0;

        readSync(this.fd, this.events, 0, this.events.length, EVENTS_OFFSET);

        const out: OverlayInputEvent[] = [];
        while (read !== write) {
            const base = (read % EVENT_COUNT) * EVENT_BYTES;
            const textBytes = this.events.subarray(base + 32, base + 64);
            const nul = textBytes.indexOf(0);
            out.push({
                type: this.events.readUInt32LE(base),
                code: this.events.readUInt32LE(base + 4),
                down: this.events.readUInt32LE(base + 8) !== 0,
                mods: this.events.readUInt32LE(base + 12),
                x: this.events.readFloatLE(base + 16),
                y: this.events.readFloatLE(base + 20),
                dx: this.events.readFloatLE(base + 24),
                dy: this.events.readFloatLE(base + 28),
                text: textBytes
                    .subarray(0, nul === -1 ? textBytes.length : nul)
                    .toString('utf8')
            });
            read = (read + 1) >>> 0;
        }

        this.writeU32(H.EVENT_READ, read);
        return out;
    }

    close() {
        if (this.closed) return;
        this.closed = true;
        try {
            unlinkSync(this.path);
        } catch {
            // already removed
        }
        closeSync(this.fd);
    }
}
