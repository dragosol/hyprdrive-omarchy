#pragma once

// Pushes the app-requested cursor shape to IPC as `cursorshape>>NAME` (see CursorShape.cpp).
namespace CursorShape {
void init();
void shutdown();
}
