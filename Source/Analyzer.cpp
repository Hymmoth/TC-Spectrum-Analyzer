#include "Analyzer.h"
#include <cmath>

namespace
{
    constexpr double pi = 3.14159265358979323846;

    // RBJ band-pass, 0 dB peak gain
    void designBandPass (float& b0, float& b1, float& b2, float& a1, float& a2, double f, double q, double fs)
    {
        const double w0 = 2.0 * pi * f / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (float) (alpha / a0);
        b1 = 0.0f;
        b2 = (float) (-alpha / a0);
        a1 = (float) (-2.0 * std::cos (w0) / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    // RBJ low-pass
    void designLowPass (float& b0, float& b1, float& b2, float& a1, float& a2, double f, double q, double fs)
    {
        const double w0 = 2.0 * pi * f / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c = std::cos (w0);
        const double a0 = 1.0 + alpha;
        b0 = (float) ((1.0 - c) * 0.5 / a0);
        b1 = (float) ((1.0 - c) / a0);
        b2 = b0;
        a1 = (float) (-2.0 * c / a0);
        a2 = (float) ((1.0 - alpha) / a0);
    }

    double besselI0 (double x)
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 30; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    inline void atomicMax (std::atomic<float>& a, float v) noexcept
    {
        float cur = a.load (std::memory_order_relaxed);
        while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
    }

    inline float toDb (double power) noexcept
    {
        return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : -200.0f;
    }
}

Analyzer::Analyzer() : juce::Thread ("TC Spectrum Analyzer")
{
    for (auto& v : bandPow) v.store (0.0f);
    for (auto& v : bandMax) v.store (0.0f);
    for (auto& v : rmsDb) v.store (-200.0f);
    for (auto& v : tpSinceRead) v.store (-200.0f);
}

Analyzer::~Analyzer()
{
    stopThread (2000);
}

void Analyzer::prepare (double sr, int numCh)
{
    stopThread (2000);
    sampleRate.store (sr);
    numChannels.store (juce::jlimit (1, maxChannels, numCh));
    fifo.reset();
    needsRebuild.store (true);
    startThread (juce::Thread::Priority::low);
}

void Analyzer::release()
{
    stopThread (2000);
}

void Analyzer::push (const juce::AudioBuffer<float>& buffer) noexcept
{
    const int numCh = juce::jmin (buffer.getNumChannels(), numChannels.load (std::memory_order_relaxed));
    const int n = buffer.getNumSamples();
    if (numCh <= 0 || n <= 0)
        return;

    const auto scope = fifo.write (n);
    for (int ch = 0; ch < numCh; ++ch)
    {
        if (scope.blockSize1 > 0)
            fifoBuffer.copyFrom (ch, scope.startIndex1, buffer, ch, 0, scope.blockSize1);
        if (scope.blockSize2 > 0)
            fifoBuffer.copyFrom (ch, scope.startIndex2, buffer, ch, scope.blockSize1, scope.blockSize2);
    }
    // anything that did not fit is simply dropped: the display never blocks audio
}

int Analyzer::copyBandFrequencies (std::array<float, maxBands>& dest) const
{
    const juce::SpinLock::ScopedLockType sl (layoutLock);
    dest = bandFreqs;
    return numBandsPublished.load();
}

void Analyzer::run()
{
    juce::ScopedNoDenormals noDenormals;

    while (! threadShouldExit())
    {
        if (needsRebuild.exchange (false))
        {
            rebuildFilterBank();
            rebuildMeters();
        }

        if (lfResHz.load() != builtLfRes)
            rebuildFilterBank();

        if (responseMs.load() != builtResponse || detectRms.load() != builtDetect)
            updateEnvelopeCoeffs();

        if (resetMeterRequest.exchange (false))
        {
            tpMaxDb.store (-200.0f);
            rmsMaxDb.store (-200.0f);
        }

        const int ready = fifo.getNumReady();
        if (ready > 0)
        {
            const auto scope = fifo.read (ready);
            if (scope.blockSize1 > 0) processChunk (scope.startIndex1, scope.blockSize1);
            if (scope.blockSize2 > 0) processChunk (scope.startIndex2, scope.blockSize2);
        }

        wait (5);
    }
}

void Analyzer::rebuildFilterBank()
{
    const double fs = sampleRate.load();
    const int lf = juce::jlimit (5, 100, lfResHz.load());
    builtLfRes = lfResHz.load();

    struct Spec { double f, q; };
    std::vector<Spec> specs;

    // below 250 Hz: constant bandwidth (= LF resolution), like the PAZ
    for (int f = lf; f < 250 && (int) specs.size() < maxBands; f += lf)
        specs.push_back ({ (double) f, (double) f / (double) lf });

    // above 250 Hz: 1/7 octave spacing, Q ~ 10
    const double top = juce::jmin (20000.0, fs * 0.42);
    for (int n = 0; (int) specs.size() < maxBands; ++n)
    {
        const double f = 250.0 * std::pow (2.0, n / 7.0);
        if (f > top) break;
        specs.push_back ({ f, 10.0 });
    }

    // assign each band to the lowest sample rate that still has >= 8x headroom
    bands.clear();
    bands.resize (specs.size());
    int maxLevel = 0;
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const double fHigh = specs[i].f * (1.0 + 0.5 / specs[i].q);
        int k = (int) std::floor (std::log2 (fs / (8.0 * fHigh)));
        k = juce::jlimit (0, 12, k);
        bands[i].level = k;
        bands[i].freq = (float) specs[i].f;
        maxLevel = juce::jmax (maxLevel, k);
    }

    levels.clear();
    levels.resize ((size_t) maxLevel + 1);
    const double butterQ[3] = { 0.5176380902, 0.7071067812, 1.9318516526 };
    for (int k = 0; k <= maxLevel; ++k)
    {
        auto& L = levels[(size_t) k];
        L.rate = fs / std::pow (2.0, k);
        if (k > 0)
        {
            const double inRate = fs / std::pow (2.0, k - 1);
            for (int s = 0; s < 3; ++s)
                designLowPass (L.lp[(size_t) s].b0, L.lp[(size_t) s].b1, L.lp[(size_t) s].b2,
                               L.lp[(size_t) s].a1, L.lp[(size_t) s].a2, 0.15 * inRate, butterQ[s], inRate);
        }
    }

    // two identical 2nd-order sections: per-section Q so the cascade has the target bandwidth
    const double cascadeFactor = std::sqrt (std::sqrt (2.0) - 1.0);
    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];
        const double rate = levels[(size_t) b.level].rate;
        designBandPass (b.bp.b0, b.bp.b1, b.bp.b2, b.bp.a1, b.bp.a2, specs[i].f, specs[i].q * cascadeFactor, rate);
        levels[(size_t) b.level].bands.push_back ((int) i);
    }

    {
        const juce::SpinLock::ScopedLockType sl (layoutLock);
        bandFreqs.fill (0.0f);
        for (size_t i = 0; i < bands.size(); ++i)
            bandFreqs[i] = bands[i].freq;
        numBandsPublished.store ((int) bands.size());
    }

    for (auto& v : bandPow) v.store (0.0f);
    for (auto& v : bandMax) v.store (0.0f);
    layoutVersion.fetch_add (1);

    builtResponse = -1.0f;
    updateEnvelopeCoeffs();
}

