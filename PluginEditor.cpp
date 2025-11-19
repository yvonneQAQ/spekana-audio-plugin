#include "PluginProcessor.h"
#include "PluginEditor.h"


#include <cmath> // 为了 std::log2, std::round


namespace
{
    juce::String freqToNoteName (float freqHz)
    {
        if (freqHz <= 0.0f)
            return "-";

        // MIDI number from frequency, A4 = 440Hz => 69
        const float midi = 69.0f + 12.0f * std::log2 (freqHz / 440.0f);
        const int midiNote = (int) std::round (midi);

        // 限制一下范围，避免 weird 数字
        if (midiNote < 0 || midiNote > 127)
            return juce::String (freqHz, 1) + " Hz";

        static const char* noteNames[12] =
        {
            "C", "C#", "D", "D#", "E", "F",
            "F#", "G", "G#", "A", "A#", "B"
        };

        const int noteIndex = midiNote % 12;
        const int octave    = midiNote / 12 - 1; // 60 -> C4

        juce::String name = noteNames[noteIndex];
        name << octave;

        return name;
    }
    // ====== 频率 <-> Mel scale 工具函数 ======
    constexpr float kMinDisplayFreq = 80.0f;
    constexpr float kMaxDisplayFreq = 5000.0f;

    inline float hzToMel (float hz)
    {
        return 2595.0f * std::log10 (1.0f + hz / 700.0f);
    }

    inline float melToHz (float mel)
    {
        return 700.0f * (std::pow (10.0f, mel / 2595.0f) - 1.0f);
    }
}


//==============================================================================

AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    // juce::ignoreUnused (processorRef);   // ⭐ 现在会用到，可以删掉

    setSize (800, 600);
    // 初始化 spectrogram 的尺寸（可以跟窗口大小同步）
    spectrogramImage = juce::Image (juce::Image::RGB, 600, 300, true);

    startTimerHz (30);    // ⭐ Timer 每秒 30 次

    //add button
    addAndMakeVisible (freezeButton);
    addAndMakeVisible (releaseButton);
    addAndMakeVisible (loadFileButton);
    addAndMakeVisible (playButton);
    addAndMakeVisible (micButton); 

    // 点击 Freeze：立刻 snapshot 一帧，并且进入冻结状态

   loadFileButton.onClick = [this]()
{
    // 在成员里创建一个 FileChooser 实例
    fileChooser = std::make_unique<juce::FileChooser>(
        "Select an audio file",
        juce::File{},
        "*.wav;*.aiff;*.aif;*.mp3");

    fileChooser->launchAsync(
        juce::FileBrowserComponent::openMode
      | juce::FileBrowserComponent::canSelectFiles,
        [this](const juce::FileChooser& fc)
        {
            auto file = fc.getResult();

            if (file.existsAsFile())
            {
                processorRef.loadFile (file);
                playButton.setButtonText ("Play");
            }

            // 用完就释放 FileChooser
            fileChooser.reset();
        });
};

    playButton.onClick = [this]()
    {
        if (! processorRef.getUseFileInput())
            return; // 没有文件就不播

        if (processorRef.isFilePlaying())
        {
            processorRef.stopFilePlayback();
            playButton.setButtonText ("Play");
        }
        else
        {
            processorRef.startFilePlayback();
            playButton.setButtonText ("Stop");
        }
    };
// ⭐ 新增：Mic 按钮 → 回到麦克风
    micButton.onClick = [this]()
    {
        processorRef.stopFilePlayback();      // 停止 transportSource
        processorRef.useMicrophoneInput();   // useFileInput = false

        playButton.setButtonText ("Play");   // 恢复按钮显示
    };

freezeButton.onClick = [this]()
{
    // 进入“冻结显示”模式
    isFrozen = true;

    // 用当前最新分析结果刷新一帧显示
    spectrumForDrawing = latestSpectrum;
    topFreqsForDrawing = latestTopFreqs;
    topMagsForDrawing  = latestTopMags;

    // 更新瀑布图的这一帧
    pushSpectrumToImage();

    // ⭐ 在按下 freeze 的这一刻，把这 10 个 peak 写入 txt 文件
    //    （格式就是你原来给 Python 用的那种带时间戳 + note name 的行）
    writePeaksToTextFile (topFreqsForDrawing);

    repaint();
};



