#ifndef ATG_ENGINE_SIM_EXHAUST_PIPE_1D_H
#define ATG_ENGINE_SIM_EXHAUST_PIPE_1D_H

#include "gas_system.h"

#include <string>
#include <vector>

class ExhaustPipe1D {
public:
    enum class Limiter {
        Minmod,
        MC
    };

    struct Parameters {
        double length = 1.0;
        double crossSectionArea = 0.001;
        int cellCount = 24;
        double cfl = 0.45;
        double wallTemperature = units::celcius(120.0);
        double wallHeatTransfer = 18.0;
        double wallFriction = 12.0;
        double outletReflection = 0.35;
        double outletReflectionCutoff = 1800.0;
        Limiter limiter = Limiter::MC;
    };

    struct Diagnostics {
        double minPressure = 0.0;
        double maxPressure = 0.0;
        double minTemperature = 0.0;
        double maxTemperature = 0.0;
        int nanCount = 0;
    };

public:
    ExhaustPipe1D();

    void initialize(const Parameters &params);
    void destroy();

    double process(
        double dt,
        GasSystem *cylinder,
        GasSystem *collector,
        double valveFlowRate,
        double outletFlowRate);

    double inletPressure() const;
    double outletPressure() const;
    double outletVelocity() const;
    double outletAudioSignal() const;
    double maxStableTimestep() const;
    Diagnostics diagnostics() const;

    int cellCount() const { return static_cast<int>(m_cells.size()); }
    const Parameters &parameters() const { return m_params; }

    static Limiter limiterFromName(const std::string &name);
    static double limitSlope(double leftSlope, double rightSlope, Limiter limiter);

protected:
    struct Primitive {
        double rho = 1.2;
        double u = 0.0;
        double p = units::pressure(1.0, units::atm);
    };

    struct Conservative {
        double rho = 1.2;
        double momentum = 0.0;
        double energy = 0.0;
    };

    struct Flux {
        double mass = 0.0;
        double momentum = 0.0;
        double energy = 0.0;
    };

protected:
    void stepFiniteVolume(double dt);
    void applySourceTerms(double dt);
    double exchangeWithCylinder(GasSystem *cylinder, double valveFlowRate, double dt);
    void exchangeWithCollector(GasSystem *collector, double outletFlowRate, double dt);
    void addMassToCell(int index, double dn, double energyPerMol, const GasSystem::Mix &mix, double direction);
    void removeMassFromCell(int index, double dn);
    void sanitizeCell(Conservative *cell) const;

    Primitive primitiveFromConservative(const Conservative &state) const;
    Conservative conservativeFromPrimitive(const Primitive &state) const;
    Flux fluxFromPrimitive(const Primitive &state) const;
    Flux rusanovFlux(const Primitive &left, const Primitive &right) const;
    Primitive reflectedOutletGhost(const Primitive &last, double dt);

    double cellVolume() const;
    double cellLength() const;
    double specificGasConstant() const;
    double temperatureOf(const Primitive &state) const;

protected:
    Parameters m_params;
    std::vector<Conservative> m_cells;
    double m_reflectedPressure = 0.0;
    double m_outletAudioSignal = 0.0;
    double m_lastValveFlow = 0.0;
};

#endif /* ATG_ENGINE_SIM_EXHAUST_PIPE_1D_H */
