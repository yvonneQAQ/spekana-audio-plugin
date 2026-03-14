#pragma once

#include "PluginProcessor.h"
#include <array>
#include <memory>
#include <vector>


class AudioPluginAudioProcessor;
class FrozenChordStaffComponent;

struct FrozenChordSnapshot
{
    std::array<float, AudioPluginAudioProcessor::kNumNoisyPeaks> freqsHz {};
    juce::String label;
    bool useQuarterToneMode = false;
    double startTimeSeconds = 0.0;
    double endTimeSeconds = 0.25;
};
//==============================================================================
class AudioPluginAudioProcessorEditor  : public juce::AudioProcessorEditor,
                                         public juce::Timer       // ⭐ 一定要继承 Timer
{
public:
    AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    static constexpr int kNumNoisyPeaks = AudioPluginAudioProcessor::kNumNoisyPeaks;

private:
    bool useQuarterToneMode() const;
    void refreshBassBoostButtonText();
    void captureFrozenChord();
    void refreshStaffComponent();
    void refreshExportButtonState();
    void showTransientStatusMessage (juce::String message, double seconds = 2.5);
    void resetFrozenState();
    void closeActiveFrozenChord();
    void clearSpectrogramImage();

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
    juce::TextButton freezeButton { "Freeze" };
    juce::TextButton unfreezeButton { "Unfreeze" };
    juce::TextButton resetButton { "Reset" };
    juce::TextButton bassBoostButton { "Bass Boost" };
    juce::TextButton quarterToneButton { "Quarter-Tone" };
    juce::TextButton exportMidiButton { "Export MIDI" };
    std::unique_ptr<juce::FileChooser> exportMidiChooser;
    std::unique_ptr<juce::LookAndFeel_V4> lookAndFeel;

    std::vector<FrozenChordSnapshot> frozenChords;
    std::unique_ptr<FrozenChordStaffComponent> staffComponent;
    int freezeCaptureCount = 0;
    double activeFreezeSessionStartWallTimeSeconds = 0.0;
    double activeFreezeSessionOffsetSeconds = 0.0;
    bool pendingFreezeMarker = false;
    juce::String transientStatusMessage;
    double transientStatusMessageExpirySeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessorEditor)
};
