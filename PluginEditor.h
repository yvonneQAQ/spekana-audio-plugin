#pragma once

#include "PluginProcessor.h"
#include <array>


class AudioPluginAudioProcessor;
//==============================================================================
class AudioPluginAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                         public juce::Timer       // ⭐ 一定要继承 Timer
{
public:
    AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    std::unique_ptr<juce::FileChooser> fileChooser;
    static constexpr int kNumNoisyPeaks = AudioPluginAudioProcessor::kNumNoisyPeaks;
    void writePeaksToTextFile (const std::array<float, kNumNoisyPeaks>& freqsHz); 

private:

    void timerCallback() override;      // ⭐ Timer 回调
    void pushSpectrumToImage(); 
    

    AudioPluginAudioProcessor& processorRef;
    // ==== FFT 绘图相关 ====
    static constexpr int kFftOrder = 11;
    static constexpr int kFftSize  = 1 << kFftOrder;
    
    


// 当前这一帧的频谱（dB）
    std::array<float, kFftSize / 2> spectrumForDrawing { };
    std::array<float, kNumNoisyPeaks> topFreqsForDrawing { };
    std::array<float, kNumNoisyPeaks> topMagsForDrawing  { };

    // 🔹 后台最新分析结果（每帧更新，但不一定马上显示）
    std::array<float, kFftSize / 2>   latestSpectrum   { };
    std::array<float, kNumNoisyPeaks> latestTopFreqs   { };
    std::array<float, kNumNoisyPeaks> latestTopMags    { };


    juce::Image spectrogramImage { juce::Image::RGB, 400, 300, true };
    
    bool isFrozen = false;
        // 用来在 UI 里显示的 10 个频率
    std::array<float, kNumNoisyPeaks> scopeFrequencies {};
        // ⭐ 新增：冻结状态 + 按钮
    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton releaseButton { "Release" };
    juce::TextButton loadFileButton { "Load File" };
    juce::TextButton playButton     { "Play" };
    juce::TextButton micButton      { "Mic" }; 
    
    

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
