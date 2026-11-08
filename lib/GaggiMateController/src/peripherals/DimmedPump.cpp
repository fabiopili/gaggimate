#include "DimmedPump.h"

#include <ExtensionIOXL9555.hpp>
#include <GaggiMateController.h>
#include <peripherals/PumpOutput.h>

DimmedPump::DimmedPump(uint8_t ssr_pin, uint8_t sense_pin, PressureSensor *pressure_sensor, PressureControlRate controlRate)
    : _ssr_pin(ssr_pin), _sense_pin(sense_pin), _psm(_sense_pin, _ssr_pin, 100, FALLING, 1, 4), _pressureSensor(pressure_sensor),
      _controlRate(controlRate), _pressureController(pressureControlPeriodS(controlRate), &_ctrlPressure, &_ctrlFlow,
                                                     &_currentPressure, &_controllerPower, &_valveStatus) {
    _psm.set(0);
}

void DimmedPump::setup() {
    _cps = _psm.cps();
    ESP_LOGI("DimmedPump", "Detected ZC frequency: %dHz", _cps);
    if (_cps > 70) {
        _cps = _cps / 2;
        _psm.setDivider(2);
        ESP_LOGI("DimmedPump", "Detected 2 signals per wave, dividing down to %dHz", _cps);
    }
    xTaskCreate(loopTask, "DimmedPump::loop", configMINIMAL_STACK_SIZE * 4, this, 1, &taskHandle);
}

void DimmedPump::loop() {
    const int64_t nowUs = esp_timer_get_time();
    const uint32_t nowMs = millis();
    const float elapsed = _lastUpdateUs ? (nowUs - _lastUpdateUs) / 1000000.0f : pressureControlPeriodS(_controlRate);
    _lastUpdateUs = nowUs;
    updateZeroCrossWatchdog(nowMs);
    runValveSequencer(nowMs);
    const bool fresh = _pressureSensor->consumeAverage();
    if (fresh)
        _currentPressure = _pressureSensor->getAveragedPressure();
    updatePower(fresh, _pressureSensor->getSampleTime(), elapsed);
    _currentFlow = _pressureController.getPumpFlowRate();
}

void DimmedPump::setPower(float setpoint) {
    ESP_LOGV(LOG_TAG, "Setting power to %2f", setpoint);
    _ctrlPressure = setpoint > 0 ? 20.0f : 0.0f;
    _mode = ControlMode::POWER;
    _power = std::clamp(setpoint, 0.0f, 100.0f);
    _controllerPower = _power; // Feed manual control back into pressure controller
    _ctrlFlow = 0.0f;
    _ctrlPressure = 0.0f;
    if (_power == 0.0f) {
        _currentFlow = 0.0f;
    }
    // No direct PSM write here: updatePower() publishes the resolver's value
    // on the next pump tick (at most one control interval later). This keeps
    // updatePower() the single PSM writer and lets the watchdog and the guard
    // override anything the comms task commands.
}

float DimmedPump::getCoffeeVolume() { return _pressureController.getCoffeeOutputEstimate(); }

float DimmedPump::getPumpFlow() { return _currentFlow; }

float DimmedPump::getPuckFlow() { return _pressureController.getCoffeeFlowRate(); }

float DimmedPump::getPuckResistance() { return _pressureController.getPuckResistance(); }

void DimmedPump::tare() {
    _pumpedWater = 0.0f;
    _pressureController.tare();
    _pressureController.reset();
}

void DimmedPump::loopTask(void *arg) {
    auto *pump = static_cast<DimmedPump *>(arg);
    TickType_t lastWake = xTaskGetTickCount();
    uint8_t intervalIndex = 0;
    while (true) {
        pump->loop();
        xTaskDelayUntil(&lastWake, pdMS_TO_TICKS(pressureControlIntervalMs(pump->_controlRate, intervalIndex)));
        intervalIndex = (intervalIndex + 1) % 3;
    }
}

void DimmedPump::updatePower(bool freshPressure, float sampleTime, float elapsed) {
    _pumpedWater += _currentFlow * elapsed;
    _pressureController.update(static_cast<PressureController::ControlMode>(_mode), freshPressure, sampleTime, elapsed);
    if (_mode != ControlMode::POWER) {
        _power = _controllerPower;
    }
    PumpOutputIntent intent;
    intent.zcStale = _zcStale;
    intent.guardActive = _conductionForced || (int32_t)(millis() - _guardUntilMs) < 0;
    intent.binaryMode = _binaryMode;
    intent.binaryOn = (_controllerPower > 0 || _ctrlPressure > 0 || _ctrlFlow > 0);
    intent.power = static_cast<int>(_power);
    const int output = resolvePumpOutput(intent);
    if (output < 0) {
        // Zero-cross stream stale: the watchdog has already forced the gate
        // low. Publish nothing until fresh pulses return.
        return;
    }
    _psm.set(static_cast<unsigned int>(output));
    if (_valvePending && !_forcedPublishValid) {
        // First publish while a valve transition is pending. Snapshot the
        // fired-decision counter: the valve write is released once the
        // counter moves, proving a zero-cross decision fired after the
        // forced 100 percent was published.
        _counterAtPublish = _psm.getCounter();
        _forcedPublishValid = true;
    }
}

