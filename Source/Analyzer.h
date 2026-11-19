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
//   - a PAZ-style filter bank: steep band-pass filters (Chebyshev, 14th order) whose output
//     peak / mean-square is measured, like an analog RTA. Below 250 Hz the bands have constant
//     width (LF Res), above they are 1/7 octave (Q ~ 10). Multirate: low bands run at a
//     reduced sample rate, so they cost almost nothing.
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
    std::atomic<juce::uint32> hiddenMask { 0 };  // channels not shown: their bands are not computed

    // ---- spectrum output ----
    int  getLayoutVersion() const noexcept       { return layoutVersion.load(); }
    int  copyBandFrequencies (std::array<float, maxBands>& dest) const;
    // band mean-square over the last analysed chunk (a sine of amplitude A reads A^2 / 2)
    float getBandPower (int ch, int band) const noexcept { return bandPow[(size_t) (ch * maxBands + band)].load (std::memory_order_relaxed); }
    // highest instantaneous band power y^2 since the last call (a sine reads A^2), resets it
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
    void rebuildBands();
    void rebuildMeters();

    // ---------- FIFO ----------
    static constexpr int fifoSize = 1 << 16;
    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> fifoBuffer { maxChannels, fifoSize };

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> numChannels { 2 };
    std::atomic<bool> needsRebuild { true };

    // ---------- spectrum (steep multirate filter bank) ----------
    static constexpr int maxSections = 7;

    struct Section { double b0 = 1, a1 = 0, a2 = 0; };   // numerator b0 * (1 - z^-2)

    struct Band
    {
        float freq = 1000.0f;
        int level = 0;
        int numSections = 0;
        std::array<Section, maxSections> sec {};
        std::array<std::array<double, maxSections * 2>, maxChannels> state {};
        std::array<float, maxChannels> mx {};     // peak of y^2 in this chunk
        std::array<double, maxChannels> sum {};   // sum of y^2 in this chunk
        int count = 0;
    };

    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        inline float process (float x, float& s1, float& s2) const noexcept
        {
            const float y = b0 * x + s1;
            s1 = b1 * x - a1 * y + s2;
            s2 = b2 * x - a2 * y;
            return y;
        }
    };

    struct Level
    {
        double rate = 48000.0;
        std::array<Biquad, 4> lp;                                   // decimation filter (input side)
        std::array<std::array<float, 8>, maxChannels> lpState {};
        std::array<int, maxChannels> phase {};
        std::vector<int> bands;
    };

    std::vector<Band> bands;
    std::vector<Level> levels;
    int builtLfRes = -1;
    std::array<double, maxChannels> silentSamples {};

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
