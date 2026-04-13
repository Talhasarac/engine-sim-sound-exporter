#include "../include/exhaust_pipe_1d.h"

#include "../include/constants.h"
#include "../include/units.h"
#include "../include/utilities.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>

namespace {
    constexpr double Gamma = 1.4;
    constexpr double AtmosphericPressure = units::pressure(1.0, units::atm);
    constexpr double AmbientTemperature = units::celcius(25.0);
    constexpr double MinPressure = units::pressure(0.05, units::atm);
    constexpr double MaxPressure = units::pressure(200.0, units::atm);
    constexpr double MinTemperature = 80.0;
    constexpr double MaxTemperature = 3500.0;
    constexpr double MinDensity = 1.0e-5;
    constexpr double MaxDensity = 50.0;

    char normalizedNameChar(char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    double clampFinite(double value, double minValue, double maxValue, double fallback) {
        if (!std::isfinite(value)) {
            return fallback;
        }

        return std::clamp(value, minValue, maxValue);
    }
}

ExhaustPipe1D::ExhaustPipe1D() {
    /* void */
}

void ExhaustPipe1D::initialize(const Parameters &params) {
    m_params = params;
    m_params.cellCount = std::clamp(m_params.cellCount, 4, 256);
    m_params.length = std::max(m_params.length, units::distance(1.0, units::cm));
    m_params.crossSectionArea = std::max(m_params.crossSectionArea, units::area(0.05, units::cm2));
    m_params.cfl = std::clamp(m_params.cfl, 0.05, 0.95);
    m_params.wallTemperature = clampFinite(
        m_params.wallTemperature,
        MinTemperature,
        MaxTemperature,
        units::celcius(120.0));
    m_params.wallHeatTransfer = std::max(0.0, m_params.wallHeatTransfer);
    m_params.wallFriction = std::max(0.0, m_params.wallFriction);
    m_params.outletReflection = std::clamp(m_params.outletReflection, 0.0, 0.98);
    m_params.outletReflectionCutoff = std::max(10.0, m_params.outletReflectionCutoff);

    m_cells.assign(m_params.cellCount, conservativeFromPrimitive({}));
    m_reflectedPressure = 0.0;
    m_outletAudioSignal = 0.0;
    m_lastValveFlow = 0.0;
}

void ExhaustPipe1D::destroy() {
    m_cells.clear();
    m_reflectedPressure = 0.0;
    m_outletAudioSignal = 0.0;
    m_lastValveFlow = 0.0;
}

double ExhaustPipe1D::process(
    double dt,
    GasSystem *cylinder,
    GasSystem *collector,
    double valveFlowRate,
    double outletFlowRate)
{
    if (m_cells.empty() || dt <= 0.0) {
        return 0.0;
    }

    const double valveFlow = exchangeWithCylinder(cylinder, valveFlowRate, dt);

    double remaining = dt;
    int guard = 0;
    while (remaining > 0.0 && guard++ < 64) {
        const double substep = std::min(remaining, std::max(1.0e-7, maxStableTimestep()));
        stepFiniteVolume(substep);
        applySourceTerms(substep);
        exchangeWithCollector(collector, outletFlowRate, substep);
        remaining -= substep;
    }

    const Primitive outlet = primitiveFromConservative(m_cells.back());
    m_outletAudioSignal =
        ((outlet.p - AtmosphericPressure)
            + 0.06 * outlet.rho * outlet.u * std::abs(outlet.u))
        * (400.0 / AtmosphericPressure);

    return valveFlow;
}

double ExhaustPipe1D::inletPressure() const {
    return m_cells.empty()
        ? AtmosphericPressure
        : primitiveFromConservative(m_cells.front()).p;
}

double ExhaustPipe1D::outletPressure() const {
    return m_cells.empty()
        ? AtmosphericPressure
        : primitiveFromConservative(m_cells.back()).p;
}

double ExhaustPipe1D::outletVelocity() const {
    return m_cells.empty()
        ? 0.0
        : primitiveFromConservative(m_cells.back()).u;
}

double ExhaustPipe1D::outletAudioSignal() const {
    return m_outletAudioSignal;
}

double ExhaustPipe1D::maxStableTimestep() const {
    if (m_cells.empty()) {
        return 1.0e-4;
    }

    double maxSpeed = 1.0;
    for (const Conservative &cell : m_cells) {
        const Primitive p = primitiveFromConservative(cell);
        const double c = std::sqrt(Gamma * p.p / p.rho);
        maxSpeed = std::max(maxSpeed, std::abs(p.u) + c);
    }

    return m_params.cfl * cellLength() / maxSpeed;
}

ExhaustPipe1D::Diagnostics ExhaustPipe1D::diagnostics() const {
    Diagnostics result;
    result.minPressure = std::numeric_limits<double>::max();
    result.maxPressure = std::numeric_limits<double>::lowest();
    result.minTemperature = std::numeric_limits<double>::max();
    result.maxTemperature = std::numeric_limits<double>::lowest();

    for (const Conservative &cell : m_cells) {
        const Primitive p = primitiveFromConservative(cell);
        const double T = temperatureOf(p);
        if (!std::isfinite(p.p) || !std::isfinite(T) || !std::isfinite(p.rho) || !std::isfinite(p.u)) {
            ++result.nanCount;
            continue;
        }

        result.minPressure = std::min(result.minPressure, p.p);
        result.maxPressure = std::max(result.maxPressure, p.p);
        result.minTemperature = std::min(result.minTemperature, T);
        result.maxTemperature = std::max(result.maxTemperature, T);
    }

    if (m_cells.empty()) {
        result.minPressure = result.maxPressure = AtmosphericPressure;
        result.minTemperature = result.maxTemperature = AmbientTemperature;
    }

    return result;
}

ExhaustPipe1D::Limiter ExhaustPipe1D::limiterFromName(const std::string &name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (char c : name) {
        if (c != '_' && c != '-' && c != ' ') {
            normalized.push_back(static_cast<char>(normalizedNameChar(c)));
        }
    }

    if (normalized == "minmod") {
        return Limiter::Minmod;
    }

    return Limiter::MC;
}

double ExhaustPipe1D::limitSlope(double leftSlope, double rightSlope, Limiter limiter) {
    if (leftSlope * rightSlope <= 0.0) {
        return 0.0;
    }

    const double sign = (leftSlope > 0.0) ? 1.0 : -1.0;
    const double absLeft = std::abs(leftSlope);
    const double absRight = std::abs(rightSlope);

    if (limiter == Limiter::Minmod) {
        return sign * std::min(absLeft, absRight);
    }

    const double centered = 0.5 * (absLeft + absRight);
    return sign * std::min({ 2.0 * absLeft, centered, 2.0 * absRight });
}

void ExhaustPipe1D::stepFiniteVolume(double dt) {
    const int n = static_cast<int>(m_cells.size());
    if (n == 0) {
        return;
    }

    std::vector<Primitive> primitive(n);
    for (int i = 0; i < n; ++i) {
        primitive[i] = primitiveFromConservative(m_cells[i]);
    }

    std::vector<Primitive> slope(n);
    for (int i = 1; i < n - 1; ++i) {
        slope[i].rho = limitSlope(
            primitive[i].rho - primitive[i - 1].rho,
            primitive[i + 1].rho - primitive[i].rho,
            m_params.limiter);
        slope[i].u = limitSlope(
            primitive[i].u - primitive[i - 1].u,
            primitive[i + 1].u - primitive[i].u,
            m_params.limiter);
        slope[i].p = limitSlope(
            primitive[i].p - primitive[i - 1].p,
            primitive[i + 1].p - primitive[i].p,
            m_params.limiter);
    }

    std::vector<Flux> fluxes(n + 1);
    Primitive inletGhost = primitive.front();
    inletGhost.u = std::max(0.0, inletGhost.u);
    fluxes[0] = rusanovFlux(inletGhost, primitive.front());

    for (int iface = 1; iface < n; ++iface) {
        Primitive left = primitive[iface - 1];
        Primitive right = primitive[iface];

        left.rho += 0.5 * slope[iface - 1].rho;
        left.u += 0.5 * slope[iface - 1].u;
        left.p += 0.5 * slope[iface - 1].p;

        right.rho -= 0.5 * slope[iface].rho;
        right.u -= 0.5 * slope[iface].u;
        right.p -= 0.5 * slope[iface].p;

        fluxes[iface] = rusanovFlux(left, right);
    }

    fluxes[n] = rusanovFlux(primitive.back(), reflectedOutletGhost(primitive.back(), dt));

    const double dx = cellLength();
    for (int i = 0; i < n; ++i) {
        m_cells[i].rho -= (dt / dx) * (fluxes[i + 1].mass - fluxes[i].mass);
        m_cells[i].momentum -= (dt / dx) * (fluxes[i + 1].momentum - fluxes[i].momentum);
        m_cells[i].energy -= (dt / dx) * (fluxes[i + 1].energy - fluxes[i].energy);
        sanitizeCell(&m_cells[i]);
    }
}

void ExhaustPipe1D::applySourceTerms(double dt) {
    const double frictionAlpha = 1.0 / (1.0 + m_params.wallFriction * dt);
    const double heatAlpha = 1.0 - std::exp(-m_params.wallHeatTransfer * dt);

    for (Conservative &cell : m_cells) {
        Primitive p = primitiveFromConservative(cell);
        p.u *= frictionAlpha;

        const double T = temperatureOf(p);
        const double nextT = T + (m_params.wallTemperature - T) * heatAlpha;
        p.p = p.rho * specificGasConstant() * clampFinite(nextT, MinTemperature, MaxTemperature, T);

        cell = conservativeFromPrimitive(p);
        sanitizeCell(&cell);
    }
}

double ExhaustPipe1D::exchangeWithCylinder(GasSystem *cylinder, double valveFlowRate, double dt) {
    if (cylinder == nullptr || valveFlowRate <= 0.0 || m_cells.empty()) {
        return 0.0;
    }

    Primitive inlet = primitiveFromConservative(m_cells.front());
    const double pipeTemperature = temperatureOf(inlet);
    double molPerSecond = GasSystem::flowRate(
        valveFlowRate,
        cylinder->pressure(),
        inlet.p,
        cylinder->temperature(),
        pipeTemperature,
        Gamma,
        GasSystem::chokedFlowLimit(5),
        GasSystem::chokedFlowRate(5));

    if (!std::isfinite(molPerSecond)) {
        molPerSecond = 0.0;
    }

    double dn = molPerSecond * dt;
    const double maxCylinderDn = 0.20 * cylinder->n();
    const double maxPipeDn = 0.20 * (m_cells.front().rho * cellVolume() / units::AirMolecularMass);
    dn = std::clamp(dn, -maxPipeDn, maxCylinderDn);

    if (dn > 0.0) {
        const double energyPerMol = cylinder->kineticEnergyPerMol();
        const GasSystem::Mix mix = cylinder->mix();
        cylinder->loseN(dn, energyPerMol);
        addMassToCell(0, dn, energyPerMol, mix, 1.0);
    }
    else if (dn < 0.0) {
        const double outDn = -dn;
        const double energyPerMol = inlet.p / std::max(1.0e-9, inlet.rho / units::AirMolecularMass)
            * (1.0 / (Gamma - 1.0));
        removeMassFromCell(0, outDn);
        cylinder->gainN(outDn, std::max(0.0, energyPerMol));
    }

    m_lastValveFlow = (dt > 0.0) ? dn / dt : 0.0;
    return m_lastValveFlow;
}

void ExhaustPipe1D::exchangeWithCollector(GasSystem *collector, double outletFlowRate, double dt) {
    if (collector == nullptr || outletFlowRate <= 0.0 || m_cells.empty()) {
        return;
    }

    const int index = static_cast<int>(m_cells.size()) - 1;
    Primitive outlet = primitiveFromConservative(m_cells[index]);
    const double pipeTemperature = temperatureOf(outlet);
    double molPerSecond = GasSystem::flowRate(
        outletFlowRate,
        outlet.p,
        collector->pressure(),
        pipeTemperature,
        collector->temperature(),
        Gamma,
        GasSystem::chokedFlowLimit(5),
        GasSystem::chokedFlowRate(5));

    if (!std::isfinite(molPerSecond)) {
        molPerSecond = 0.0;
    }

    double dn = molPerSecond * dt;
    const double maxPipeDn = 0.20 * (m_cells[index].rho * cellVolume() / units::AirMolecularMass);
    const double maxCollectorDn = 0.20 * collector->n();
    dn = std::clamp(dn, -maxCollectorDn, maxPipeDn);

    if (dn > 0.0) {
        const double energyPerMol = outlet.p / std::max(1.0e-9, outlet.rho / units::AirMolecularMass)
            * (1.0 / (Gamma - 1.0));
        removeMassFromCell(index, dn);
        collector->gainN(dn, std::max(0.0, energyPerMol));
    }
    else if (dn < 0.0) {
        const double outDn = -dn;
        const double energyPerMol = collector->kineticEnergyPerMol();
        const GasSystem::Mix mix = collector->mix();
        collector->loseN(outDn, energyPerMol);
        addMassToCell(index, outDn, energyPerMol, mix, -1.0);
    }
}

void ExhaustPipe1D::addMassToCell(
    int index,
    double dn,
    double energyPerMol,
    const GasSystem::Mix & /* mix */,
    double direction)
{
    if (dn <= 0.0 || index < 0 || index >= static_cast<int>(m_cells.size())) {
        return;
    }

    Conservative &cell = m_cells[index];
    const double addedMass = dn * units::AirMolecularMass / cellVolume();
    const double entryVelocity = direction * std::min(450.0, std::abs(m_lastValveFlow) * 20.0);

    cell.rho += addedMass;
    cell.momentum += addedMass * entryVelocity;
    cell.energy += (dn * energyPerMol / cellVolume())
        + 0.5 * addedMass * entryVelocity * entryVelocity;

    sanitizeCell(&cell);
}

void ExhaustPipe1D::removeMassFromCell(int index, double dn) {
    if (dn <= 0.0 || index < 0 || index >= static_cast<int>(m_cells.size())) {
        return;
    }

    Conservative &cell = m_cells[index];
    const double removedMass = std::min(cell.rho * 0.8, dn * units::AirMolecularMass / cellVolume());
    if (removedMass <= 0.0) {
        return;
    }

    const Primitive p = primitiveFromConservative(cell);
    const double specificEnergy = cell.energy / std::max(cell.rho, MinDensity);
    cell.rho -= removedMass;
    cell.momentum -= removedMass * p.u;
    cell.energy -= removedMass * specificEnergy;
    sanitizeCell(&cell);
}

void ExhaustPipe1D::sanitizeCell(Conservative *cell) const {
    Primitive p = primitiveFromConservative(*cell);
    p.rho = clampFinite(p.rho, MinDensity, MaxDensity, 1.2);
    p.u = clampFinite(p.u, -900.0, 900.0, 0.0);
    p.p = clampFinite(p.p, MinPressure, MaxPressure, AtmosphericPressure);
    *cell = conservativeFromPrimitive(p);
}

ExhaustPipe1D::Primitive ExhaustPipe1D::primitiveFromConservative(const Conservative &state) const {
    Primitive p;
    p.rho = clampFinite(state.rho, MinDensity, MaxDensity, 1.2);
    p.u = clampFinite(state.momentum / p.rho, -900.0, 900.0, 0.0);
    const double internalEnergy = state.energy - 0.5 * p.rho * p.u * p.u;
    p.p = clampFinite((Gamma - 1.0) * internalEnergy, MinPressure, MaxPressure, AtmosphericPressure);
    return p;
}

ExhaustPipe1D::Conservative ExhaustPipe1D::conservativeFromPrimitive(const Primitive &state) const {
    Primitive p = state;
    p.rho = clampFinite(p.rho, MinDensity, MaxDensity, 1.2);
    p.u = clampFinite(p.u, -900.0, 900.0, 0.0);
    p.p = clampFinite(p.p, MinPressure, MaxPressure, AtmosphericPressure);

    Conservative result;
    result.rho = p.rho;
    result.momentum = p.rho * p.u;
    result.energy = p.p / (Gamma - 1.0) + 0.5 * p.rho * p.u * p.u;
    return result;
}

ExhaustPipe1D::Flux ExhaustPipe1D::fluxFromPrimitive(const Primitive &state) const {
    const Conservative conservative = conservativeFromPrimitive(state);

    Flux flux;
    flux.mass = conservative.momentum;
    flux.momentum = conservative.momentum * state.u + state.p;
    flux.energy = (conservative.energy + state.p) * state.u;
    return flux;
}

ExhaustPipe1D::Flux ExhaustPipe1D::rusanovFlux(const Primitive &left, const Primitive &right) const {
    const Flux leftFlux = fluxFromPrimitive(left);
    const Flux rightFlux = fluxFromPrimitive(right);
    const Conservative leftU = conservativeFromPrimitive(left);
    const Conservative rightU = conservativeFromPrimitive(right);
    const double leftC = std::sqrt(Gamma * left.p / left.rho);
    const double rightC = std::sqrt(Gamma * right.p / right.rho);
    const double waveSpeed = std::max(std::abs(left.u) + leftC, std::abs(right.u) + rightC);

    Flux result;
    result.mass = 0.5 * (leftFlux.mass + rightFlux.mass)
        - 0.5 * waveSpeed * (rightU.rho - leftU.rho);
    result.momentum = 0.5 * (leftFlux.momentum + rightFlux.momentum)
        - 0.5 * waveSpeed * (rightU.momentum - leftU.momentum);
    result.energy = 0.5 * (leftFlux.energy + rightFlux.energy)
        - 0.5 * waveSpeed * (rightU.energy - leftU.energy);
    return result;
}

ExhaustPipe1D::Primitive ExhaustPipe1D::reflectedOutletGhost(const Primitive &last, double dt) {
    const double cutoff = m_params.outletReflectionCutoff;
    const double alpha = 1.0 - std::exp(-2.0 * constants::pi * cutoff * dt);
    const double pressureDelta = last.p - AtmosphericPressure;
    m_reflectedPressure += alpha * (pressureDelta - m_reflectedPressure);

    Primitive ghost = last;
    ghost.p = AtmosphericPressure + m_params.outletReflection * m_reflectedPressure;
    ghost.u = std::max(0.0, last.u) * (1.0 - 0.5 * m_params.outletReflection);
    ghost.rho = clampFinite(ghost.p / (specificGasConstant() * AmbientTemperature), MinDensity, MaxDensity, 1.2);
    return ghost;
}

double ExhaustPipe1D::cellVolume() const {
    return m_params.crossSectionArea * cellLength();
}

double ExhaustPipe1D::cellLength() const {
    return m_params.length / std::max(1, static_cast<int>(m_cells.size()));
}

double ExhaustPipe1D::specificGasConstant() const {
    return constants::R / units::AirMolecularMass;
}

double ExhaustPipe1D::temperatureOf(const Primitive &state) const {
    return clampFinite(
        state.p / (std::max(state.rho, MinDensity) * specificGasConstant()),
        MinTemperature,
        MaxTemperature,
        AmbientTemperature);
}
