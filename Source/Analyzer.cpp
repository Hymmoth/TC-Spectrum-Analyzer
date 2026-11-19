#include "Analyzer.h"
#include <cmath>
#include <complex>

namespace
{
    constexpr double pi = 3.14159265358979323846;

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
            rebuildBands();
            rebuildMeters();
        }

        if (lfResHz.load() != builtLfRes)
            rebuildBands();

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

        wait (4);
    }
}

void Analyzer::rebuildBands()
{
    const double fs = sampleRate.load();
    builtLfRes = lfResHz.load();
    const double lf = juce::jlimit (5, 100, builtLfRes);

    struct Spec { double f, lo, hi; };
    std::vector<Spec> specs;

    // below 250 Hz: constant bandwidth (= LF resolution), like the PAZ
    for (double f = lf; f < 250.0 && (int) specs.size() < maxBands; f += lf)
        specs.push_back ({ f, f - lf * 0.5, f + lf * 0.5 });

    // above 250 Hz: 1/7 octave bands, Q ~ 10
    const double top = juce::jmin (20000.0, fs * 0.45);
    const double edge = std::pow (2.0, 1.0 / 14.0);
    for (int n = 0; (int) specs.size() < maxBands; ++n)
    {
        const double f = 250.0 * std::pow (2.0, n / 7.0);
        if (f > top) break;
        double lo = f / edge;
        if (! specs.empty()) lo = juce::jmax (lo, specs.back().hi);
        specs.push_back ({ f, lo, f * edge });
    }

    // each band runs at the lowest sample rate that keeps >= 4x headroom above its upper edge
    bands.clear();
    bands.resize (specs.size());
    int maxLevel = 0;
    for (size_t i = 0; i < specs.size(); ++i)
    {
        int k = (int) std::floor (std::log2 (fs / (4.0 * specs[i].hi)));
        k = juce::jlimit (0, 12, k);
        bands[i].level = k;
        bands[i].freq = (float) specs[i].f;
        maxLevel = juce::jmax (maxLevel, k);
    }

    levels.clear();
    levels.resize ((size_t) maxLevel + 1);
    // 8th-order Butterworth decimation filter at 0.15 fs_in: -0.2 dB at the top band, aliases < -60 dB
    const double butterQ[4] = { 0.5097955791, 0.6013448869, 0.8999762231, 2.5629154477 };
    for (int k = 0; k <= maxLevel; ++k)
    {
        auto& L = levels[(size_t) k];
        L.rate = fs / std::pow (2.0, k);
        if (k > 0)
        {
            const double inRate = fs / std::pow (2.0, k - 1);
            for (int s = 0; s < 4; ++s)
            {
                auto& q = L.lp[(size_t) s];
                const double w0 = 2.0 * pi * 0.15;
                const double alpha = std::sin (w0) / (2.0 * butterQ[s]);
                const double c = std::cos (w0), a0 = 1.0 + alpha;
                q.b0 = (float) ((1.0 - c) * 0.5 / a0);
                q.b1 = (float) ((1.0 - c) / a0);
                q.b2 = q.b0;
                q.a1 = (float) (-2.0 * c / a0);
                q.a2 = (float) ((1.0 - alpha) / a0);
                juce::ignoreUnused (inRate);
            }
        }
    }

    // Chebyshev type I band-pass, prototype order 7 (0.5 dB ripple) -> 7 biquads, 14th order.
    // A tone at the centre of a band is ~60 dB down in the neighbouring bands.
    constexpr int order = 7;
    const double rippleDb = 0.5;
    const double eps = std::sqrt (std::pow (10.0, rippleDb / 10.0) - 1.0);
    const double mu = std::asinh (1.0 / eps) / order;
    using C = std::complex<double>;

    for (size_t i = 0; i < specs.size(); ++i)
    {
        auto& b = bands[i];
        const double rate = levels[(size_t) b.level].rate;
        const double T2 = 2.0 * rate;
        const double wl = T2 * std::tan (pi * specs[i].lo / rate);   // pre-warped edges
        const double wh = T2 * std::tan (pi * specs[i].hi / rate);
        const double w0sq = wl * wh, bw = wh - wl;

        b.numSections = 0;
        auto addPole = [&] (C s)
        {
            const C z = (1.0 + s / T2) / (1.0 - s / T2);
            auto& sec = b.sec[(size_t) b.numSections++];
            sec.a1 = -2.0 * z.real();
            sec.a2 = std::norm (z);
            sec.b0 = 1.0;
        };

        for (int k = 1; k <= order; ++k)
        {
            const double theta = (2.0 * k - 1.0) * pi / (2.0 * order);
            const C p (-std::sinh (mu) * std::sin (theta), std::cosh (mu) * std::cos (theta));
            if (p.imag() < -1.0e-12)
                continue;                                    // conjugates are implied
            // low-pass -> band-pass: s^2 - p B s + w0^2 = 0
            const C disc = std::sqrt (p * p * bw * bw - 4.0 * w0sq);
            const C s1 = (p * bw + disc) * 0.5, s2 = (p * bw - disc) * 0.5;
            if (std::abs (p.imag()) <= 1.0e-12)
                addPole (s1.imag() >= 0.0 ? s1 : s2);        // real prototype pole -> one conjugate pair
            else
            {
                addPole (s1);
                addPole (s2);
            }
        }

        // normalise the gain to 1 at the (pre-warped) centre: a passband peak for odd order
        const double wc = 2.0 * std::atan (std::sqrt (w0sq) / T2);
        const C zc = std::polar (1.0, -wc), zc2 = zc * zc;
        C h (1.0, 0.0);
        for (int s = 0; s < b.numSections; ++s)
        {
            const auto& sec = b.sec[(size_t) s];
            h *= (1.0 - zc2) / (1.0 + sec.a1 * zc + sec.a2 * zc2);
        }
        const double g = std::pow (1.0 / std::abs (h), 1.0 / b.numSections);
        for (int s = 0; s < b.numSections; ++s)
            b.sec[(size_t) s].b0 = g;

        levels[(size_t) b.level].bands.push_back ((int) i);
    }

    {
        const juce::SpinLock::ScopedLockType sl (layoutLock);
        bandFreqs.fill (0.0f);
        for (size_t i = 0; i < bands.size(); ++i)
            bandFreqs[i] = bands[i].freq;
        numBandsPublished.store ((int) bands.size());
    }

    silentSamples.fill (0.0);
    for (auto& v : bandPow) v.store (0.0f);
    for (auto& v : bandMax) v.store (0.0f);
    layoutVersion.fetch_add (1);
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
    const bool doSpectrum = spectrumActive.load (std::memory_order_relaxed);

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
        if (! doSpectrum || bands.empty())
            continue;

        if ((hiddenMask.load (std::memory_order_relaxed) >> ch) & 1u)
        {
            for (size_t bi = 0; bi < bands.size(); ++bi)
                bandPow[(size_t) ch * maxBands + bi].store (0.0f, std::memory_order_relaxed);
            continue;
        }

        auto& silent = silentSamples[(size_t) ch];
        silent = peak > 0.0f ? 0.0 : silent + numSamples;
        if (silent > sampleRate.load())
        {
            // more than a second of digital silence: the filters have rung out, skip the work
            for (size_t bi = 0; bi < bands.size(); ++bi)
                bandPow[(size_t) ch * maxBands + bi].store (0.0f, std::memory_order_relaxed);
            continue;
        }

        for (auto& b : bands) { b.mx[(size_t) ch] = 0.0f; b.sum[(size_t) ch] = 0.0; }
        std::array<int, 16> levelCount {};

        for (int i = 0; i < numSamples; ++i)
        {
            float v = x[i];
            size_t k = 0;
            for (;;)
            {
                auto& L = levels[k];
                ++levelCount[k];
                for (int bi : L.bands)
                {
                    auto& b = bands[(size_t) bi];
                    auto& st = b.state[(size_t) ch];
                    double y = v;
                    for (int s = 0; s < b.numSections; ++s)
                    {
                        const auto& sec = b.sec[(size_t) s];
                        double& s1 = st[(size_t) (2 * s)];
                        double& s2 = st[(size_t) (2 * s + 1)];
                        const double in = y;
                        y = sec.b0 * in + s1;
                        s1 = -sec.a1 * y + s2;
                        s2 = -sec.b0 * in - sec.a2 * y;
                    }
                    const float p = (float) (y * y);
                    b.sum[(size_t) ch] += p;
                    b.mx[(size_t) ch] = juce::jmax (b.mx[(size_t) ch], p);
                }

                if (k + 1 >= levels.size())
                    break;

                auto& next = levels[k + 1];
                auto& ls = next.lpState[(size_t) ch];
                float f = next.lp[0].process (v, ls[0], ls[1]);
                f = next.lp[1].process (f, ls[2], ls[3]);
                f = next.lp[2].process (f, ls[4], ls[5]);
                f = next.lp[3].process (f, ls[6], ls[7]);

                auto& ph = next.phase[(size_t) ch];
                ph ^= 1;
                if (ph == 0)
                    break;           // keep every second sample
                v = f;
                ++k;
            }
        }

        for (size_t bi = 0; bi < bands.size(); ++bi)
        {
            auto& b = bands[bi];
            const int n = levelCount[(size_t) b.level];
            const size_t idx = (size_t) ch * maxBands + bi;
            if (n > 0)
                bandPow[idx].store ((float) (b.sum[(size_t) ch] / n), std::memory_order_relaxed);
            atomicMax (bandMax[idx], b.mx[(size_t) ch]);
        }
    }
}
