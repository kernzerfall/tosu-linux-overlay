/* Compile-time check of the SDL3 ABI assumptions in src/input.c. */
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdio.h>

#define CHECK(expr) _Static_assert(expr, #expr)

CHECK(sizeof(SDL_Event) == 128);
CHECK(SDL_EVENT_WINDOW_FOCUS_LOST == 0x20F);
CHECK(SDL_EVENT_KEY_DOWN == 0x300);
CHECK(SDL_EVENT_KEY_UP == 0x301);
CHECK(SDL_EVENT_TEXT_EDITING == 0x302);
CHECK(SDL_EVENT_TEXT_INPUT == 0x303);
CHECK(SDL_EVENT_TEXT_EDITING_CANDIDATES == 0x307);
CHECK(SDL_EVENT_MOUSE_MOTION == 0x400);
CHECK(SDL_EVENT_MOUSE_BUTTON_DOWN == 0x401);
CHECK(SDL_EVENT_MOUSE_BUTTON_UP == 0x402);
CHECK(SDL_EVENT_MOUSE_WHEEL == 0x403);
CHECK(SDL_GETEVENT == 2);
CHECK(SDL_MOUSEWHEEL_FLIPPED == 1);

CHECK(offsetof(SDL_WindowEvent, windowID) == 16);
CHECK(offsetof(SDL_KeyboardEvent, windowID) == 16);
CHECK(offsetof(SDL_KeyboardEvent, scancode) == 24);
CHECK(offsetof(SDL_KeyboardEvent, mod) == 32);
CHECK(offsetof(SDL_KeyboardEvent, down) == 36);
CHECK(offsetof(SDL_KeyboardEvent, repeat) == 37);
CHECK(offsetof(SDL_MouseMotionEvent, x) == 28);
CHECK(offsetof(SDL_MouseMotionEvent, y) == 32);
CHECK(offsetof(SDL_MouseButtonEvent, button) == 24);
CHECK(offsetof(SDL_MouseButtonEvent, down) == 25);
CHECK(offsetof(SDL_MouseButtonEvent, x) == 28);
CHECK(offsetof(SDL_MouseButtonEvent, y) == 32);
CHECK(offsetof(SDL_MouseWheelEvent, x) == 24);
CHECK(offsetof(SDL_MouseWheelEvent, y) == 28);
CHECK(offsetof(SDL_MouseWheelEvent, direction) == 32);
CHECK(offsetof(SDL_MouseWheelEvent, mouse_x) == 36);
CHECK(offsetof(SDL_MouseWheelEvent, mouse_y) == 40);
CHECK(offsetof(SDL_TextInputEvent, text) == 24);

int main(void) {
    puts("SDL ABI assumptions hold");
    return 0;
}
