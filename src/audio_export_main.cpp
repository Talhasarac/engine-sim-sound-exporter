#include "../include/engine.h"
#include "../include/simulator.h"
#include "../include/transmission.h"
#include "../include/units.h"
#include "../include/vehicle.h"
#include "../scripting/include/compiler.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int AudioSampleRate = 44100;
constexpr double OfflineFrameSeconds = 0.01;
constexpr double LoopSeamSearchSeconds = 0.5;
constexpr int DefaultRpmAnchorSet[] = {
    800, 1000, 1250, 1500, 1750, 2000, 2500, 3000, 3500, 4000, 4750, 5500, 6250
};

struct ExportOptions {
    std::filesystem::path scriptPath;
    std::filesystem::path outputDirectory = "exports/unity_audio";
    double durationSeconds = 5.0;
    double warmupSeconds = 3.0;
    double loopCrossfadeSeconds = 0.02;
    std::vector<int> rpmTargets;
    std::vector<int> throttleTargets = { 30, 70, 100 };
    bool exportStartup = true;
    bool exportIgnitionOff = true;
};

struct LoadedSimulation {
    Engine *engine = nullptr;
    Vehicle *vehicle = nullptr;
    Transmission *transmission = nullptr;
    Simulator *simulator = nullptr;
};

struct ClipManifestRow {
    std::string filename;
    std::string type;
    int rpm = 0;
    int throttlePercent = -1;
    bool loop = false;
    double durationSeconds = 0.0;
};

struct PcmWave {
    int sampleRate = 0;
    int channels = 0;
    std::vector<int16_t> samples;
};

struct ClipAudioDiagnostics {
    int peak = 0;
    int clippingSamples = 0;
    double rms = 0.0;
    double spectralCentroidHz = 0.0;
};

void printUsage() {
    std::cout
        << "Usage: engine-sim-exporter [options]\n"
        << "\n"
        << "Options:\n"
        << "  --script <path>       Engine script to load. Defaults to assets/main.mr or ../assets/main.mr.\n"
        << "  --out <directory>     Output directory. Default: exports/unity_audio\n"
        << "  --duration <seconds>  Clip length. Default: 5\n"
        << "  --warmup <seconds>    Warmup before steady RPM captures. Default: 3\n"
        << "  --rpm <list>          Comma-separated RPM targets. When omitted, defaults to\n"
        << "                        800,1000,1250,1500,1750,2000,2500,3000,3500,4000,\n"
        << "                        4750,5500,6250.\n"
        << "  --throttle <list>     Comma-separated throttle percentages. Default: 30,70,100\n"
        << "  --no-startup          Skip startup_5s.wav.\n"
        << "  --no-ignition-off     Skip ignition_off_5s.wav.\n"
        << "  --loop-crossfade <ms> Crossfade from extra rendered audio into loop head. Default: 20\n"
        << "  --help                Show this help.\n";
}

double parseDouble(const std::string &value, const char *name) {
    char *end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) {
        throw std::runtime_error(std::string("Invalid value for ") + name + ": " + value);
    }

    return parsed;
}

std::vector<int> parseRpmList(const std::string &value) {
    std::vector<int> result;
    std::stringstream stream(value);
    std::string item;

    while (std::getline(stream, item, ',')) {
        if (item.empty()) {
            continue;
        }

        const int rpm = static_cast<int>(std::lround(parseDouble(item, "--rpm")));
        if (rpm <= 0) {
            throw std::runtime_error("RPM targets must be greater than zero.");
        }

        result.push_back(rpm);
    }

    if (result.empty()) {
        throw std::runtime_error("--rpm did not contain any RPM targets.");
    }

    return result;
}

std::vector<int> parseThrottleList(const std::string &value) {
    std::vector<int> result;
    std::stringstream stream(value);
    std::string item;

    while (std::getline(stream, item, ',')) {
        if (item.empty()) {
            continue;
        }

        const int throttle = static_cast<int>(std::lround(parseDouble(item, "--throttle")));
        if (throttle < 0 || throttle > 100) {
            throw std::runtime_error("Throttle targets must be between 0 and 100.");
        }

        result.push_back(throttle);
    }

    if (result.empty()) {
        throw std::runtime_error("--throttle did not contain any throttle targets.");
    }

    return result;
}