void Analyzer::updateEnvelopeCoeffs()
{
    builtResponse = responseMs.load();
    builtDetect = detectRms.load();
    const double tau = juce::jlimit (1.0, 10000.0, (double) builtResponse) * 0.001;

    for (auto& b : bands)
    {
        const double rate = levels[(size_t) b.level].rate;
        if (builtDetect == 1)
            b.coeff = (float) (1.0 - std::exp (-1.0 / (tau * rate)));    // RMS: one-pole on x^2
        else
            b.coeff = (float) std::exp (-2.0 / (tau * rate));            // Peak: release of the squared envelope
    }
}

void Analyzer::rebuildMeters()
{
    const double fs = sampleRate.load();
    subBlockLen = juce::jmax (1, (int) std::round (fs * 0.01)); // 10 ms
    ringLen = 60;                                                  // 60 x 10 ms = 600 ms window

    for (auto& m : meters)
    {
        m.tpHistory.fill (0.0f);
        m.subSum = 0.0;
        m.subCount = 0;
        m.ring.assign ((size_t) ringLen, 0.0);
        m.ringPos = 0;
        m.ringSum = 0.0;
    }

    // 4x oversampling interpolator for true peak (48 taps, Kaiser windowed sinc)
    const double centre = 23.5, beta = 8.0;
    std::array<double, 48> h {};
    for (int n = 0; n < 48; ++n)
    {
        const double t = (n - centre) / 4.0;
        const double sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (pi * t) / (pi * t);
        const double r = (n - centre) / (centre + 0.5);
        const double w = besselI0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / besselI0 (beta);
        h[(size_t) n] = sinc * w;
    }
    for (int p = 0; p < 4; ++p)
    {
        double sum = 0.0;
        for (int k = 0; k < 12; ++k) sum += h[(size_t) (p + 4 * k)];
        for (int k = 0; k < 12; ++k) tpCoeffs[(size_t) (p + 4 * k)] = (float) (h[(size_t) (p + 4 * k)] / sum);
    }
}

