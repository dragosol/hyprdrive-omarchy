// Logs pointer enter/leave/motion: shows whether hover reaches a window that
// dock chrome is covering (nested pass-through test).
#include <SDL3/SDL.h>
#include <stdio.h>
int main(void) {
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* w = SDL_CreateWindow("hoverlog", 900, 700, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* r = SDL_CreateRenderer(w, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    // Draw ONCE, then only wait for events: a nested compositor on a hidden
    // desktop sends no frame callbacks, and a present-per-loop client blocks
    // there and stops reading pointer events.
    SDL_SetRenderVSync(r, 0);
    SDL_SetRenderDrawColor(r, 60, 90, 140, 255); SDL_RenderClear(r); SDL_RenderPresent(r);
    for (;;) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100))
            continue;
        if (e.type == SDL_EVENT_QUIT) return 0;
        if (e.type == SDL_EVENT_WINDOW_MOUSE_ENTER) printf("enter\n");
        if (e.type == SDL_EVENT_WINDOW_MOUSE_LEAVE) printf("leave\n");
        if (e.type == SDL_EVENT_MOUSE_MOTION) printf("motion %.0f %.0f\n", e.motion.x, e.motion.y);
    }
}