ExportOptions parseArguments(int argc, char **argv) {
    ExportOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto requireValue = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("Missing value for ") + name);
            }

            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage();
            std::exit(0);
        }
        else if (arg == "--script") {
            options.scriptPath = requireValue("--script");
        }
        else if (arg == "--out") {
            options.outputDirectory = requireValue("--out");
        }
        else if (arg == "--duration") {
            options.durationSeconds = parseDouble(requireValue("--duration"), "--duration");
        }
        else if (arg == "--warmup") {
            options.warmupSeconds = parseDouble(requireValue("--warmup"), "--warmup");
        }
        else if (arg == "--rpm") {
            options.rpmTargets = parseRpmList(requireValue("--rpm"));
        }
        else if (arg == "--throttle") {
            options.throttleTargets = parseThrottleList(requireValue("--throttle"));
        }
        else if (arg == "--no-startup") {
            options.exportStartup = false;
        }
        else if (arg == "--no-ignition-off") {
            options.exportIgnitionOff = false;
        }
        else if (arg == "--loop-crossfade") {
            options.loopCrossfadeSeconds =
                parseDouble(requireValue("--loop-crossfade"), "--loop-crossfade") / 1000.0;
        }
        else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (options.durationSeconds <= 0.0) {
        throw std::runtime_error("--duration must be greater than zero.");
    }

    if (options.warmupSeconds < 0.0) {
        throw std::runtime_error("--warmup cannot be negative.");
    }

    if (options.loopCrossfadeSeconds < 0.0) {
        throw std::runtime_error("--loop-crossfade cannot be negative.");
    }

    if (options.scriptPath.empty()) {
        const std::filesystem::path candidates[] = {
            "assets/main.mr",
            "../assets/main.mr",
            "../../assets/main.mr",
            "../../../assets/main.mr",
        };

        for (const std::filesystem::path &candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                options.scriptPath = candidate;
                break;
            }
        }

        if (options.scriptPath.empty()) {
            options.scriptPath = "assets/main.mr";
        }
    }

    return options;
}

std::string durationTag(double seconds) {
    const int rounded = static_cast<int>(std::lround(seconds));
    if (std::abs(seconds - rounded) < 0.001) {
        return std::to_string(rounded) + "s";
    }

    std::ostringstream stream;
    stream << seconds;
    std::string tag = stream.str();
    std::replace(tag.begin(), tag.end(), '.', 'p');
    return tag + "s";
}

std::string sanitizeName(const std::string &name) {
    std::string result;
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc)) {
            result.push_back(static_cast<char>(std::tolower(uc)));
        }
        else if (!result.empty() && result.back() != '_') {
            result.push_back('_');
        }
    }

    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }

    return result.empty() ? "engine" : result;
}

void writeU16(std::ofstream &file, std::uint16_t value) {
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
}

void writeU32(std::ofstream &file, std::uint32_t value) {
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
    file.put(static_cast<char>((value >> 16) & 0xff));
    file.put(static_cast<char>((value >> 24) & 0xff));
}

int16_t clampInt16(double value) {
    const long rounded = std::lround(value);
    return static_cast<int16_t>(
        std::clamp<long>(rounded, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()));
}

double smoothstep(double t) {
    return t * t * (3.0 - 2.0 * t);
}

