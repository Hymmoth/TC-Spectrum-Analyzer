#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

// Analysis engine.
// The audio thread only copies samples into a lock-free FIFO (push()).
// A background thread drains it and runs:
//   - a PAZ-style band analysis: multi-resolution FFTs (Blackman-Harris), bins summed per band.
//     Below 250 Hz the bands have constant width (LF Res), above they are 1/7 octave (Q ~ 10).
//     Each band uses the shortest FFT that still separates it sharply from its neighbours.
//   - per-channel K-System meters (AES17 RMS, 600 ms window, 4x true peak)
// Results are published through atomics; the editor applies the ballistics at frame rate.
class Analyzer : private juce::Thread
{
public:
    static constexpr int maxChannels = 16;
    static constexpr int maxBands    = 80;

    Analyzer();
    ~Analyzer() override;

    void prepare (double sampleRate, int numChannels);
    void release();

    // Audio thread: never blocks, never allocates.
    void push (const juce::AudioBuffer<float>& buffer) noexcept;

    // ---- settings ----
    std::atomic<int>  lfResHz { 20 };
    std::atomic<bool> spectrumActive { false }; // true while the editor is open

    // ---- spectrum output ----
    int  getLayoutVersion() const noexcept       { return layoutVersion.load(); }
    int  copyBandFrequencies (std::array<float, maxBands>& dest) const;
    // latest band mean-square power (a sine of amplitude A reads A^2 / 2)
    float getBandPower (int ch, int band) const noexcept { return bandPow[(size_t) (ch * maxBands + band)].load (std::memory_order_relaxed); }
    // highest band power since the last call (resets it)
    float takeBandMax (int ch, int band) noexcept        { return bandMax[(size_t) (ch * maxBands + band)].exchange (0.0f, std::memory_order_relaxed); }

    // ---- meter output (dBFS) ----
    float getRmsDb (int ch) const noexcept      { return rmsDb[(size_t) ch].load (std::memory_order_relaxed); }
    float takeTruePeakDb (int ch) noexcept      { return tpSinceRead[(size_t) ch].exchange (-200.0f, std::memory_order_relaxed); }
    float getTruePeakMaxDb() const noexcept     { return tpMaxDb.load(); }
    float getRmsMaxDb() const noexcept          { return rmsMaxDb.load(); }
    void  resetMeterMax() noexcept              { resetMeterRequest.store (true); }

    int getNumChannels() const noexcept         { return numChannels.load(); }

private:
    void run() override;
    void processChunk (int start, int numSamples);
    void runSpectrum();
    void rebuildBands();
    void rebuildMeters();

    // ---------- FIFO ----------
    static constexpr int fifoSize = 1 << 16;
    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> fifoBuffer { maxChannels, fifoSize };

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> numChannels { 2 };
    std::atomic<bool> needsRebuild { true };

    // ---------- spectrum ----------
    static constexpr int maxOrder = 16;                 // 65536-point FFT at most
    static constexpr int historySize = 1 << maxOrder;
    juce::AudioBuffer<float> history { maxChannels, historySize * 2 }; // written twice -> contiguous reads
    int historyPos = 0;
    juce::int64 totalSamples = 0;
    std::array<juce::int64, maxChannels> lastSound {};  // sample index after the last non-silent sample

    struct Resolution
    {
        int order = 10, size = 1024, hop = 256;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::vector<float> window, buffer;
        float norm = 1.0f;          // converts summed |X|^2 to mean-square power
        juce::int64 lastRun = 0;
        std::vector<int> bands;
    };

    struct Band
    {
        float freq = 1000.0f;
        int resolution = 0;
        int binLo = 0, binHi = 0;
    };

    std::vector<Resolution> resolutions;
    std::vector<Band> bands;
    int builtLfRes = -1;

    std::array<std::atomic<float>, maxChannels * maxBands> bandPow {};
    std::array<std::atomic<float>, maxChannels * maxBands> bandMax {};
    std::array<float, maxBands> bandFreqs {};
    std::atomic<int> numBandsPublished { 0 };
    std::atomic<int> layoutVersion { 0 };
    mutable juce::SpinLock layoutLock;

    // ---------- meters ----------
    struct MeterState
    {
        std::array<float, 12> tpHistory {};
        double subSum = 0.0;
        int subCount = 0;
        std::vector<double> ring;
        int ringPos = 0;
        double ringSum = 0.0;
    };
    std::array<MeterState, maxChannels> meters;
    std::array<float, 48> tpCoeffs {};
    int subBlockLen = 480;
    int ringLen = 60;

    std::array<std::atomic<float>, maxChannels> rmsDb {};
    std::array<std::atomic<float>, maxChannels> tpSinceRead {};
    std::atomic<float> tpMaxDb { -200.0f };
    std::atomic<float> rmsMaxDb { -200.0f };
    std::atomic<bool> resetMeterRequest { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Analyzer)
};
