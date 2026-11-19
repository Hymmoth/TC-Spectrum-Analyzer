#include "Analyzer.h"
#include <cmath>

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
    history.clear();
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
            if (spectrumActive.load (std::memory_order_relaxed))
                runSpectrum();
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

    // each band gets the shortest FFT whose bins are <= 1/6 of its width,
    // so a tone in one band stays ~60 dB down in the next (Blackman-Harris main lobe)
    resolutions.clear();
    bands.clear();
    std::array<int, maxOrder + 1> orderToIndex;
    orderToIndex.fill (-1);

    for (auto& s : specs)
    {
        const double bw = s.hi - s.lo;
        const int order = juce::jlimit (8, maxOrder, (int) std::ceil (std::log2 (6.0 * fs / bw)));

        if (orderToIndex[(size_t) order] < 0)
        {
            orderToIndex[(size_t) order] = (int) resolutions.size();
            Resolution r;
            r.order = order;
            r.size = 1 << order;
            r.hop = juce::jlimit (256, 4096, r.size / 4);
            r.fft = std::make_unique<juce::dsp::FFT> (order);
            r.window.resize ((size_t) r.size);
            r.buffer.assign ((size_t) r.size * 2, 0.0f);
            double sumW2 = 0.0;
            for (int i = 0; i < r.size; ++i)
            {
                const double x = 2.0 * pi * i / (r.size - 1);
                const double w = 0.35875 - 0.48829 * std::cos (x) + 0.14128 * std::cos (2.0 * x) - 0.01168 * std::cos (3.0 * x);
                r.window[(size_t) i] = (float) w;
                sumW2 += w * w;
            }
            r.norm = (float) (2.0 / ((double) r.size * sumW2));
            resolutions.push_back (std::move (r));
        }

        const int ri = orderToIndex[(size_t) order];
        auto& r = resolutions[(size_t) ri];
        const double df = fs / r.size;
        Band b;
        b.freq = (float) s.f;
        b.resolution = ri;
        b.binLo = juce::jlimit (1, r.size / 2, (int) std::ceil (s.lo / df));
        b.binHi = juce::jlimit (1, r.size / 2, (int) std::ceil (s.hi / df) - 1);
        if (b.binHi < b.binLo)
            b.binLo = b.binHi = juce::jlimit (1, r.size / 2, (int) std::round (s.f / df));
        r.bands.push_back ((int) bands.size());
        bands.push_back (b);
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

        // ---------------- history for the FFTs ----------------
        if (doSpectrum)
        {
            if (peak > 0.0f)
                lastSound[(size_t) ch] = totalSamples + numSamples;

            float* h = history.getWritePointer (ch);
            int pos = historyPos;
            int remaining = numSamples, offset = 0;
            while (remaining > 0)
            {
                const int n = juce::jmin (remaining, historySize - pos);
                std::memcpy (h + pos, x + offset, (size_t) n * sizeof (float));
                std::memcpy (h + pos + historySize, x + offset, (size_t) n * sizeof (float));
                pos = (pos + n) & (historySize - 1);
                offset += n;
                remaining -= n;
            }
        }
    }

    if (doSpectrum)
    {
        historyPos = (historyPos + numSamples) & (historySize - 1);
        totalSamples += numSamples;
    }
}

void Analyzer::runSpectrum()
{
    const int numCh = numChannels.load();

    for (auto& r : resolutions)
    {
        if (r.bands.empty() || totalSamples - r.lastRun < r.hop)
            continue;
        r.lastRun = totalSamples;

        for (int ch = 0; ch < numCh; ++ch)
        {
            if (totalSamples - lastSound[(size_t) ch] > r.size)
            {
                // the whole window is digital silence: skip the FFT
                for (int bi : r.bands)
                    bandPow[(size_t) ch * maxBands + (size_t) bi].store (0.0f, std::memory_order_relaxed);
                continue;
            }

            // the last r.size samples, contiguous thanks to the doubled history
            const float* src = history.getReadPointer (ch) + historyPos + historySize - r.size;
            float* buf = r.buffer.data();
            juce::FloatVectorOperations::multiply (buf, src, r.window.data(), r.size);
            std::fill (buf + r.size, buf + 2 * r.size, 0.0f);
            r.fft->performRealOnlyForwardTransform (buf, true);

            for (int bi : r.bands)
            {
                const auto& b = bands[(size_t) bi];
                float sum = 0.0f;
                for (int k = b.binLo; k <= b.binHi; ++k)
                {
                    const float re = buf[2 * k], im = buf[2 * k + 1];
                    sum += re * re + im * im;
                }
                const float p = sum * r.norm;
                const size_t idx = (size_t) ch * maxBands + (size_t) bi;
                bandPow[idx].store (p, std::memory_order_relaxed);
                atomicMax (bandMax[idx], p);
            }
        }
    }
}