void DimmedPump::setFlowTarget(float targetFlow, float pressureLimit) {
    _mode = ControlMode::FLOW;
    _ctrlFlow = targetFlow;
    _ctrlPressure = pressureLimit;
    _pressureController.setPressureLimit(pressureLimit);
}

void DimmedPump::setPressureTarget(float targetPressure, float flowLimit) {
    _mode = ControlMode::PRESSURE;
    _ctrlFlow = flowLimit;
    _ctrlPressure = targetPressure;
    _pressureController.setFlowLimit(flowLimit);
}

void DimmedPump::setBinaryMode(bool binaryMode) { _binaryMode = binaryMode; }

void DimmedPump::requestValve(bool open) {
    if (_valveState == open && !_valvePending) {
        _valveStatus = open ? 1 : 0;
        return;
    }
    _valveState = open;
    _valveStatus = open ? 1 : 0;
    _pendingValveState = open;
    _valvePending = true;
    _valveArmMs = millis();
    _forcedPublishValid = false;
    // Force full conduction from now until the valve write; the guard window
    // then continues from the write. Conducting is the safe state for both
    // known failure mechanisms of the TRIAC channel.
    _conductionForced = true;
}

void DimmedPump::updateZeroCrossWatchdog(uint32_t nowMs) {
    // The PSM gate is written only inside the zero-cross ISR, so lost pulses
    // leave the gate frozen in its last state. While the pulse stream is
    // stale, force the gate low from the task side every tick and withhold
    // every other PSM write via the resolver. Recovery needs two consecutive
    // fresh checks so a single late pulse cannot flap the state.
    const bool stale = (nowMs - _psm.getLastMillis()) > ZC_STALE_MS;
    if (stale) {
        _freshStreak = 0;
        if (!_zcStale) {
            ESP_LOGE(LOG_TAG, "Zero-cross stream stale, forcing pump gate off");
        }
        _zcStale = true;
        _psm.set(0);
        digitalWrite(_ssr_pin, LOW);
        return;
    }
    if (_zcStale) {
        if (++_freshStreak < 2) {
            return;
        }
        ESP_LOGI(LOG_TAG, "Zero-cross stream recovered, pump control resumes");
        _zcStale = false;
        _freshStreak = 0;
    }
}

void DimmedPump::runValveSequencer(uint32_t nowMs) {
    if (!_valvePending) {
        return;
    }
    // Release the valve write when a fired zero-cross decision proves the
    // channel is conducting (the fired-decision counter moved past the
    // snapshot taken at the forced publish), or when the fallback cap
    // expires on a dead zero-cross path, where the watchdog owns the gate.
    const bool conductionProven = (long)(_psm.getCounter() - _counterAtPublish) > 0;
    const bool capExpired = (nowMs - _valveArmMs) >= VALVE_HOLD_OFF_CAP_MS;
    if (!conductionProven && !capExpired) {
        return;
    }
    if (_valveWriteFn) {
        _valveWriteFn(_pendingValveState);
    }
    _valvePending = false;
    _forcedPublishValid = false;
    _conductionForced = false;
    _guardUntilMs = nowMs + GUARD_MS;
    ESP_LOGI(LOG_TAG, "Valve %s written after %s", _pendingValveState ? "open" : "closed",
             conductionProven ? "conduction proof" : "hold-off cap");
}

void DimmedPump::setPumpFlowCoeff(float oneBarFlow, float nineBarFlow) {
    _pressureController.setPumpFlowCoeff(oneBarFlow, nineBarFlow);
}

void DimmedPump::setPumpFlowPolyCoeffs(float a, float b, float c, float d) {
    _pressureController.setPumpFlowPolyCoeffs(a, b, c, d);
}

void DimmedPump::setPumpSlipPolyCoeffs(float a, float b, float c, float d) {
    _pressureController.setPumpSlipPolyCoeffs(a, b, c, d);
}

void DimmedPump::setGains(float commutationGain, float convergenceGain, float integralGain) {
    _pressureController.setGains(commutationGain, convergenceGain, integralGain);
}
