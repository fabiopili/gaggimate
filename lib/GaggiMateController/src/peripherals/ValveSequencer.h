// Brew valve sequencing state machine and zero-cross stream watchdog for
// the pump TRIAC channel.
//
// ValveSequencer owns the ordering that keeps the TRIAC conducting across
// valve transitions: force full conduction, release the valve GPIO only
// once a fired zero-cross decision proves the channel conducts (or when a
// fallback cap expires), and hold a guard window from the valve write.
//
// Single-writer discipline: tick() runs on the pump task and owns every
// non-atomic member; request() and abortSequence() are safe from any task
// and communicate through atomics only. The clock, fired-decision counter
// and GPIO writer are injected, so the interleavings are testable on the
// host with the production logic.
//
// Header-only and free of Arduino dependencies.

#ifndef VALVESEQUENCER_H
#define VALVESEQUENCER_H

#include <atomic>
#include <cstdint>
#include <functional>

class ValveSequencer {
  public:
    enum class Event : uint8_t { None, ValveWritten, OpenRefusedWhileStale };
    enum class ReleaseReason : uint8_t { None, ConductionProof, HoldOffCap, Abort };

    struct Config {
        uint32_t holdOffCapMs; // fallback release when no conduction proof arrives
        uint32_t guardMs;      // forced-conduction window, armed at the valve write
    };
    using ClockFn = std::function<uint32_t()>;
    using CounterFn = std::function<long()>; // PSM fired-decision counter
    using WriterFn = std::function<void(bool)>;

    ValveSequencer(Config config, ClockFn clock, CounterFn counter, WriterFn writer)
        : config_(config), clock_(std::move(clock)), counter_(std::move(counter)),
          writer_(std::move(writer)) {}

    // Any task: store the requested valve state. Edge detection happens on
    // the pump task, so a request landing mid-transition is never lost.
    void request(bool open) { requested_.store(open, std::memory_order_release); }

    // Any task: cancel any pending transition and close immediately without
    // forced conduction. Used by the safety paths, where a sequenced close
    // would first drive the pump to full power for a guard window.
    void abortSequence() {
        requested_.store(false, std::memory_order_release);
        abort_.store(true, std::memory_order_release);
    }

    // The pump task calls this after publishing a PSM value. While a
    // transition is forcing full conduction, the first publish snapshots the
    // fired-decision counter: a later counter move proves a decision fired
    // after the forced value was published.
    void notePublish() {
        if (conductionForced_ && !publishValid_) {
            counterAtPublish_ = counter_();
            publishValid_ = true;
        }
    }

    // Pump task only. zeroCrossStale must be this tick's watchdog verdict.
    Event tick(bool zeroCrossStale) {
        const uint32_t now = clock_();
        if (abort_.exchange(false, std::memory_order_acq_rel)) {
            finish(false, now, ReleaseReason::Abort);
            return Event::ValveWritten;
        }
        const bool requested = requested_.load(std::memory_order_acquire);
        if (!pending_) {
            if (requested == valveState_) {
                expireGuard(now);
                return Event::None;
            }
            begin(requested, now);
        } else if (requested != pendingState_) {
            // The requested state changed while a transition was in flight:
            // retarget it and restart the fallback window. Forcing and any
            // conduction proof stay valid, since the forced 100 percent
            // publication predates both states.
            pendingState_ = requested;
            armMs_ = now;
        }
        const bool proof = publishValid_ && (counter_() - counterAtPublish_) > 0;
        const bool capExpired = (now - armMs_) >= config_.holdOffCapMs;
        if (proof || capExpired) {
            if (!proof && zeroCrossStale && pendingState_) {
                // Opening onto a blocking TRIAC with no zero-cross stream
                // would defeat the protection and the shot cannot run anyway.
                // Cancel the open instead of leaving it pending: a deferred
                // open would self-resume after recovery through the cap
                // branch without a fresh force-publish-proof cycle, restarting
                // a shot the display has already abandoned to an error screen.
                // The user restarts the shot; a safety abort still closes.
                requested_.store(false, std::memory_order_release);
                pending_ = false;
                conductionForced_ = false;
                publishValid_ = false;
                return Event::OpenRefusedWhileStale;
            }
            finish(pendingState_, now,
                   proof ? ReleaseReason::ConductionProof : ReleaseReason::HoldOffCap);
            guardArmed_ = true;
            guardUntilMs_ = now + config_.guardMs;
            return Event::ValveWritten;
        }
        expireGuard(now);
        return Event::None;
    }

    // True while the state machine is forcing full conduction (between the
    // request edge and the valve write, and through the guard window).
    bool forcingConduction() const { return conductionForced_; }
    bool guardActive() const { return guardArmed_ && (int32_t)(clock_() - guardUntilMs_) < 0; }
    bool valveState() const { return valveState_; }
    ReleaseReason reason() const { return releaseReason_; }

  private:
    void begin(bool state, uint32_t now) {
        pending_ = true;
        pendingState_ = state;
        armMs_ = now;
        conductionForced_ = true;
        publishValid_ = false;
    }
    void finish(bool state, uint32_t now, ReleaseReason reason) {
        writer_(state);
        valveState_ = state;
        pending_ = false;
        conductionForced_ = false;
        publishValid_ = false;
        releaseReason_ = reason;
        guardArmed_ = false;
        guardUntilMs_ = now;
    }
    // The armed flag makes expiry explicit. Relying on the signed deadline
    // difference alone latches the guard on once millis() passes 2^31 on a
    // machine that has been idle since before the last transition.
    void expireGuard(uint32_t now) {
        if (guardArmed_ && (int32_t)(now - guardUntilMs_) >= 0) {
            guardArmed_ = false;
        }
    }

    Config config_;
    ClockFn clock_;
    CounterFn counter_;
    WriterFn writer_;

    std::atomic<bool> requested_{false};
    std::atomic<bool> abort_{false};

    // Pump-task-only state below this line.
    bool valveState_ = false;
    bool pending_ = false;
    bool pendingState_ = false;
    bool conductionForced_ = false;
    bool publishValid_ = false;
    long counterAtPublish_ = 0;
    uint32_t armMs_ = 0;
    bool guardArmed_ = false;
    uint32_t guardUntilMs_ = 0;
    ReleaseReason releaseReason_ = ReleaseReason::None;
};

// Age test for the zero-cross pulse stream. The pulse timestamp is written by
// the ISR and can be newer than a clock sample taken a few microseconds
// earlier on the task; a plain unsigned subtraction would then wrap to a huge
// age and trip the watchdog falsely. The signed difference treats a newer
// pulse as fresh and survives the millis() wrap.
inline bool zeroCrossPulseFresh(uint32_t nowMs, uint32_t lastPulseMs, uint32_t thresholdMs) {
    return static_cast<int32_t>(nowMs - lastPulseMs) <= static_cast<int32_t>(thresholdMs);
}

// Zero-cross stream health. Enters the stale state on a single stale check
// and leaves it after two consecutive fresh checks, so one late pulse (or
// one flash-write stall shorter than the threshold) cannot flap the state.
class ZeroCrossWatchdog {
  public:
    // freshPulses: true when the last accepted zero-cross pulse is newer
    // than the staleness threshold. Returns true while stale.
    bool update(bool freshPulses) {
        if (!freshPulses) {
            freshStreak_ = 0;
            stale_ = true;
        } else if (stale_ && ++freshStreak_ >= 2) {
            stale_ = false;
            freshStreak_ = 0;
        }
        return stale_;
    }

  private:
    bool stale_ = false;
    uint8_t freshStreak_ = 0;
};

#endif // VALVESEQUENCER_H
