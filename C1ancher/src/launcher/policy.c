#include "launcher/policy.h"

bool c1_launcher_should_restart(bool stop_requested)
{
    return !stop_requested;
}