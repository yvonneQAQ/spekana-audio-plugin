#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>
#include <vector>



//==============================================================================
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       )
{
    formatManager.registerBasicFormats(); // ⭐ 支持 wav, aiff, mp3 等等
}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioPluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioPluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}


void AudioPluginAudioProcessor::loadFile (const juce::File& file)
{
    stopFilePlayback();
    useFileInput = false;

    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader.get() != nullptr)
    {
        auto newSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

        transportSource.setSource (newSource.get(),
                                   0,                    // read ahead buffer
                                   nullptr,              // thread
                                   currentSampleRate);

        readerSource = std::move (newSource);
        useFileInput = true;   // ✅ 选中文件作为输入
    }
}

void AudioPluginAudioProcessor::startFilePlayback()
{
    if (useFileInput && readerSource != nullptr)
        transportSource.setPosition (0.0);
        transportSource.start();
}

void AudioPluginAudioProcessor::stopFilePlayback()
{
    transportSource.stop();
}

// 在 AudioPluginAudioProcessor.cpp 里实现：
void AudioPluginAudioProcessor::getSpectrumCopy (std::array<float, kFftSize / 2>& dest) const
{
    const juce::SpinLock::ScopedLockType lock (fftLock);
    dest = smoothedMagnitudesDb;
}

void AudioPluginAudioProcessor::getTopPeaksCopy (std::array<float, kNumNoisyPeaks>& freqsHz,
                                                 std::array<float, kNumNoisyPeaks>& residualDbOut) const
{
    const juce::SpinLock::ScopedLockType lock (peakLock);   // ⭐ 用跟 runFftAndFindPeaks 一样的锁

    for (int i = 0; i < kNumNoisyPeaks; ++i)
    {
        freqsHz[(size_t) i]       = topFrequenciesHz[(size_t) i];   // runFftAndFindPeaks 填的 noisy peaks 频率
        residualDbOut[(size_t) i] = topMagnitudes[(size_t) i];      // “高出 envelope 的 dB”
    }
}


// 这个名字可以保留不变：Editor 直接用它显示 10 行文本


void AudioPluginAudioProcessor::updateEnvelopeAndNoisyPeaks()
{
    const int numBins = (int) smoothedMagnitudesDb.size();
    if (numBins <= 0)
        return;

    // ===============================
    // 1) 先做一个粗糙的 envelope（平均到 60 个 band）
    // ===============================
    std::array<float, kNumEnvBands> bandSum   {};
    std::array<float, kNumEnvBands> bandCount {};

    for (int bin = 0; bin < numBins; ++bin)
    {
        float db = smoothedMagnitudesDb[(size_t) bin];

        // bin -> band 映射 (线性分配)：
        int bandIdx = (bin * kNumEnvBands) / numBins;
        bandIdx = juce::jlimit (0, kNumEnvBands - 1, bandIdx);

        bandSum[bandIdx]   += db;
        bandCount[bandIdx] += 1.0f;
    }

    // 求每个 band 的平均值
    std::array<float, kNumEnvBands> bandEnv {};
    for (int b = 0; b < kNumEnvBands; ++b)
    {
        if (bandCount[b] > 0.0f)
            bandEnv[b] = bandSum[b] / bandCount[b];
        else
            bandEnv[b] = -100.0f;     // 很小的 dB
    }

    // 把 bandEnv 拉回每个 bin 上（简单做：同一 band 内用同一个值）
    for (int bin = 0; bin < numBins; ++bin)
    {
        int bandIdx = (bin * kNumEnvBands) / numBins;
        bandIdx = juce::jlimit (0, kNumEnvBands - 1, bandIdx);

        envelopeDb[(size_t) bin] = bandEnv[bandIdx];
    }

    // ===============================
    // 2) residual = smoothedMagnitudesDb - envelopeDb
    // ===============================
    for (int bin = 0; bin < numBins; ++bin)
    {
        residualDb[(size_t) bin] = smoothedMagnitudesDb[(size_t) bin] - envelopeDb[(size_t) bin];
    }

    // ===============================
    // 3) 在 residual 上找 noisy peaks
    // ===============================
    struct Peak
    {
        int   bin      = 0;
        float residual = -1.0e9f;
    };

    // 简单存很多 peak 然后排序
    std::vector<Peak> candidates;
    candidates.reserve (numBins);

    const float residualThreshold = 3.5f; // 至少比 envelope 高 6dB 才算 noisy

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        float r = residualDb[(size_t) bin];

        // residual 要够大，而且是局部最大
        if (r > residualThreshold
            && r > residualDb[(size_t) bin - 1]
            && r > residualDb[(size_t) bin + 1])
        {
            Peak p;
            p.bin      = bin;
            p.residual = r;
            candidates.push_back (p);
        }
    }

    // 排序：按 residual 从大到小
    std::sort (candidates.begin(), candidates.end(),
               [] (const Peak& a, const Peak& b) { return a.residual > b.residual; });

    // ===============================
    // 4) 选前 kNumNoisyPeaks 个，算频率 Hz，填到 noisyPeakFreqsHz
    // ===============================
    const double sr      = getSampleRate();
    const double binToHz = (sr * 0.5) / (double) (numBins - 1);  // 0..Nyquist 映到 0..numBins-1

    for (int i = 0; i < kNumNoisyPeaks; ++i)
    {
        if (i < (int) candidates.size())
        {
            auto bin = candidates[(size_t) i].bin;
            noisyPeakResidualDb[(size_t) i] = candidates[(size_t) i].residual;
            noisyPeakFreqsHz[(size_t) i]    = (float) (bin * binToHz);
        }
        else
        {
            // 没有那么多 peak，就填 0
            noisyPeakResidualDb[(size_t) i] = -100.0f;
            noisyPeakFreqsHz[(size_t) i]    = 0.0f;
        }
    }
}



