// Single decision point for the pump gate value.
//
// Every write to the PSM flows through resolvePumpOutput so that the
// zero-cross watchdog and the brew guard window cannot be undone by
// another writer. Precedence, highest first:
//   1. zero-cross staleness: return -1, the caller has already forced the
//      gate low and must not touch the PSM until pulses return
//   2. brew guard window: full conduction, the protective state for both
//      known failure mechanisms of the TRIAC channel
//   3. commanded power, binary-mapped if the pump is in binary mode
//
// Header-only and free of Arduino dependencies so the native test
// environment compiles it directly.

#ifndef PUMPOUTPUT_H
#define PUMPOUTPUT_H

#include <cstdint>

struct PumpOutputIntent {
    bool zcStale = false;
    bool guardActive = false;
    bool binaryMode = false;
    bool binaryOn = false;
    int power = 0; // proportional power, 0..100
};

inline int resolvePumpOutput(const PumpOutputIntent &intent) {
    if (intent.zcStale) {
        return -1;
    }
    if (intent.guardActive) {
        return 100;
    }
    int value = intent.binaryMode ? (intent.binaryOn ? 100 : 0) : intent.power;
    if (value < 0) {
        value = 0;
    }
    if (value > 100) {
        value = 100;
    }
    return value;
}

#endif // PUMPOUTPUT_H
