#include <gtest/gtest.h>

#include "../include/exhaust_pipe_1d.h"
#include "../include/units.h"

#include <cmath>

TEST(ExhaustPipe1DTests, LimiterSuppressesOppositeSlopes) {
    EXPECT_DOUBLE_EQ(
        ExhaustPipe1D::limitSlope(1.0, -1.0, ExhaustPipe1D::Limiter::MC),
        0.0);
    EXPECT_DOUBLE_EQ(
        ExhaustPipe1D::limitSlope(-1.0, 1.0, ExhaustPipe1D::Limiter::Minmod),
        0.0);
}

TEST(ExhaustPipe1DTests, MCLimiterIsLessDiffusiveThanMinmod) {
    const double minmod = ExhaustPipe1D::limitSlope(1.0, 3.0, ExhaustPipe1D::Limiter::Minmod);
    const double mc = ExhaustPipe1D::limitSlope(1.0, 3.0, ExhaustPipe1D::Limiter::MC);

    EXPECT_DOUBLE_EQ(minmod, 1.0);
    EXPECT_GT(mc, minmod);
    EXPECT_LE(mc, 2.0);
}

TEST(ExhaustPipe1DTests, StableTimestepIsPositive) {
    ExhaustPipe1D pipe;
    ExhaustPipe1D::Parameters params;
    params.length = 1.0 * units::m;
    params.crossSectionArea = 10.0 * units::cm2;
    params.cellCount = 24;
    params.cfl = 0.45;

    pipe.initialize(params);

    EXPECT_GT(pipe.maxStableTimestep(), 0.0);
    EXPECT_TRUE(std::isfinite(pipe.maxStableTimestep()));
}

TEST(ExhaustPipe1DTests, ValveFlowMovesMassFromHotCylinderIntoPipe) {
    ExhaustPipe1D pipe;
    ExhaustPipe1D::Parameters params;
    params.length = 1.0 * units::m;
    params.crossSectionArea = 10.0 * units::cm2;
    params.cellCount = 12;
    pipe.initialize(params);

    GasSystem cylinder;
    cylinder.initialize(
        units::pressure(4.0, units::atm),
        units::volume(500.0, units::cc),
        units::celcius(900.0));

    GasSystem collector;
    collector.initialize(
        units::pressure(1.0, units::atm),
        units::volume(5.0, units::L),
        units::celcius(120.0));

    const double cylinderMolesBefore = cylinder.n();
    const double flow = pipe.process(
        1.0 / 10000.0,
        &cylinder,
        &collector,
        GasSystem::k_28inH2O(250.0),
        GasSystem::k_carb(250.0));

    EXPECT_GT(flow, 0.0);
    EXPECT_LT(cylinder.n(), cylinderMolesBefore);

    const ExhaustPipe1D::Diagnostics diagnostics = pipe.diagnostics();
    EXPECT_EQ(diagnostics.nanCount, 0);
    EXPECT_GT(diagnostics.minPressure, 0.0);
    EXPECT_GT(diagnostics.minTemperature, 0.0);
}

TEST(ExhaustPipe1DTests, ChokedFlowDirectionFollowsPressureRatio) {
    const double forward = GasSystem::flowRate(
        GasSystem::k_28inH2O(250.0),
        units::pressure(5.0, units::atm),
        units::pressure(1.0, units::atm),
        units::celcius(900.0),
        units::celcius(120.0),
        1.4,
        GasSystem::chokedFlowLimit(5),
        GasSystem::chokedFlowRate(5));

    const double reverse = GasSystem::flowRate(
        GasSystem::k_28inH2O(250.0),
        units::pressure(1.0, units::atm),
        units::pressure(5.0, units::atm),
        units::celcius(120.0),
        units::celcius(900.0),
        1.4,
        GasSystem::chokedFlowLimit(5),
        GasSystem::chokedFlowRate(5));

    EXPECT_GT(forward, 0.0);
    EXPECT_LT(reverse, 0.0);
}
