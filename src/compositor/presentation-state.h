#ifndef AMUX_PRESENTATION_STATE_H
#define AMUX_PRESENTATION_STATE_H

#include <stdbool.h>

enum presentation_state { VIEW_IDLE, VIEW_STARTING, VIEW_MAPPED, VIEW_FAILED };

static inline bool presentation_can_launch(enum presentation_state state,
        bool visible, bool ready, bool surface_exists, bool launch_exists) {
    return state == VIEW_IDLE && visible && ready && !surface_exists && !launch_exists;
}

static inline enum presentation_state presentation_retry(enum presentation_state state) {
    return state == VIEW_MAPPED ? VIEW_MAPPED : VIEW_IDLE;
}

#endif
