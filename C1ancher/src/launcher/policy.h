#ifndef C1_LAUNCHER_POLICY_H
#define C1_LAUNCHER_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define C1_LAUNCHER_UPDATE_EXIT 75
#define C1_LAUNCHER_CRASH_STORM_EXIT 70
#define C1_LAUNCHER_SHORT_RUNTIME_MS 30000U
#define C1_LAUNCHER_MAX_SHORT_CRASHES 5U

struct c1_launcher_observation {
    bool stop_requested;
    bool exited;
    int exit_code;
    uint64_t runtime_ms;
    unsigned int short_crashes;
};

enum c1_launcher_action {
    C1_LAUNCHER_RESTART,
    C1_LAUNCHER_UPDATE,
    C1_LAUNCHER_STOP,
    C1_LAUNCHER_FATAL
};

struct c1_launcher_decision {
    enum c1_launcher_action action;
    unsigned int short_crashes;
    unsigned int backoff_seconds;
};

struct c1_launcher_decision
c1_launcher_decide(const struct c1_launcher_observation *observation);

#endif
