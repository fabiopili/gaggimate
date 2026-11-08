#ifndef DIMMEDPUMP_H
#define DIMMEDPUMP_H
#include "PSM.h"
#include "PressureController/PressureController.h"
#include "PressureSensor.h"
#include "Pump.h"
#include <Arduino.h>
#include <cstdint>
#include <functional>

class DimmedPump : public Pump {
  public:
    enum class ControlMode { POWER, PRESSURE, FLOW };

    DimmedPump(uint8_t ssr_pin, uint8_t sense_pin, PressureSensor *pressureSensor, PressureControlRate controlRate);
    ~DimmedPump() = default;

    void setup() override;
    void loop() override;
    void setPower(float setpoint) override;

    float getCoffeeVolume();
    float getPumpFlow();
    float getPuckFlow();
    float getPuckResistance();
    float getPressureTarget() { return _ctrlPressure; }
    float getFlowTarget() { return _ctrlFlow; }
    float *getPumpPowerPtr() override { return &_power; } // For external pump control
    float *getPumpFlowPtr() { return &_currentFlow; }     // For thermal feedforward
    float getPumpedWater() { return _pumpedWater; }
    int *getValveStatusPtr() { return &_valveStatus; } // For thermal feedforward valve state
    void tare();

    void setFlowTarget(float targetFlow, float pressureLimit);
    void setPressureTarget(float targetPressure, float flowLimit);
    void setPumpFlowCoeff(float oneBarFlow, float nineBarFlow);
    void setPumpFlowPolyCoeffs(float a, float b, float c, float d);
    void setPumpSlipPolyCoeffs(float a, float b, float c, float d);
    void setGains(float commutationGain, float convergenceGain, float integralGain);
    void setBinaryMode(bool binaryMode);

    // Brew valve sequencing. requestValve() replaces direct writes to the
    // valve GPIO: it forces full pump conduction, writes the valve once a
    // fired zero-cross decision proves the channel is conducting (or after
    // a fallback cap), and holds a guard window from the valve write.
    // setValveWriter() injects the GPIO write; everything else lives here so
    // the gate and the valve cannot drift apart.
    void requestValve(bool open);
    void setValveWriter(std::function<void(bool)> writer) { _valveWriteFn = std::move(writer); }

  private:
    uint8_t _ssr_pin;
    uint8_t _sense_pin;
    PSM _psm;
    PressureSensor *_pressureSensor;
    const PressureControlRate _controlRate;
    PressureController _pressureController;

    xTaskHandle taskHandle;

    ControlMode _mode = ControlMode::POWER;
    float _power = 0.0f;
    float _controllerPower = 0.0f;
    float _ctrlPressure = 0.0f;
    float _ctrlFlow = 0.0f;
    float _currentPressure = 0.0f;
    float _currentFlow = 0.0f;
    float _lastPressure = 0.0f;
    float _pumpedWater = 0.0f;
    int _valveStatus = 0;
    int _cps = MAX_FREQ;
    bool _binaryMode = false;

    float _opvPressure = 0.0f;

    // Valve sequencing state. _valveState tracks the commanded physical
    // valve state; every change flows through requestValve(), so it cannot
    // go stale the way a cached flag did when safety paths bypassed it.
    std::function<void(bool)> _valveWriteFn;
    bool _valveState = false;
    bool _valvePending = false;
    bool _pendingValveState = false;
    uint32_t _valveArmMs = 0;
    bool _conductionForced = false;
    bool _forcedPublishValid = false;
    long _counterAtPublish = 0;
    uint32_t _guardUntilMs = 0;

    // Zero-cross watchdog state.
    bool _zcStale = false;
    uint8_t _freshStreak = 0;

    static constexpr uint32_t VALVE_HOLD_OFF_CAP_MS = 60;
    static constexpr uint32_t GUARD_MS = 50;
    static constexpr uint32_t ZC_STALE_MS = 100;

    static constexpr float BASE_FLOW_RATE = 0.25f;
    static constexpr float MAX_PRESSURE = 15.0f;
    static constexpr float MAX_FREQ = 60.0f;

    void updatePower(bool freshPressure, float sampleTime, float elapsed);
    int64_t _lastUpdateUs = 0;
    void onPressureUpdate(float pressure);

    const char *LOG_TAG = "DimmedPump";
    static void loopTask(void *arg);
};

#endif // DIMMEDPUMP_H