std::uint16_t readU16(std::ifstream &file) {
    unsigned char bytes[2] = {};
    file.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
    if (!file) {
        throw std::runtime_error("Unexpected end of WAV file.");
    }

    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

std::uint32_t readU32(std::ifstream &file) {
    unsigned char bytes[4] = {};
    file.read(reinterpret_cast<char *>(bytes), sizeof(bytes));
    if (!file) {
        throw std::runtime_error("Unexpected end of WAV file.");
    }

    return
        static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string readFourCc(std::ifstream &file) {
    char id[4] = {};
    file.read(id, sizeof(id));
    if (!file) {
        throw std::runtime_error("Unexpected end of WAV file.");
    }

    return std::string(id, id + 4);
}

PcmWave readPcm16Wave(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open impulse response: " + path.string());
    }

    if (readFourCc(file) != "RIFF") {
        throw std::runtime_error("Invalid WAV RIFF header: " + path.string());
    }

    readU32(file);
    if (readFourCc(file) != "WAVE") {
        throw std::runtime_error("Invalid WAV WAVE header: " + path.string());
    }

    PcmWave wave;
    std::uint16_t audioFormat = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t blockAlign = 0;
    std::vector<char> data;

    while (file.peek() != EOF) {
        const std::string chunkId = readFourCc(file);
        const std::uint32_t chunkSize = readU32(file);
        const std::streampos chunkDataStart = file.tellg();

        if (chunkId == "fmt ") {
            audioFormat = readU16(file);
            wave.channels = readU16(file);
            wave.sampleRate = static_cast<int>(readU32(file));
            readU32(file);
            blockAlign = readU16(file);
            bitsPerSample = readU16(file);
        }
        else if (chunkId == "data") {
            data.resize(chunkSize);
            file.read(data.data(), chunkSize);
            if (!file) {
                throw std::runtime_error("Could not read WAV data: " + path.string());
            }
        }

        file.seekg(chunkDataStart + static_cast<std::streamoff>(chunkSize + (chunkSize & 1)));
    }

    if (audioFormat != 1 || bitsPerSample != 16 || wave.channels <= 0 || blockAlign == 0) {
        throw std::runtime_error("Only PCM 16-bit WAV impulse responses are supported: " + path.string());
    }

    const int frameCount = static_cast<int>(data.size() / blockAlign);
    wave.samples.reserve(frameCount);
    for (int i = 0; i < frameCount; ++i) {
        const int offset = i * blockAlign;
        const unsigned char lo = static_cast<unsigned char>(data[offset]);
        const unsigned char hi = static_cast<unsigned char>(data[offset + 1]);
        wave.samples.push_back(static_cast<int16_t>(lo | (hi << 8)));
    }

    return wave;
}

void writeWav(const std::filesystem::path &path, const std::vector<int16_t> &samples) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open output file: " + path.string());
    }

    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(samples.size() * sizeof(int16_t));
    const std::uint16_t channels = 1;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t byteRate = AudioSampleRate * channels * bitsPerSample / 8;
    const std::uint16_t blockAlign = channels * bitsPerSample / 8;

    file.write("RIFF", 4);
    writeU32(file, 36 + dataBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    writeU32(file, 16);
    writeU16(file, 1);
    writeU16(file, channels);
    writeU32(file, AudioSampleRate);
    writeU32(file, byteRate);
    writeU16(file, blockAlign);
    writeU16(file, bitsPerSample);
    file.write("data", 4);
    writeU32(file, dataBytes);
    file.write(reinterpret_cast<const char *>(samples.data()), dataBytes);
}

void writeManifest(
    const std::filesystem::path &path,
    const std::vector<ClipManifestRow> &rows)
{
    std::ofstream file(path);
    if (!file) {
        throw std::runtime_error("Could not write manifest: " + path.string());
    }

    file << "filename,type,rpm,throttle_percent,loop,duration_seconds\n";
    for (const ClipManifestRow &row : rows) {
        file
            << row.filename << ','
            << row.type << ','
            << row.rpm << ','
            << ((row.throttlePercent >= 0) ? std::to_string(row.throttlePercent) : "") << ','
            << (row.loop ? "true" : "false") << ','
            << row.durationSeconds << '\n';
    }
}

ClipAudioDiagnostics analyzeClipAudio(const std::vector<int16_t> &samples) {
    ClipAudioDiagnostics diagnostics;
    if (samples.empty()) {
        return diagnostics;
    }

    double sumSquares = 0.0;
    for (const int16_t sample : samples) {
        const int absSample = std::abs(static_cast<int>(sample));
        diagnostics.peak = std::max(diagnostics.peak, absSample);
        if (absSample >= INT16_MAX - 1) {
            ++diagnostics.clippingSamples;
        }

        sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
    }
    diagnostics.rms = std::sqrt(sumSquares / samples.size());

    constexpr int WindowSize = 2048;
    const int n = std::min(WindowSize, static_cast<int>(samples.size()));
    if (n > 32) {
        double weightedMagnitude = 0.0;
        double totalMagnitude = 0.0;
        const int maxBin = n / 2;
        for (int bin = 1; bin <= maxBin; ++bin) {
            double real = 0.0;
            double imag = 0.0;
            for (int i = 0; i < n; ++i) {
                const double window = 0.5 - 0.5 * std::cos((2.0 * constants::pi * i) / (n - 1));
                const double phase = (2.0 * constants::pi * bin * i) / n;
                const double sample = window * samples[i];
                real += sample * std::cos(phase);
                imag -= sample * std::sin(phase);
            }

            const double magnitude = std::sqrt(real * real + imag * imag);
            const double frequency = (AudioSampleRate * bin) / static_cast<double>(n);
            weightedMagnitude += frequency * magnitude;
            totalMagnitude += magnitude;
        }

        if (totalMagnitude > 0.0) {
            diagnostics.spectralCentroidHz = weightedMagnitude / totalMagnitude;
        }
    }

    return diagnostics;
}

