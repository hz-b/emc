// SPDX-FileCopyrightText: 2026 Helmholtz-Zentrum Berlin für Materialien und Energie GmbH
// SPDX-License-Identifier: MIT

#ifndef MONO_SIM_MODEL_H
#define MONO_SIM_MODEL_H

// Not thread-safe: callers serialize all calls.
// Call advance(now) first, and before every read or change.
//
// Logging via emc_server_debug 
//   0 quiet, 1 rejected commands, 2 also motion/config changes.
//
// Units: eV, nm, eV/s. Inputs must be positive and finite, and
// hc/E (hc = 1239.8419843320026 eV nm) must be finite.
// A rejected setter returns false and changes nothing.

class MonoSimModel {
public:
    enum class State { Idle, MovingToTarget, MovingToStart, SweepReady, Sweeping };

    MonoSimModel();
    void advance(double now); // Monotonic seconds; backward/invalid time is ignored.

    double energy() const { return energy_; } // eV
    double wavelength() const; // nm; always positive and finite for supported energy
    double target() const { return target_; }
    double sweepStart() const { return sweep_start_; }
    double sweepEnd() const { return sweep_end_; }
    double velocity() const { return velocity_; } // eV/s, for every kind of move
    State state() const { return state_; }
    bool moving() const;
    long status() const { return moving() ? 1L : 0L; }

    bool setEnergy(double energy);
    bool setWavelength(double wavelength);
    bool setSweepStart(double energy);
    bool setSweepEnd(double energy);
    bool setVelocity(double velocity);
    bool sweepInit();
    bool sweepRun();
    void stop();

private:
    void beginMove(double target, State state);
    void finishMove();

    double energy_;
    double target_;
    double sweep_start_;
    double sweep_end_;
    double velocity_;
    State state_;
    bool have_time_;
    double now_;
    double move_start_time_;
    double move_start_energy_;
};
#endif // MONO_SIM_MODEL_H
