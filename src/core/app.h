// Cruller's services, the same on every board.

#pragma once

// Starts them in order, once the board's own bring-up is done (the watchdog armed, the network stack
// up), and confirms an updated image once the web server listens. Then logs that it's alive every 30 s.
void app_run(void) __attribute__((noreturn));