//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;

    fifoIndex = 0;
    fifo.fill (0.0f);
    fftBuffer.fill (0.0f);
    magnitudeSpectrum.fill (0.0f);
    topFrequenciesHz.fill (0.0f);
    topMagnitudes.fill (0.0f);
    smoothedMagnitudes.fill (0.0f);
    smoothedMagnitudesDb.fill (0.0f);
    
    // Use this method as the place to do any pre-playback
    // initialisation that you need..
    transportSource.prepareToPlay (samplesPerBlock, sampleRate);
    juce::ignoreUnused (sampleRate, samplesPerBlock);
}

void AudioPluginAudioProcessor::releaseResources()
{
    transportSource.releaseResources();
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}


void AudioPluginAudioProcessor::pushSample (float s)
{
    fifo[fifoIndex++] = s;

    if (fifoIndex >= kFftSize)
    {
        fifoIndex = 0;

        // 把时域数据复制到 FFT buffer 前半部分
        std::copy (fifo.begin(), fifo.end(), fftBuffer.begin());
        // 后半部分清零
        std::fill (fftBuffer.begin() + kFftSize, fftBuffer.end(), 0.0f);

        runFftAndFindPeaks();
    }
}


void AudioPluginAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);

    juce::ScopedNoDenormals noDenormals;

    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // 清理没有输入的输出通道（避免多余的噪声）
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, buffer.getNumSamples());

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // =====================================================
    // 1. 选择信号来源：File vs Mic/Host
    // =====================================================
    if (useFileInput && readerSource != nullptr)
    {
        // 用文件覆盖当前 buffer
        juce::AudioSourceChannelInfo info (&buffer, 0, numSamples);
        buffer.clear();                        // 先清空，避免叠加原输入
        transportSource.getNextAudioBlock (info);
    }
    // else: 不动 buffer → 直接用 host/mic 输入


    // =====================================================
    // 2. 做分析：统一用第 0 个 channel 做 FFT / noisy peaks
    // =====================================================
    if (numChannels > 0)
    {
        auto* channelData = buffer.getReadPointer (0);

        for (int n = 0; n < numSamples; ++n)
            pushSample (channelData[n]);   // 你原来用来填 FIFO + runFftAndFindPeaks 的入口
    }
}