void addFallbackVehicleAndTransmission(LoadedSimulation *loaded) {
    if (loaded->vehicle == nullptr) {
        Vehicle::Parameters vehParams;
        vehParams.mass = units::mass(1597, units::kg);
        vehParams.diffRatio = 3.42;
        vehParams.tireRadius = units::distance(10, units::inch);
        vehParams.dragCoefficient = 0.25;
        vehParams.crossSectionArea =
            units::distance(6.0, units::foot) * units::distance(6.0, units::foot);
        vehParams.rollingResistance = 2000.0;

        loaded->vehicle = new Vehicle;
        loaded->vehicle->initialize(vehParams);
    }

    if (loaded->transmission == nullptr) {
        const double gearRatios[] = { 2.97, 2.07, 1.43, 1.00, 0.84, 0.56 };
        Transmission::Parameters tParams;
        tParams.GearCount = 6;
        tParams.GearRatios = gearRatios;
        tParams.MaxClutchTorque = units::torque(1000.0, units::ft_lb);

        loaded->transmission = new Transmission;
        loaded->transmission->initialize(tParams);
    }
}

void initializeImpulseResponses(LoadedSimulation *loaded) {
    for (int i = 0; i < loaded->engine->getExhaustSystemCount(); ++i) {
        ImpulseResponse *response = loaded->engine->getExhaustSystem(i)->getImpulseResponse();
        if (response == nullptr) {
            continue;
        }

        const PcmWave wave = readPcm16Wave(response->getFilename());

        loaded->simulator->synthesizer().initializeImpulseResponse(
            wave.samples.data(),
            static_cast<unsigned int>(wave.samples.size()),
            static_cast<float>(response->getVolume()),
            i);
    }
}

LoadedSimulation loadSimulation(const std::filesystem::path &scriptPath) {
    LoadedSimulation loaded;

    es_script::Compiler compiler;
    compiler.initialize();
    *es_script::Compiler::output() = es_script::Compiler::Output();

    const bool compiled = compiler.compile(scriptPath.string().c_str());
    if (!compiled) {
        compiler.destroy();
        throw std::runtime_error(
            "Could not compile script: " + scriptPath.string() + " (see error_log.log)");
    }

    const es_script::Compiler::Output output = compiler.execute();
    compiler.destroy();

    loaded.engine = output.engine;
    loaded.vehicle = output.vehicle;
    loaded.transmission = output.transmission;

    if (loaded.engine == nullptr) {
        throw std::runtime_error("Script did not create an engine.");
    }

    addFallbackVehicleAndTransmission(&loaded);

    loaded.simulator = loaded.engine->createSimulator(loaded.vehicle, loaded.transmission);
    loaded.engine->calculateDisplacement();
    loaded.simulator->setSimulationFrequency(
        static_cast<int>(loaded.engine->getSimulationFrequency()));

    Synthesizer::AudioParameters audioParams =
        loaded.simulator->synthesizer().getAudioParameters();
    audioParams.inputSampleNoise = static_cast<float>(loaded.engine->getInitialJitter());
    audioParams.airNoise = static_cast<float>(loaded.engine->getInitialNoise());
    audioParams.dF_F_mix = static_cast<float>(loaded.engine->getInitialHighFrequencyGain());
    loaded.simulator->synthesizer().setAudioParameters(audioParams);

    initializeImpulseResponses(&loaded);

    return loaded;
}

void destroySimulation(LoadedSimulation *loaded) {
    if (loaded->simulator != nullptr) {
        loaded->simulator->endAudioRenderingThread();
        loaded->simulator->destroy();
        delete loaded->simulator;
        loaded->simulator = nullptr;
    }

    if (loaded->engine != nullptr) {
        loaded->engine->destroy();
        delete loaded->engine;
        loaded->engine = nullptr;
    }

    delete loaded->transmission;
    delete loaded->vehicle;
    loaded->transmission = nullptr;
    loaded->vehicle = nullptr;
}

