#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <vector>

// Analysis engine.
// The audio thread only copies samples into a lock-free FIFO (push()).
// A background thread drains it and runs:
//   - a PAZ-style constant-Q filter bank (multirate, so low bands are cheap)
//   - per-channel K-System meters (AES17 RMS, 600 ms window, 4x true peak)
// Results are published through atomics that the editor reads at frame rate.
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

    // ---- settings (written by the processor / editor) ----
    std::atomic<int>   detectRms  { 0 };      // 0 = Peak, 1 = RMS
    std::atomic<float> responseMs { 160.0f };
    std::atomic<int>   lfResHz    { 20 };
    std::atomic<bool>  spectrumActive { false }; // true while the editor is open

    // ---- spectrum output ----
    int  getNumBands() const noexcept            { return numBandsPublished.load(); }
    int  getLayoutVersion() const noexcept       { return layoutVersion.load(); }
    // copies the band centre frequencies, returns the band count
    int  copyBandFrequencies (std::array<float, maxBands>& dest) const;
    // current smoothed power (linear, 1.0 = full-scale sine peak)
    float getBandPower (int ch, int band) const noexcept { return bandPow[(size_t) (ch * maxBands + band)].load (std::memory_order_relaxed); }
    // highest power since the last call (resets it)
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
    void rebuildFilterBank();
    void rebuildMeters();

    // ---------- FIFO ----------
    static constexpr int fifoSize = 1 << 16;
    juce::AbstractFifo fifo { fifoSize };
    juce::AudioBuffer<float> fifoBuffer { maxChannels, fifoSize };

    std::atomic<double> sampleRate { 48000.0 };
    std::atomic<int> numChannels { 2 };
    std::atomic<bool> needsRebuild { true };

    // ---------- filter bank ----------
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

    struct Band
    {
        float freq = 1000.0f;
        int level = 0;
        Biquad bp;
        std::array<std::array<float, 4>, maxChannels> state {};
        std::array<float, maxChannels> env {};
        std::array<float, maxChannels> mx {};
        float coeff = 0.0f; // envelope coefficient at this band's sample rate
    };

    struct Level
    {
        double rate = 48000.0;
        std::array<Biquad, 3> lp;                                  // decimation filter (input side)
        std::array<std::array<float, 6>, maxChannels> lpState {};
        std::array<int, maxChannels> phase {};
        std::vector<int> bands;
    };

    std::vector<Band> bands;
    std::vector<Level> levels;
    int builtLfRes = -1;
    float builtResponse = -1.0f;
    int builtDetect = -1;
    void updateEnvelopeCoeffs();

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
        int tpPos = 0;
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