releaseButton.onClick = [this]()
{
    isFrozen = false;
    // Timer 已经在构造函数里 start 过了，这里一般不用再 start
    repaint();
};
}


AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor() {}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    auto fullBounds = getLocalBounds();
    auto bounds = fullBounds.reduced (10);

    // -------- 顶部状态条：20 px --------
    auto statusArea = bounds.removeFromTop (20);
    g.setColour (isFrozen ? juce::Colours::yellow : juce::Colours::lightgreen);
    g.setFont (14.0f);
    g.drawText (isFrozen ? "Status: FROZEN" : "Status: LIVE",
                statusArea,
                juce::Justification::centredLeft);

    // 底部 60 px 留给按钮（按钮自己画）
    bounds.removeFromBottom (60);

    // 底部 160 px 给 Top10 文本
    auto textArea = bounds.removeFromBottom (220);

    // 剩下的就是 spectrogram 区
    auto spectroArea = bounds;

    // ===== 上半：Spectrogram =====
    auto plotArea = spectroArea.reduced (20, 10);

    g.setColour (juce::Colours::darkgrey);
    g.fillRect (plotArea);
    g.setColour (juce::Colours::grey);
    g.drawRect (plotArea);

    g.drawImageWithin (spectrogramImage,
                       (int) plotArea.getX(),
                       (int) plotArea.getY(),
                       (int) plotArea.getWidth(),
                       (int) plotArea.getHeight(),
                       juce::RectanglePlacement::stretchToFit);

        // ===== 在 spectrogram 上叠加频率刻度线（Mel 对应的位置） =====
    {
        const float sampleRate = (float) processorRef.getSampleRate();
        const float nyquist    = sampleRate * 0.5f;

        const float minFreq = kMinDisplayFreq;
        const float maxFreq = juce::jmin (kMaxDisplayFreq, nyquist);

        const float melMin = hzToMel (minFreq);
        const float melMax = hzToMel (maxFreq);

        // 想画的刻度频率（可以根据需要改）
        const float tickFreqs[] = { 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f };

        g.setColour (juce::Colours::darkgrey.withAlpha (0.7f));
        g.setFont (10.0f);

        for (float f : tickFreqs)
        {
            if (f < minFreq || f > maxFreq || f > nyquist)
                continue;

            float mel = hzToMel (f);

            // 计算在 Mel 范围里的归一化位置
            float norm = (mel - melMin) / (melMax - melMin);
            norm = juce::jlimit (0.0f, 1.0f, norm);

            // 对应到 plotArea 里的 y 坐标（注意下方是低频）
            float y = plotArea.getBottom() - norm * plotArea.getHeight();

            // 画一条横线
            g.drawLine (plotArea.getX(), y, plotArea.getRight(), y, 1.0f);

            // 在左边画频率标签
            juce::Rectangle<float> labelArea {
                plotArea.getX() + 2.0f,
                y - 7.0f,
                60.0f,
                14.0f
            };

            juce::String label;
            if (f >= 1000.0f)
                label << juce::String (f / 1000.0f, 1) << " kHz";
            else
                label << juce::String ((int) f) << " Hz";

            g.drawFittedText (label, labelArea.toNearestInt(),
                              juce::Justification::centredLeft, 1);
        }
    }

// ===== 下半：Top 10 文字区域 =====
g.setColour (juce::Colours::darkslategrey);
g.fillRect (textArea);

auto inner = textArea.reduced (10, 5);

// 标题
g.setColour (juce::Colours::white);
g.setFont (16.0f);
auto titleArea = inner.removeFromTop (25);
g.drawText ("Top 10 FFT Peaks (Pitch + Frequency)",
            titleArea,
            juce::Justification::centredLeft);

// 再留一点空白
inner.removeFromTop (5);

// ===== 两列布局：左 5 个，右 5 个 =====
g.setFont (14.0f);

const int numCols       = 2;
const int rowsPerColumn = kNumNoisyPeaks / numCols;  // 10 / 2 = 5

const int colWidth   = inner.getWidth()  / numCols;
const int lineHeight = inner.getHeight() / rowsPerColumn;