double speedControlForRpm(const Engine &engine, int rpm) {
    const double redline = std::max(units::toRpm(engine.getRedline()), 1000.0);
    const double normalized = rpm / redline;
    return std::clamp(normalized, 0.02, 1.0);
}

void setRunningState(LoadedSimulation *loaded, double speedControl, bool starter, bool dyno, int rpm) {
    loaded->engine->setSpeedControl(std::clamp(speedControl, 0.0, 1.0));
    loaded->engine->getIgnitionModule()->m_enabled = true;

    loaded->simulator->m_starterMotor.m_enabled = starter;
    loaded->simulator->m_dyno.m_enabled = dyno;
    loaded->simulator->m_dyno.m_hold = dyno;
    if (rpm > 0) {
        loaded->simulator->m_dyno.m_rotationSpeed = units::rpm(rpm);
    }
}

void setIgnitionOffState(LoadedSimulation *loaded) {
    loaded->engine->setSpeedControl(0.0);
    loaded->engine->getIgnitionModule()->m_enabled = false;
    loaded->simulator->m_starterMotor.m_enabled = false;
    loaded->simulator->m_dyno.m_enabled = false;
    loaded->simulator->m_dyno.m_hold = false;
}

int queuedAudioSamples(Simulator *simulator) {
    return static_cast<int>(simulator->synthesizer().m_audioBuffer.size());
}

void drainAudio(Simulator *simulator, std::vector<int16_t> *target, int maxSamples) {
    std::vector<int16_t> temp(4096);

    while (queuedAudioSamples(simulator) > 0 && maxSamples != 0) {
        int samples = std::min<int>(queuedAudioSamples(simulator), static_cast<int>(temp.size()));
        if (maxSamples > 0) {
            samples = std::min(samples, maxSamples);
        }

        const int read = simulator->readAudioOutput(samples, temp.data());
        if (read <= 0) {
            break;
        }

        if (target != nullptr) {
            target->insert(target->end(), temp.begin(), temp.begin() + read);
        }

        if (maxSamples > 0) {
            maxSamples -= read;
        }
    }
}

void advanceSimulation(
    LoadedSimulation *loaded,
    double seconds,
    std::vector<int16_t> *recorded,
    int targetRecordedSamples)
{
    double elapsed = 0.0;
    while (elapsed < seconds
        || (recorded != nullptr && static_cast<int>(recorded->size()) < targetRecordedSamples))
    {
        const double frameSeconds =
            (elapsed < seconds)
                ? std::min(OfflineFrameSeconds, seconds - elapsed)
                : OfflineFrameSeconds;

        drainAudio(loaded->simulator, nullptr, -1);

        loaded->simulator->startFrame(frameSeconds);
        while (loaded->simulator->simulateStep()) {
            // Run all physics iterations for this offline frame.
        }
        loaded->simulator->endFrame();

        while (loaded->simulator->synthesizer().renderAudioNonblocking()) {
            // Consume all synthesizer input generated by this frame.
        }

        if (recorded != nullptr) {
            const int remaining = targetRecordedSamples - static_cast<int>(recorded->size());
            drainAudio(loaded->simulator, recorded, remaining);
        }
        else {
            drainAudio(loaded->simulator, nullptr, -1);
        }

        elapsed += frameSeconds;
    }
}