void AudioPluginAudioProcessor::runFftAndFindPeaks()
{
    // 1. 加窗 + FFT
    window.multiplyWithWindowingTable (fftBuffer.data(), kFftSize);
    fft.performFrequencyOnlyForwardTransform (fftBuffer.data());

    const int numBins = kFftSize / 2;

    // 2. 时间平滑：smoothedMagnitudes = 0.9 * old + 0.1 * new
    for (int bin = 0; bin < numBins; ++bin)
    {
        float mag = fftBuffer[bin]; // frequency-only FFT 已经是 magnitude

        smoothedMagnitudes[bin] =
            smoothingCoeff * smoothedMagnitudes[bin]
          + (1.0f - smoothingCoeff) * mag;
    }

    // 3. 转成 dB（存在 smoothedMagnitudesDb 里）
    for (int bin = 0; bin < numBins; ++bin)
    {
        float mag = smoothedMagnitudes[bin];

        if (mag <= 1.0e-6f)
            smoothedMagnitudesDb[bin] = -120.0f;  // 底噪
        else
            smoothedMagnitudesDb[bin] = 20.0f * std::log10 (mag);
    }

    // ==========================================================
    // 4. 先从 smoothedMagnitudesDb 生成一个粗糙的 spectral envelope
    // ==========================================================
    static constexpr int kNumEnvBands = 60;

    std::array<float, kNumEnvBands> bandSum   {};
    std::array<float, kNumEnvBands> bandCount {};
    std::array<float, kNumEnvBands> bandEnv   {};

    for (int bin = 0; bin < numBins; ++bin)
    {
        float db = smoothedMagnitudesDb[(size_t) bin];

        int bandIdx = (bin * kNumEnvBands) / numBins;
        bandIdx = std::clamp (bandIdx, 0, kNumEnvBands - 1);

        bandSum[(size_t) bandIdx]   += db;
        bandCount[(size_t) bandIdx] += 1.0f;
    }

    for (int b = 0; b < kNumEnvBands; ++b)
    {
        if (bandCount[(size_t) b] > 0.0f)
            bandEnv[(size_t) b] = bandSum[(size_t) b] / bandCount[(size_t) b];
        else
            bandEnv[(size_t) b] = -120.0f;
    }

    // 把 bandEnv 拉回每个 bin（同 band 内用同一个 dB）
    for (int bin = 0; bin < numBins; ++bin)
    {
        int bandIdx = (bin * kNumEnvBands) / numBins;
        bandIdx = std::clamp (bandIdx, 0, kNumEnvBands - 1);

        envelopeDb[(size_t) bin] = bandEnv[(size_t) bandIdx];
    }

    // ==========================================================
    // 5. residual = smoothedMagnitudesDb - envelopeDb （单位：dB）
    // ==========================================================
    for (int bin = 0; bin < numBins; ++bin)
        residualDb[(size_t) bin] = smoothedMagnitudesDb[(size_t) bin]
                                  - envelopeDb[(size_t) bin];

    // ==========================================================
    // 6. 在 residual 上找 "noisy peaks"
    //    条件：
    //      - residual > threshold
    //      - 是局部最大
    //      - 频率在 100 ~ 5500 Hz 之间
    // ==========================================================
    struct Peak
    {
        float residualDb;
        int   binIndex;
    };

    std::vector<Peak> peaks;
    peaks.reserve (kNumNoisyPeaks * 2);   // 预留多一点以防有很多候选

    const float residualThreshold = 3.0f; // 可以自己再调：1.0 / 2.0 / 6.0 ...
    const float minHz = 40.0f;
    const float maxHz = 4000.0f;

    for (int bin = 1; bin < numBins - 1; ++bin)
    {
        float r = residualDb[(size_t) bin];

        // 先看是否明显高于 envelope
        if (r <= residualThreshold)
            continue;

        // 局部最大
        if (r <= residualDb[(size_t) bin - 1] ||
            r <= residualDb[(size_t) bin + 1])
            continue;

        // 频率范围
        float hz = (float) bin * (float) currentSampleRate / (float) kFftSize;
        if (hz < minHz || hz > maxHz)
            continue;

        peaks.push_back (Peak { r, bin });
    }

    // 如果一个都没有，也要清零输出
    if (peaks.empty())
    {
        const juce::SpinLock::ScopedLockType lock (peakLock);
        for (int k = 0; k < kNumNoisyPeaks; ++k)
        {
            topFrequenciesHz[(size_t) k] = 0.0f;
            topMagnitudes[(size_t) k]    = 0.0f;
        }
        return;
    }

    // 按 residual 从大到小排序
    std::sort (peaks.begin(), peaks.end(),
               [] (const Peak& a, const Peak& b)
               {
                   return a.residualDb > b.residualDb;
               });

    // ==========================================================
    // 7. 写入成员 topFrequenciesHz / topMagnitudes（用锁保护）
    //    注意：topMagnitudes 存的是 “比 envelope 高出的 dB 数”
    // ==========================================================
    {
        const juce::SpinLock::ScopedLockType lock (peakLock);

        for (int k = 0; k < kNumNoisyPeaks; ++k)
        {
            if (k < (int) peaks.size())
            {
                int   binIndex = peaks[(size_t) k].binIndex;
                float hz       = (float) binIndex * (float) currentSampleRate
                               / (float) kFftSize;

                topFrequenciesHz[(size_t) k] = hz;
                topMagnitudes[(size_t) k]    = peaks[(size_t) k].residualDb;
            }
            else
            {
                // 不足 10 个时，剩下的在“末尾”填 0
                topFrequenciesHz[(size_t) k] = 0.0f;
                topMagnitudes[(size_t) k]    = 0.0f;
            }
        }
    }

// ⭐⭐ Debug：在 processor 侧打印所有 kNumPeaks
{
    juce::String s ("Processor noisy peaks (Hz): ");
    for (int k = 0; k < kNumNoisyPeaks; ++k)
        s << juce::String (topFrequenciesHz[(size_t) k], 1) << "  ";

    DBG (s);
}
}





void AudioPluginAudioProcessor::logTopFrequencies() const
{
    juce::String s ("Top 10 freqs (Hz): ");
    for (int k = 0; k < kNumNoisyPeaks; ++k)
    {
        s << juce::String (topFrequenciesHz[k], 1) << " ";
    }
    DBG (s);
}


//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
    juce::ignoreUnused (destData);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    juce::ignoreUnused (data, sizeInBytes);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
