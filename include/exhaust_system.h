#ifndef ATG_ENGINE_SIM_EXHAUST_SYSTEM_H
#define ATG_ENGINE_SIM_EXHAUST_SYSTEM_H

#include "part.h"

#include "exhaust_pipe_1d.h"
#include "gas_system.h"
#include "impulse_response.h"

#include <string>

class ExhaustSystem : public Part {
    friend class Engine;

    public:
        struct Parameters {
            double length = 1.0;
            double collectorCrossSectionArea = 0.001;
            double outletFlowRate = 0.0;
            double primaryTubeLength = 0.0;
            double primaryFlowRate = 0.0;
            double velocityDecay = 1.0;
            double audioVolume = 1.0;
            double solverCellCount = 24.0;
            double solverCfl = 0.45;
            double wallTemperature = units::celcius(120.0);
            double wallHeatTransfer = 18.0;
            double wallFriction = 12.0;
            double outletReflection = 0.35;
            double outletReflectionCutoff = 1800.0;
            std::string solverLimiter = "mc";
            ImpulseResponse *impulseResponse = nullptr;
        };

    public:
        ExhaustSystem();
        virtual ~ExhaustSystem();

        void initialize(const Parameters &params);
        virtual void destroy();

        void process(double dt);

        inline int getIndex() const { return m_index; }
        inline double getLength() const { return m_length; }
        inline double getFlow() const { return m_flow; }
        inline double getAudioVolume() const { return m_audioVolume; }
        inline double getPrimaryFlowRate() const { return m_primaryFlowRate; }
        inline double getCollectorCrossSectionArea() const { return m_collectorCrossSectionArea; }
        inline double getPrimaryTubeLength() const { return m_primaryTubeLength; }
        inline double getVelocityDecay() const { return m_velocityDecay; }
        inline ImpulseResponse *getImpulseResponse() const { return m_impulseResponse; }
        inline const Parameters &getParameters() const { return m_parameters; }

        inline GasSystem *getSystem() { return &m_system; }
        ExhaustPipe1D::Parameters makePipeParameters(double primaryLength, double primaryArea) const;

    protected:
        GasSystem m_atmosphere;
        GasSystem m_system;

        ImpulseResponse *m_impulseResponse;
        Parameters m_parameters;

        double m_length;
        double m_primaryTubeLength;
        double m_collectorCrossSectionArea;
        double m_primaryFlowRate;
        double m_outletFlowRate;
        double m_audioVolume;
        double m_velocityDecay;
        int m_index;

        double m_flow;
};

#endif /* ATG_ENGINE_SIM_EXHAUST_SYSTEM_H */
