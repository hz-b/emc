#include "mono_sim_model.h"

#include <cmath>
#include <limits>
#include <epicsUnitTest.h>
#include <testMain.h>

namespace {
bool near(double actual, double expected) {
    return std::fabs(actual - expected) < 1e-9;
}

void testTargetMoves() {
    testDiag("target motion, elapsed time, stopping and retargeting");
    MonoSimModel model;
    testOk(model.status() == 0 && model.energy() == 1000.0 && model.velocity() == 1.0,
           "initial energy 1000 eV, speed 1 eV/s, ready");
    model.advance(10.0);
    testOk1(model.setEnergy(1004.0));
    testOk(model.status() == 1 && model.state() == MonoSimModel::State::MovingToTarget,
           "target command starts motion");
    model.advance(11.5);
    testOk(near(model.energy(), 1001.5) && model.status() == 1,
           "intermediate energy follows elapsed time");
    model.advance(50.0);
    testOk(model.energy() == 1004.0 && model.status() == 0,
           "long gap without polling clamps to target");
    model.advance(60.0);
    testOk1(model.energy() == 1004.0);
    testOk1(model.setVelocity(2.0));
    testOk1(model.setEnergy(994.0));
    model.advance(61.25);
    testOk(near(model.energy(), 1001.5), "descending move uses the same speed");
    model.stop();
    model.advance(80.0);
    testOk(near(model.energy(), 1001.5) && model.status() == 0,
           "stop freezes the current position");
    testOk1(model.setEnergy(995.5));
    model.advance(81.0);
    testOk1(near(model.energy(), 999.5));
    testOk1(model.setEnergy(1003.5));
    model.advance(82.0);
    testOk(near(model.energy(), 1001.5), "retarget reverses from the current energy");
    model.advance(83.0);
    testOk(model.energy() == 1003.5 && model.status() == 0,
           "retargeted motion completes at the exact target");
    testOk1(model.setEnergy(1003.5));
    testOk(model.status() == 0, "zero-distance target is ready immediately");
}

void testSweeps() {
    testDiag("sweep preparation, running and configuration changes");
    MonoSimModel model;
    model.advance(0.0);
    testOk(!model.sweepRun(), "SR requires completed SI");
    testOk1(model.setSweepStart(1002.0));
    testOk1(model.setSweepEnd(1008.0));
    testOk1(model.setVelocity(2.0));
    testOk1(model.sweepInit());
    model.advance(0.5);
    testOk(near(model.energy(), 1001.0) && model.status() == 1,
           "SI moves to the start using SSV speed");
    testOk(!model.sweepRun() && !model.sweepInit(), "SI and SR fail during preparation");
    testOk(!model.setSweepStart(900.0) && !model.setSweepEnd(1100.0) &&
           !model.setVelocity(5.0), "sweep configuration cannot change during motion");
    testOk(model.sweepStart() == 1002.0 && model.sweepEnd() == 1008.0 &&
           model.velocity() == 2.0, "rejected setters preserve sweep settings");
    model.advance(1.0);
    testOk(model.state() == MonoSimModel::State::SweepReady && model.status() == 0,
           "completed SI is stationary and ready");
    testOk1(model.sweepRun());
    model.advance(2.0);
    testOk(near(model.energy(), 1004.0) && model.status() == 1,
           "SR sweeps toward the configured end");
    testOk(!model.sweepRun(), "repeated SR fails during a sweep");
    model.advance(20.0);
    testOk(model.energy() == 1008.0 && model.state() == MonoSimModel::State::Idle &&
           model.status() == 0, "sweep clamps to end and becomes idle");
    testOk(!model.sweepRun(), "another sweep needs another SI");
    testOk1(model.sweepInit());
    model.advance(23.0);
    testOk1(model.state() == MonoSimModel::State::SweepReady);
    testOk1(model.setVelocity(3.0));
    testOk(!model.sweepRun(), "changing speed invalidates sweep readiness");
    testOk1(model.sweepInit());
    model.stop();
    testOk(!model.sweepRun() && model.status() == 0, "STO cancels sweep readiness");

    MonoSimModel descending;
    descending.advance(0.0);
    testOk1(descending.setSweepEnd(996.0));
    testOk1(descending.setVelocity(2.0));
    testOk1(descending.sweepInit());
    testOk1(descending.sweepRun());
    descending.advance(1.0);
    testOk(near(descending.energy(), 998.0) && descending.status() == 1,
           "descending sweep uses positive SSV and direction from endpoints");
    descending.advance(2.0);
    testOk(descending.energy() == 996.0 && descending.status() == 0,
           "descending sweep completes");

    MonoSimModel zero;
    zero.advance(0.0);
    testOk(zero.sweepInit() && zero.sweepRun() && zero.status() == 0,
           "zero-length sweep completes immediately");
}

void testInvalidInputsAndClock() {
    testDiag("invalid values and clock rollback cannot corrupt motion");
    MonoSimModel model;
    model.advance(10.0);
    testOk1(model.setEnergy(1010.0));
    model.advance(12.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    testOk(!model.setEnergy(0.0) && !model.setEnergy(-1.0) &&
           !model.setEnergy(nan) && !model.setEnergy(inf), "invalid targets fail");
    testOk(model.energy() == 1002.0 && model.target() == 1010.0 && model.status() == 1,
           "failed target commands preserve motion");
    model.advance(11.0);
    model.advance(nan);
    model.advance(inf);
    testOk1(model.energy() == 1002.0);
    model.advance(13.0);
    testOk(model.energy() == 1003.0, "motion resumes normally after invalid clock values");
    model.stop();
    testOk(!model.setVelocity(0.0) && !model.setVelocity(-1.0) &&
           !model.setVelocity(nan) && !model.setVelocity(inf), "invalid speeds fail");
    testOk(!model.setSweepStart(0.0) && !model.setSweepEnd(nan), "invalid endpoints fail");
    testOk(model.velocity() == 1.0 && model.sweepStart() == 1000.0 &&
           model.sweepEnd() == 1000.0, "invalid settings preserve defaults");
}

void testWavelength() {
    testDiag("wavelength in nm shares the energy position and motion speed");
    MonoSimModel model;
    model.advance(0.0);
    testOk(near(model.wavelength(), 1.2398419843320026), "1000 eV corresponds to 1.23984 nm");
    testOk1(model.setWavelength(12.398419843320026));
    model.advance(1000.0);
    testOk(near(model.energy(), 100.0) && near(model.wavelength(), 12.398419843320026),
           "wavelength target converts to energy and finishes there");
    testOk(!model.setWavelength(0.0) && !model.setWavelength(-1.0) &&
           !model.setWavelength(std::numeric_limits<double>::infinity()) &&
           !model.setWavelength(std::numeric_limits<double>::denorm_min()),
           "invalid wavelength and conversion overflow fail");
    testOk(near(model.energy(), 100.0) && model.status() == 0,
           "failed wavelength changes preserve the position");
}

void testNumericRange() {
    testDiag("energy and sweep endpoints keep wavelength positive and finite");
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double smallestNormal = std::numeric_limits<double>::min();
    const double largest = std::numeric_limits<double>::max();
    const double hc = 1239.8419843320026;
    // Move one representable step above the approximate overflow boundary.
    const double lower = std::nextafter(hc / largest, largest);

    MonoSimModel model;
    model.advance(0.0);
    testOk1(model.setEnergy(1010.0));
    model.advance(2.0);
    testOk(!model.setEnergy(tiny) && !model.setEnergy(smallestNormal) &&
           !model.setEnergy(lower / 2.0), "targets whose wavelength overflows fail");
    testOk(model.energy() == 1002.0 && model.target() == 1010.0 &&
           model.state() == MonoSimModel::State::MovingToTarget,
           "overflow rejection preserves the active move and current position");
    model.advance(3.0);
    testOk(model.energy() == 1003.0, "active move continues after overflow rejection");
    model.stop();
    testOk1(model.sweepInit());
    model.advance(6.0);
    testOk1(model.state() == MonoSimModel::State::SweepReady);
    testOk(!model.setSweepStart(tiny) && !model.setSweepEnd(tiny) &&
           !model.setSweepStart(smallestNormal) && !model.setSweepEnd(smallestNormal) &&
           !model.setSweepStart(lower / 2.0) && !model.setSweepEnd(lower / 2.0),
           "both sweep endpoints reject wavelength overflow");
    testOk(model.sweepStart() == 1000.0 && model.sweepEnd() == 1000.0 &&
           model.state() == MonoSimModel::State::SweepReady,
           "rejected endpoints preserve configuration and prepared sweep state");
    model.stop();

    testOk1(model.setVelocity(largest));
    testOk1(model.setEnergy(lower));
    model.advance(7.0);
    testOk(model.energy() == lower && model.status() == 0 &&
           model.wavelength() > 0.0 && std::isfinite(model.wavelength()),
           "energy near the lower boundary produces finite wavelength");
    testOk1(model.setVelocity(lower));
    testOk1(model.setEnergy(2.0 * lower));
    model.advance(7.5);
    testOk(model.energy() >= lower && model.energy() <= 2.0 * lower &&
           model.status() == 1 && std::isfinite(model.wavelength()),
           "interpolation near the lower boundary preserves supported energy");
    testOk1(model.setEnergy(lower));
    model.advance(7.75);
    testOk(model.energy() >= lower && model.status() == 1 &&
           std::isfinite(model.wavelength()),
           "descending retarget near the lower boundary keeps finite wavelength");
    model.advance(8.0);
    testOk(model.energy() == lower && model.status() == 0,
           "descending move finishes exactly at the supported lower target");
    testOk1(model.setVelocity(largest));
    testOk1(model.setEnergy(largest));
    model.advance(9.0);
    testOk(model.energy() == largest && model.status() == 0 &&
           model.wavelength() > 0.0 && std::isfinite(model.wavelength()),
           "largest finite energy produces positive finite wavelength");

    testOk1(model.setWavelength(largest));
    model.advance(10.0);
    testOk(model.energy() > 0.0 && std::isfinite(model.energy()) &&
           model.wavelength() > 0.0 && std::isfinite(model.wavelength()),
           "largest finite wavelength converts to supported energy");
    testOk1(model.setWavelength(lower));
    model.advance(11.0);
    testOk(model.energy() > 0.0 && std::isfinite(model.energy()) &&
           model.wavelength() > 0.0 && std::isfinite(model.wavelength()),
           "wavelength near the lower boundary converts to supported energy");

    MonoSimModel sweep;
    sweep.advance(0.0);
    testOk(sweep.setSweepStart(lower) && sweep.setSweepEnd(largest) &&
           sweep.setVelocity(largest), "sweep accepts supported boundary endpoints");
    testOk1(sweep.sweepInit());
    sweep.advance(1.0);
    testOk(sweep.energy() == lower && sweep.state() == MonoSimModel::State::SweepReady &&
           std::isfinite(sweep.wavelength()), "sweep preparation reaches finite lower boundary");
    testOk1(sweep.sweepRun());
    sweep.advance(2.0);
    testOk(sweep.energy() == largest && sweep.status() == 0 &&
           sweep.wavelength() > 0.0 && std::isfinite(sweep.wavelength()),
           "boundary sweep completes with positive finite wavelength");
}
} // namespace

MAIN(testMonoSimModel) {
    testPlan(0);
    testTargetMoves();
    testSweeps();
    testInvalidInputsAndClock();
    testWavelength();
    testNumericRange();
    return testDone();
}