void Analyzer::processChunk (int start, int numSamples)
{
    const int numCh = numChannels.load();
    const bool doSpectrum = spectrumActive.load (std::memory_order_relaxed) && ! bands.empty();
    const bool rms = builtDetect == 1;

    for (int ch = 0; ch < numCh; ++ch)
    {
        const float* x = fifoBuffer.getReadPointer (ch, start);

        // ---------------- meter ----------------
        auto& m = meters[(size_t) ch];
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = x[i];
            std::memmove (m.tpHistory.data() + 1, m.tpHistory.data(), 11 * sizeof (float));
            m.tpHistory[0] = s;

            peak = juce::jmax (peak, std::abs (s));
            for (int p = 0; p < 4; ++p)
            {
                float y = 0.0f;
                for (int k = 0; k < 12; ++k)
                    y += tpCoeffs[(size_t) (p + 4 * k)] * m.tpHistory[(size_t) k];
                peak = juce::jmax (peak, std::abs (y));
            }

            m.subSum += (double) s * s;
            if (++m.subCount >= subBlockLen)
            {
                m.ringSum -= m.ring[(size_t) m.ringPos];
                m.ring[(size_t) m.ringPos] = m.subSum;
                m.ringSum += m.subSum;
                m.ringPos = (m.ringPos + 1) % ringLen;
                m.subSum = 0.0;
                m.subCount = 0;

                const double meanSquare = juce::jmax (0.0, m.ringSum) / (double) (ringLen * subBlockLen);
                const float db = toDb (2.0 * meanSquare); // AES17: sine reads the same as its peak
                rmsDb[(size_t) ch].store (db, std::memory_order_relaxed);
                atomicMax (rmsMaxDb, db);
            }
        }
        const float tpDb = peak > 1.0e-10f ? 20.0f * std::log10 (peak) : -200.0f;
        atomicMax (tpSinceRead[(size_t) ch], tpDb);
        atomicMax (tpMaxDb, tpDb);

        // ---------------- spectrum ----------------
        if (! doSpectrum)
            continue;

        for (int i = 0; i < numSamples; ++i)
        {
            float v = x[i];
            size_t k = 0;
            for (;;)
            {
                auto& L = levels[k];
                for (int bi : L.bands)
                {
                    auto& b = bands[(size_t) bi];
                    auto& st = b.state[(size_t) ch];
                    float y = b.bp.process (v, st[0], st[1]);
                    y = b.bp.process (y, st[2], st[3]);
                    const float p = y * y;
                    float& e = b.env[(size_t) ch];
                    if (rms)
                        e += b.coeff * (p - e);
                    else
                        e = juce::jmax (p, e * b.coeff);
                    b.mx[(size_t) ch] = juce::jmax (b.mx[(size_t) ch], e);
                }

                if (k + 1 >= levels.size())
                    break;

                auto& next = levels[k + 1];
                auto& ls = next.lpState[(size_t) ch];
                float f = next.lp[0].process (v, ls[0], ls[1]);
                f = next.lp[1].process (f, ls[2], ls[3]);
                f = next.lp[2].process (f, ls[4], ls[5]);

                auto& ph = next.phase[(size_t) ch];
                ph ^= 1;
                if (ph == 0)
                    break;           // drop every second sample
                v = f;
                ++k;
            }
        }

        for (size_t bi = 0; bi < bands.size(); ++bi)
        {
            auto& b = bands[bi];
            const size_t idx = (size_t) ch * maxBands + bi;
            bandPow[idx].store (b.env[(size_t) ch], std::memory_order_relaxed);
            atomicMax (bandMax[idx], b.mx[(size_t) ch]);
            b.mx[(size_t) ch] = 0.0f;
        }
    }
}