std::vector<int16_t> captureSeconds(
    LoadedSimulation *loaded,
    double seconds,
    bool loop,
    double loopCrossfadeSeconds)
{
    const int requestedSamples = static_cast<int>(std::lround(seconds * AudioSampleRate));
    const int extraSamples = loop
        ? std::min(
            requestedSamples / 2,
            static_cast<int>(std::lround(loopCrossfadeSeconds * AudioSampleRate)))
        : 0;
    const int seamSearchSamples = loop
        ? std::min(
            requestedSamples / 2,
            static_cast<int>(std::lround(LoopSeamSearchSeconds * AudioSampleRate)))
        : 0;
    const int renderedExtraSamples = extraSamples + seamSearchSamples;
    const double extraSeconds = renderedExtraSamples / static_cast<double>(AudioSampleRate);

    std::vector<int16_t> recorded;
    recorded.reserve(requestedSamples + renderedExtraSamples);
    advanceSimulation(loaded, seconds + extraSeconds, &recorded, requestedSamples + renderedExtraSamples);

    if (static_cast<int>(recorded.size()) < requestedSamples + renderedExtraSamples) {
        recorded.resize(requestedSamples + renderedExtraSamples, 0);
    }

    if (loop && seamSearchSamples > 0) {
        int bestOffset = 0;
        int bestScore = std::numeric_limits<int>::max();
        for (int offset = 0; offset <= seamSearchSamples; ++offset) {
            const int score = std::abs(
                static_cast<int>(recorded[offset + requestedSamples])
                - static_cast<int>(recorded[offset + requestedSamples - 1]));
            if (score < bestScore) {
                bestScore = score;
                bestOffset = offset;
            }
        }

        std::vector<int16_t> shifted;
        shifted.reserve(requestedSamples + extraSamples);
        shifted.insert(
            shifted.end(),
            recorded.begin() + bestOffset,
            recorded.begin() + bestOffset + requestedSamples + extraSamples);
        recorded.swap(shifted);
    }

    if (loop && extraSamples > 1 && static_cast<int>(recorded.size()) >= requestedSamples + extraSamples) {
        // The rendered tail is the natural continuation after the selected clip end.
        // Blending it into the head makes the DAW/Unity wrap point follow the same waveform.
        for (int i = 0; i < extraSamples; ++i) {
            const double t = smoothstep(i / static_cast<double>(extraSamples - 1));
            const double head = recorded[i];
            const double wrap = recorded[requestedSamples + i];
            recorded[i] = clampInt16(wrap * (1.0 - t) + head * t);
        }
    }

    recorded.resize(requestedSamples);
    return recorded;
}

void prepareSteadyRpm(
    LoadedSimulation *loaded,
    int rpm,
    double warmupSeconds,
    double speedControl)
{
    setRunningState(loaded, 0.2, true, false, 0);
    advanceSimulation(loaded, 1.2, nullptr, 0);

    setRunningState(loaded, speedControl, false, true, rpm);
    advanceSimulation(loaded, warmupSeconds, nullptr, 0);
}

void exportClip(
    const std::filesystem::path &path,
    const std::vector<int16_t> &samples,
    std::vector<ClipManifestRow> *manifest,
    const std::string &type,
    int rpm,
    int throttlePercent,
    bool loop,
    double durationSeconds)
{
    writeWav(path, samples);
    const ClipAudioDiagnostics diagnostics = analyzeClipAudio(samples);

    manifest->push_back({
        path.filename().string(),
        type,
        rpm,
        throttlePercent,
        loop,
        durationSeconds,
    });

    std::cout
        << "Wrote " << path.string()
        << " | peak=" << diagnostics.peak
        << " rms=" << static_cast<int>(std::lround(diagnostics.rms))
        << " clip_samples=" << diagnostics.clippingSamples
        << " centroid_hz=" << static_cast<int>(std::lround(diagnostics.spectralCentroidHz))
        << '\n';
}

std::vector<int> defaultRpmTargets(const Engine & /* engine */) {
    return std::vector<int>(std::begin(DefaultRpmAnchorSet), std::end(DefaultRpmAnchorSet));
}

void exportStartup(
    const ExportOptions &options,
    const std::string &baseName,
    std::vector<ClipManifestRow> *manifest)
{
    LoadedSimulation loaded = loadSimulation(options.scriptPath);

    setRunningState(&loaded, 0.12, true, false, 0);
    std::vector<int16_t> samples;
    const int requestedSamples = static_cast<int>(std::lround(options.durationSeconds * AudioSampleRate));
    samples.reserve(requestedSamples);

    const double starterSeconds = std::min(1.4, options.durationSeconds);
    const int starterSamples =
        std::min(requestedSamples, static_cast<int>(std::lround(starterSeconds * AudioSampleRate)));
    advanceSimulation(&loaded, starterSeconds, &samples, starterSamples);

    if (static_cast<int>(samples.size()) < requestedSamples) {
        setRunningState(&loaded, 0.04, false, false, 0);
        advanceSimulation(
            &loaded,
            options.durationSeconds - starterSeconds,
            &samples,
            requestedSamples);
    }

    samples.resize(requestedSamples);

    exportClip(
        options.outputDirectory / (baseName + "_startup_" + durationTag(options.durationSeconds) + ".wav"),
        samples,
        manifest,
        "startup",
        0,
        -1,
        false,
        options.durationSeconds);

    destroySimulation(&loaded);
}

