/**
 * SDL3 scancode helpers for the Linux overlay.
 *
 * - DOM key names (as used by INGAME_OVERLAY_KEYBIND, e.g. "Control + Shift + Space")
 *   -> SDL scancode alternatives, for the game-side keybind check.
 * - SDL scancodes -> Electron `sendInputEvent` key codes.
 */

const LETTERS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ';

// SDL_Scancode values (SDL3 / USB HID usage ids)
const SC = {
    A: 4,
    N1: 30,
    N0: 39,
    RETURN: 40,
    ESCAPE: 41,
    BACKSPACE: 42,
    TAB: 43,
    SPACE: 44,
    MINUS: 45,
    EQUALS: 46,
    LEFTBRACKET: 47,
    RIGHTBRACKET: 48,
    BACKSLASH: 49,
    SEMICOLON: 51,
    APOSTROPHE: 52,
    GRAVE: 53,
    COMMA: 54,
    PERIOD: 55,
    SLASH: 56,
    F1: 58,
    INSERT: 73,
    HOME: 74,
    PAGEUP: 75,
    DELETE: 76,
    END: 77,
    PAGEDOWN: 78,
    RIGHT: 79,
    LEFT: 80,
    DOWN: 81,
    UP: 82,
    LCTRL: 224,
    LSHIFT: 225,
    LALT: 226,
    LGUI: 227,
    RCTRL: 228,
    RSHIFT: 229,
    RALT: 230,
    RGUI: 231
} as const;

const DOM_TO_SCANCODES: Record<string, number[]> = {
    Control: [SC.LCTRL, SC.RCTRL],
    Ctrl: [SC.LCTRL, SC.RCTRL],
    Shift: [SC.LSHIFT, SC.RSHIFT],
    Alt: [SC.LALT, SC.RALT],
    Meta: [SC.LGUI, SC.RGUI],
    Super: [SC.LGUI, SC.RGUI],
    Space: [SC.SPACE],
    ' ': [SC.SPACE],
    Enter: [SC.RETURN],
    Escape: [SC.ESCAPE],
    Backspace: [SC.BACKSPACE],
    Tab: [SC.TAB],
    Insert: [SC.INSERT],
    Delete: [SC.DELETE],
    Home: [SC.HOME],
    End: [SC.END],
    PageUp: [SC.PAGEUP],
    PageDown: [SC.PAGEDOWN],
    ArrowUp: [SC.UP],
    ArrowDown: [SC.DOWN],
    ArrowLeft: [SC.LEFT],
    ArrowRight: [SC.RIGHT],
    '`': [SC.GRAVE],
    Backquote: [SC.GRAVE],
    '-': [SC.MINUS],
    '=': [SC.EQUALS],
    '[': [SC.LEFTBRACKET],
    ']': [SC.RIGHTBRACKET],
    '\\': [SC.BACKSLASH],
    ';': [SC.SEMICOLON],
    "'": [SC.APOSTROPHE],
    ',': [SC.COMMA],
    '.': [SC.PERIOD],
    '/': [SC.SLASH]
};

/** Electron accelerator-style key codes, indexed by SDL scancode. */
const SCANCODE_TO_ELECTRON: Record<number, string> = {
    [SC.RETURN]: 'Enter',
    [SC.ESCAPE]: 'Escape',
    [SC.BACKSPACE]: 'Backspace',
    [SC.TAB]: 'Tab',
    [SC.SPACE]: 'Space',
    [SC.MINUS]: '-',
    [SC.EQUALS]: '=',
    [SC.LEFTBRACKET]: '[',
    [SC.RIGHTBRACKET]: ']',
    [SC.BACKSLASH]: '\\',
    [SC.SEMICOLON]: ';',
    [SC.APOSTROPHE]: "'",
    [SC.GRAVE]: '`',
    [SC.COMMA]: ',',
    [SC.PERIOD]: '.',
    [SC.SLASH]: '/',
    [SC.INSERT]: 'Insert',
    [SC.HOME]: 'Home',
    [SC.PAGEUP]: 'PageUp',
    [SC.DELETE]: 'Delete',
    [SC.END]: 'End',
    [SC.PAGEDOWN]: 'PageDown',
    [SC.RIGHT]: 'Right',
    [SC.LEFT]: 'Left',
    [SC.DOWN]: 'Down',
    [SC.UP]: 'Up',
    [SC.LCTRL]: 'Control',
    [SC.RCTRL]: 'Control',
    [SC.LSHIFT]: 'Shift',
    [SC.RSHIFT]: 'Shift',
    [SC.LALT]: 'Alt',
    [SC.RALT]: 'Alt',
    [SC.LGUI]: 'Meta',
    [SC.RGUI]: 'Meta'
};

for (let i = 0; i < LETTERS.length; i++) {
    DOM_TO_SCANCODES[LETTERS[i]] = [SC.A + i];
    DOM_TO_SCANCODES[LETTERS[i].toLowerCase()] = [SC.A + i];
    SCANCODE_TO_ELECTRON[SC.A + i] = LETTERS[i];
}
for (let i = 1; i <= 9; i++) {
    DOM_TO_SCANCODES[String(i)] = [SC.N1 + i - 1];
    SCANCODE_TO_ELECTRON[SC.N1 + i - 1] = String(i);
}
DOM_TO_SCANCODES['0'] = [SC.N0];
SCANCODE_TO_ELECTRON[SC.N0] = '0';
for (let i = 1; i <= 12; i++) {
    DOM_TO_SCANCODES[`F${i}`] = [SC.F1 + i - 1];
    SCANCODE_TO_ELECTRON[SC.F1 + i - 1] = `F${i}`;
}

/**
 * "Control + Shift + Space" -> [[224, 228], [225, 229], [44]]
 * Unknown keys make the whole keybind invalid (returns null).
 */
export function keybindToScancodes(keys: string[]): number[][] | null {
    const groups: number[][] = [];
    for (const key of keys) {
        const scancodes = DOM_TO_SCANCODES[key] ?? DOM_TO_SCANCODES[key.trim()];
        if (!scancodes) return null;
        groups.push(scancodes);
    }
    return groups.length > 0 ? groups : null;
}

export function scancodeToElectronKey(scancode: number): string | undefined {
    return SCANCODE_TO_ELECTRON[scancode];
}

// SDL_Keymod bits
const KMOD_SHIFT = 0x0003;
const KMOD_CTRL = 0x00c0;
const KMOD_ALT = 0x0300;
const KMOD_GUI = 0x0c00;

export type ElectronModifier = 'shift' | 'control' | 'alt' | 'meta';

export function sdlModsToElectron(mods: number): ElectronModifier[] {
    const out: ElectronModifier[] = [];
    if (mods & KMOD_SHIFT) out.push('shift');
    if (mods & KMOD_CTRL) out.push('control');
    if (mods & KMOD_ALT) out.push('alt');
    if (mods & KMOD_GUI) out.push('meta');
    return out;
}