for (int i = 0; i < kNumNoisyPeaks; ++i)
{
    // 第几列 / 第几行
    int col = i / rowsPerColumn;   // 0 或 1
    int row = i % rowsPerColumn;   // 0..4

    juce::Rectangle<int> cell;
    cell.setX (inner.getX() + col * colWidth);
    cell.setY (inner.getY() + row * lineHeight);
    cell.setWidth  (colWidth);
    cell.setHeight (lineHeight);

    float freq = topFreqsForDrawing[(size_t) i];
    juce::String text = juce::String (i + 1) + ": ";

    if (freq > 0.0f)
    {
        auto noteName = freqToNoteName (freq);
        text << noteName << "  (" << juce::String (freq, 1) << " Hz)";
    }
    else
    {
        text << "-";
    }

    g.setColour (juce::Colours::white);
    g.drawText (text,
                cell,
                juce::Justification::centredLeft);
}


    // 额外 debug：在文字区域右下角画一句 红字 "DEBUG"
    g.setColour (juce::Colours::red);
    g.drawText ("DEBUG",
                textArea.removeFromBottom (20),
                juce::Justification::centredRight);
}


void AudioPluginAudioProcessorEditor::resized()
{
    auto full = getLocalBounds();
    auto bounds = full.reduced (10);

    // 顶部状态条：20px
    auto statusBar = bounds.removeFromTop (20);
    juce::ignoreUnused (statusBar);

    // 底部按钮区域：40px
    auto buttonRow = bounds.removeFromBottom (60);

    // 文本区域（Top 10 frequencies）：固定 160px
    auto textArea = bounds.removeFromBottom (220);
    juce::ignoreUnused (textArea); // paint() 里使用尺寸并绘制

    // 剩下的是 spectrogram 区域
    auto spectroArea = bounds;

    // ----- 更新 spectrogramImage 尺寸 -----
    {
        int w = juce::jmax (1, spectroArea.getWidth());
        int h = juce::jmax (1, spectroArea.getHeight());
        spectrogramImage = juce::Image (juce::Image::RGB, w, h, true);
    }

    // ======== 按钮布局（5个平均分布） ========
    {
        auto buttons = buttonRow.reduced (10);
        int buttonWidth = buttons.getWidth() / 5;

        freezeButton     .setBounds (buttons.removeFromLeft(buttonWidth).reduced(5));
        releaseButton    .setBounds (buttons.removeFromLeft(buttonWidth).reduced(5));
        loadFileButton   .setBounds (buttons.removeFromLeft(buttonWidth).reduced(5));
        playButton       .setBounds (buttons.removeFromLeft(buttonWidth).reduced(5));
        micButton        .setBounds (buttons.reduced(5)); // 最后剩下的归 Mic
    }
}


