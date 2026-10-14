// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#include "mono_sim_model.h"

#include <cmath>
#include <cstdio>

// Defined/exported by emc_server.cpp; share the IOC's existing debug switch.
extern int emc_server_debug;
#define Debug(L, ...) do { if ((L) <= emc_server_debug) { \
    std::fprintf(stderr, "[MonoSimModel] " __VA_ARGS__); \
    std::fprintf(stderr, "\t- file : " __FILE__ ", line %d\n", __LINE__); \
} } while (false)

namespace {
// Planck constant * speed of light, expressed in eV nm.
const double hc = 1239.8419843320026;

bool positiveFinite(double value) {
    return std::isfinite(value) && value > 0.0;
}

bool supportedEnergy(double energy) {
    return positiveFinite(energy) && std::isfinite(hc / energy);
}

const char* stateName(MonoSimModel::State state) {
    switch (state) {
    case MonoSimModel::State::Idle: return "idle";
    case MonoSimModel::State::MovingToTarget: return "moving-to-target";
    case MonoSimModel::State::MovingToStart: return "moving-to-start";
    case MonoSimModel::State::SweepReady: return "sweep-ready";
    case MonoSimModel::State::Sweeping: return "sweeping";
    }
    return "unknown";
}
} // namespace

MonoSimModel::MonoSimModel()
    : energy_(1000.0), target_(1000.0), sweep_start_(1000.0), sweep_end_(1000.0),
      velocity_(1.0), state_(State::Idle), have_time_(false), now_(0.0),
      move_start_time_(0.0), move_start_energy_(1000.0) {}

bool MonoSimModel::moving() const {
    return state_ == State::MovingToTarget || state_ == State::MovingToStart ||
           state_ == State::Sweeping;
}

double MonoSimModel::wavelength() const {
    return hc / energy_;
}

void MonoSimModel::advance(double now) {
    if (!std::isfinite(now) || (have_time_ && now < now_)) {
        Debug(2, "advance ignored: time=%.17g s previous=%.17g s established=%d",
              now, now_, have_time_);
        return;
    }
    have_time_ = true;
    now_ = now;
    if (!moving()) return;

    const double distance = std::fabs(target_ - move_start_energy_);
    const double travelled = velocity_ * (now_ - move_start_time_);
    if (travelled >= distance) {
        finishMove();
    } else {
        energy_ = move_start_energy_ + (target_ > move_start_energy_ ? travelled : -travelled);
    }
}

void MonoSimModel::finishMove() {
    energy_ = target_;
    state_ = state_ == State::MovingToStart ? State::SweepReady : State::Idle;
    Debug(2, "move complete: energy=%.17g eV time=%.17g s state=%s",
          energy_, now_, stateName(state_));
}

void MonoSimModel::beginMove(double target, State state) {
    move_start_energy_ = energy_;
    move_start_time_ = now_;
    target_ = target;
    state_ = state;
    Debug(2, "move start: energy=%.17g eV target=%.17g eV velocity=%.17g eV/s time=%.17g s state=%s",
          energy_, target_, velocity_, now_, stateName(state_));
    if (target_ == energy_) finishMove();
}

bool MonoSimModel::setEnergy(double energy) {
    if (!supportedEnergy(energy)) {
        Debug(1, "setEnergy rejected: %.17g eV must be positive and finite with finite hc/energy",
              energy);
        return false;
    }
    // Target commands replace an active move/sweep at the current position.
    beginMove(energy, State::MovingToTarget);
    return true;
}

bool MonoSimModel::setWavelength(double wavelength) {
    if (!positiveFinite(wavelength)) {
        Debug(1, "setWavelength rejected: %.17g nm must be positive and finite", wavelength);
        return false;
    }
    const double targetEnergy = hc / wavelength;
    if (!supportedEnergy(targetEnergy)) {
        Debug(1, "setWavelength rejected: %.17g nm converts to unsupported energy %.17g eV",
              wavelength, targetEnergy);
        return false;
    }
    return setEnergy(targetEnergy);
}

bool MonoSimModel::setSweepStart(double energy) {
    if (!supportedEnergy(energy) || moving()) {
        Debug(1, "setSweepStart rejected: energy=%.17g eV requires supported energy and no motion (state=%s)",
              energy, stateName(state_));
        return false;
    }
    sweep_start_ = energy;
    state_ = State::Idle; // A changed configuration needs another SI.
    Debug(2, "sweep start set: %.17g eV; sweep preparation required", sweep_start_);
    return true;
}

bool MonoSimModel::setSweepEnd(double energy) {
    if (!supportedEnergy(energy) || moving()) {
        Debug(1, "setSweepEnd rejected: energy=%.17g eV requires supported energy and no motion (state=%s)",
              energy, stateName(state_));
        return false;
    }
    sweep_end_ = energy;
    state_ = State::Idle;
    Debug(2, "sweep end set: %.17g eV; sweep preparation required", sweep_end_);
    return true;
}

bool MonoSimModel::setVelocity(double velocity) {
    if (!positiveFinite(velocity) || moving()) {
        Debug(1, "setVelocity rejected: velocity=%.17g eV/s requires positive finite velocity and no motion (state=%s)",
              velocity, stateName(state_));
        return false;
    }
    velocity_ = velocity;
    state_ = State::Idle;
    Debug(2, "velocity set: %.17g eV/s; sweep preparation required", velocity_);
    return true;
}

bool MonoSimModel::sweepInit() {
    if (moving()) {
        Debug(1, "sweepInit rejected: motion active (state=%s)", stateName(state_));
        return false;
    }
    beginMove(sweep_start_, State::MovingToStart);
    return true;
}

bool MonoSimModel::sweepRun() {
    if (state_ != State::SweepReady) {
        Debug(1, "sweepRun rejected: sweep preparation must complete (state=%s)", stateName(state_));
        return false;
    }
    beginMove(sweep_end_, State::Sweeping);
    return true;
}

void MonoSimModel::stop() {
    Debug(2, "stop: energy=%.17g eV previous-target=%.17g eV state=%s",
          energy_, target_, stateName(state_));
    target_ = energy_;
    state_ = State::Idle;
}