void exportIgnitionOff(
    const ExportOptions &options,
    const std::string &baseName,
    std::vector<ClipManifestRow> *manifest)
{
    LoadedSimulation loaded = loadSimulation(options.scriptPath);
    const int minRpm =
        std::max(1000, static_cast<int>(std::round(units::toRpm(loaded.engine->getDynoMinSpeed()))));
    const int maxRpm =
        std::max(minRpm, static_cast<int>(std::round(units::toRpm(loaded.engine->getRedline()) * 0.8)));
    const int rpm = std::clamp(1000, minRpm, maxRpm);

    prepareSteadyRpm(&loaded, rpm, options.warmupSeconds, speedControlForRpm(*loaded.engine, rpm));
    setIgnitionOffState(&loaded);

    std::vector<int16_t> samples =
        captureSeconds(&loaded, options.durationSeconds, false, 0.0);

    exportClip(
        options.outputDirectory / (baseName + "_ignition_off_" + durationTag(options.durationSeconds) + ".wav"),
        samples,
        manifest,
        "ignition_off",
        rpm,
        -1,
        false,
        options.durationSeconds);

    destroySimulation(&loaded);
}

void exportRpmLoop(
    const ExportOptions &options,
    const std::string &baseName,
    int rpm,
    int throttlePercent,
    std::vector<ClipManifestRow> *manifest)
{
    LoadedSimulation loaded = loadSimulation(options.scriptPath);

    const double speedControl = throttlePercent / 100.0;
    prepareSteadyRpm(&loaded, rpm, options.warmupSeconds, speedControl);

    std::vector<int16_t> samples =
        captureSeconds(&loaded, options.durationSeconds, true, options.loopCrossfadeSeconds);

    exportClip(
        options.outputDirectory / (
            baseName + "_rpm_" + std::to_string(rpm)
            + "_throttle_" + std::to_string(throttlePercent)
            + "_loop_"
            + durationTag(options.durationSeconds) + ".wav"),
        samples,
        manifest,
        "rpm_loop",
        rpm,
        throttlePercent,
        true,
        options.durationSeconds);

    destroySimulation(&loaded);
}

void cleanupGeneratedOutput(const std::filesystem::path &outputDirectory) {
    if (!std::filesystem::exists(outputDirectory)) {
        return;
    }

    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(outputDirectory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::filesystem::path path = entry.path();
        if (path.extension() == ".wav" || path.filename() == "unity_audio_manifest.csv") {
            std::filesystem::remove(path);
        }
    }
}

void runExporter(const ExportOptions &options) {
    std::filesystem::create_directories(options.outputDirectory);
    cleanupGeneratedOutput(options.outputDirectory);

    LoadedSimulation probe = loadSimulation(options.scriptPath);
    const std::string baseName = sanitizeName(probe.engine->getName());
    std::vector<int> rpmTargets = options.rpmTargets;
    if (rpmTargets.empty()) {
        rpmTargets = defaultRpmTargets(*probe.engine);
    }

    std::cout << "Loaded engine: " << probe.engine->getName() << '\n';
    destroySimulation(&probe);

    std::vector<ClipManifestRow> manifest;

    if (options.exportStartup) {
        exportStartup(options, baseName, &manifest);
    }

    for (int rpm : rpmTargets) {
        for (int throttlePercent : options.throttleTargets) {
            exportRpmLoop(options, baseName, rpm, throttlePercent, &manifest);
        }
    }

    if (options.exportIgnitionOff) {
        exportIgnitionOff(options, baseName, &manifest);
    }

    writeManifest(options.outputDirectory / "unity_audio_manifest.csv", manifest);
}

} // namespace

int main(int argc, char **argv) {
    try {
        const ExportOptions options = parseArguments(argc, argv);
        runExporter(options);
        return 0;
    }
    catch (const std::exception &e) {
        std::cerr << "Export failed: " << e.what() << '\n';
        return 1;
    }
}