void AudioPluginAudioProcessorEditor::pushSpectrumToImage()
{
    auto width   = spectrogramImage.getWidth();
    auto height  = spectrogramImage.getHeight();
    auto numBins = (int) spectrumForDrawing.size();

    if (width <= 0 || height <= 0 || numBins <= 0)
        return;

    // 1. 时间轴向右滚：把整张图往左移 1 像素
    spectrogramImage.moveImageSection(
        0, 0,           // dst x, y
        1, 0,           // src x, y（从第二列开始）
        width - 1,      // 宽度：去掉最右一列
        height
    );

    const float sampleRate = (float) processorRef.getSampleRate();
    if (sampleRate <= 0.0f)
        return;

    const float nyquist = sampleRate * 0.5f;

    // ===== 频率范围 & mel 映射参数（这组只定义一次）=====
    const float minFreq = kMinDisplayFreq;                         // 例如 80 Hz
    const float maxFreq = juce::jmin (kMaxDisplayFreq, nyquist);   // 例如 5000 Hz 或 Nyquist
    const float melMin  = hzToMel (minFreq);
    const float melMax  = hzToMel (maxFreq);

    const int x = width - 1;   // 新的一列在最右侧

    const float minDb = -100.0f;
    const float maxDb =   0.0f;

    // 2. 先画背景灰度 spectrogram（spectrumForDrawing 是 dB）
    for (int y = 0; y < height; ++y)
    {
        // y 从下到上映射到 [minFreq, maxFreq] 的 mel 频率
        float normY = 1.0f - (float) y / (float) (height - 1);
        float mel   = melMin + normY * (melMax - melMin);
        float freq  = melToHz (mel);

        // freq → bin
        float binPos = (freq / nyquist) * (float) (numBins - 1);
        int   bin    = (int) std::round (binPos);
        bin          = juce::jlimit (0, numBins - 1, bin);

        float db = spectrumForDrawing[(size_t) bin];
        if (std::isnan (db) || std::isinf (db))
            db = minDb;

        db = juce::jlimit (minDb, maxDb, db);

        float norm = (db - minDb) / (maxDb - minDb);  // 0..1
        float shade = 1.0f - norm;                    // 能量大 → darker

        juce::Colour colour (shade, shade, shade, 1.0f);
        spectrogramImage.setPixelAt (x, y, colour);
    }

    // 3. 叠加 noisy peaks：用 topFreqsForDrawing 里的 Hz，在同一列画红点
    for (int i = 0; i < (int) topFreqsForDrawing.size(); ++i)
    {
        float f = topFreqsForDrawing[(size_t) i];

        if (f <= 0.0f || f < minFreq || f > maxFreq)
            continue;

        float mel   = hzToMel (f);
        float normY = (mel - melMin) / (melMax - melMin);
        normY       = juce::jlimit (0.0f, 1.0f, normY);

        int y = height - 1 - (int) std::round (normY * (float) (height - 1));

        // 中心点
        spectrogramImage.setPixelAt (x, y, juce::Colours::red);

        // 让点粗一点（可选）
        if (y > 0)
            spectrogramImage.setPixelAt (x, y - 1, juce::Colours::red);
        if (y < height - 1)
            spectrogramImage.setPixelAt (x, y + 1, juce::Colours::red);
    }
}

//==============================================================================
void AudioPluginAudioProcessorEditor::timerCallback()
{
    // 1️⃣ 后台：无论 freeze 与否，都从 processor 拿“最新分析结果”
    processorRef.getSpectrumCopy (latestSpectrum);
    processorRef.getTopPeaksCopy (latestTopFreqs, latestTopMags);

    // 2️⃣ 只有在没 freeze 的时候，才更新“前台显示的数据”和 spectrogram 图像
    if (! isFrozen)
    {
        // 把最新分析结果拷贝到用于显示的数组
        spectrumForDrawing = latestSpectrum;
        topFreqsForDrawing = latestTopFreqs;
        topMagsForDrawing  = latestTopMags;

        // 画一列新的 spectrogram（内部用 spectrumForDrawing）
        pushSpectrumToImage();
    }

    // 3️⃣ 每一帧都重画
    //    注意：所有 label / 绘制都应该使用 spectrumForDrawing / topFreqsForDrawing / topMagsForDrawing
    repaint();
}


void AudioPluginAudioProcessorEditor::writePeaksToTextFile
    (const std::array<float, kNumNoisyPeaks>& freqsHz)
{
    // 保存到 用户文档目录 / AudioPluginPeaks.txt
    auto file = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                    .getChildFile ("AudioPluginPeaks.txt");

    // 确保文件存在
    if (! file.existsAsFile())
        file.create();   // 创建空文件

    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
    if (out == nullptr || (! out->openedOk()))
        return; // 打不开就算了

    // 追加写入：移动到文件末尾
    out->setPosition (file.getSize());

    // 时间戳
    auto nowString = juce::Time::getCurrentTime().toString (true, true); // 日期+时间

    juce::String line;
    line << nowString << "  |  ";

    // 把 10 个 peak 写进去（显示 pitch + Hz）
    for (int i = 0; i < kNumNoisyPeaks; ++i)
    {
        float f = freqsHz[(size_t) i];

        if (f > 0.0f)
        {
            auto noteName = freqToNoteName (f);
            line << (i + 1) << ": " << noteName
                 << " (" << juce::String (f, 1) << " Hz),  ";
        }
        else
        {
            line << (i + 1) << ": -,  ";
        }
    }

    line << "\n";

    out->writeText (line, false, false, "\n");  // 不自动加 BOM，不转行尾
    out->flush();
}




