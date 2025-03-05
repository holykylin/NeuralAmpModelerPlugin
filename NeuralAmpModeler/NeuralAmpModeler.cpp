#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <iostream>
#include <utility>

#include "Colors.h"
#include "NeuralAmpModelerCore/NAM/activations.h"
#include "NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"

#include "NeuralAmpModelerControls.h"

using namespace iplug;
using namespace igraphics;

const double kDCBlockerFrequency = 5.0;

// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::NAM_THEMECOLOR, // Foreground
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.3f), // Pressed
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::NAM_THEMECOLOR, // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::NAM_THEMECOLOR.WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text5
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};
const IVStyle titleStyle =
  DEFAULT_STYLE.WithValueText(IText(30, COLOR_WHITE, "Michroma-Regular")).WithDrawFrame(false).WithShadowOffset(2.f);
const IVStyle radioButtonStyle =
  style
    .WithColor(EVColor::kON, PluginColors::NAM_THEMECOLOR) // Pressed buttons and their labels
    .WithColor(EVColor::kOFF, PluginColors::NAM_THEMECOLOR.WithOpacity(0.1f)) // Unpressed buttons
    .WithColor(EVColor::kX1, PluginColors::NAM_THEMECOLOR.WithOpacity(0.6f)); // Unpressed buttons' labels

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}

const std::string kCalibrateInputParamName = "CalibrateInput";
const bool kDefaultCalibrateInput = false;
const std::string kInputCalibrationLevelParamName = "InputCalibrationLevel";
const double kDefaultInputCalibrationLevel = 12.0;


NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // 修改为立体声配置
  MakeDefaultInput(ERoute::kInput, 0, 2, "AudioInput"); // 立体声输入
  MakeDefaultOutput(ERoute::kOutput, 0, 2, "AudioOutput"); // 立体声输出
  
  _InitToneStack();
  nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", true);
  GetParam(kOutputMode)->InitEnum("OutputMode", 1, {"Raw", "Normalized", "Calibrated"}); // TODO DRY w/ control
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
  GetParam(kInputCalibrationLevel)
    ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");
    
  // 初始化新参数
  GetParam(kProcessingMode)->InitEnum("Mode", 0, {"Guitar", "Vocal"});
  GetParam(kABToggle)->InitEnum("Slot", 0, {"A", "B"});
  GetParam(kABMix)->InitDouble("A/B Mix", 0.0, 0.0, 1.0, 0.01);

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);

  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconOnSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);
    const auto irIconOffSVG = pGraphics->LoadSVG(IR_ICON_OFF_FN);

    const auto backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto inputLevelBackgroundBitmap = pGraphics->LoadBitmap(INPUTLEVELBACKGROUND_FN);
    const auto linesBitmap = pGraphics->LoadBitmap(LINES_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadSVG(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadSVG(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadSVG(METERBACKGROUND_FN);

    const auto b = pGraphics->GetBounds();
    const auto mainArea = b.GetPadded(-20);
    const auto contentArea = mainArea.GetPadded(-10);
    const auto titleHeight = 50.0f;
    const auto titleArea = contentArea.GetFromTop(titleHeight);

    // Areas for knobs
    const auto knobsPad = 20.0f;
    const auto knobsExtraSpaceBelowTitle = 25.0f;
    const auto singleKnobPad = -2.0f;
    const auto knobsArea = contentArea.GetFromTop(NAM_KNOB_HEIGHT)
                             .GetReducedFromLeft(knobsPad)
                             .GetReducedFromRight(knobsPad)
                             .GetVShifted(titleHeight + knobsExtraSpaceBelowTitle);
    const auto inputKnobArea = knobsArea.GetGridCell(0, kInputLevel, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto noiseGateArea = knobsArea.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto bassKnobArea = knobsArea.GetGridCell(0, kToneBass, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto midKnobArea = knobsArea.GetGridCell(0, kToneMid, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, kToneTreble, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto outputKnobArea = knobsArea.GetGridCell(0, kOutputLevel, 1, numKnobs).GetPadded(-singleKnobPad);

    const auto ngToggleArea =
      noiseGateArea.GetVShifted(noiseGateArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto eqToggleArea = midKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto outNormToggleArea =
      outputKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);

    // Areas for model and IR
    const auto fileWidth = 200.0f;
    const auto fileHeight = 30.0f;
    const auto irYOffset = 38.0f;
    const auto modelArea =
      contentArea.GetFromBottom((2.0f * fileHeight)).GetFromTop(fileHeight).GetMidHPadded(fileWidth).GetVShifted(-1);
    const auto modelIconArea = modelArea.GetFromLeft(30).GetTranslated(-40, 10);
    const auto irArea = modelArea.GetVShifted(irYOffset);
    const auto irSwitchArea = irArea.GetFromLeft(30.0f).GetHShifted(-40.0f).GetScaledAboutCentre(0.6f);

    // Areas for meters
    const auto inputMeterArea = contentArea.GetFromLeft(30).GetHShifted(-20).GetMidVPadded(100).GetVShifted(-25);
    const auto outputMeterArea = contentArea.GetFromRight(30).GetHShifted(20).GetMidVPadded(100).GetVShifted(-25);

    // Misc Areas
    const auto settingsButtonArea = CornerButtonArea(b);

    // Model loader button
    auto loadModelCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        // Sets mNAMPath and mStagedNAM
        const std::string msg = _StageModel(fileName);
        // TODO error messages like the IR loader.
        if (msg.size())
        {
          std::stringstream ss;
          ss << "Failed to load NAM model. Message:\n\n" << msg;
          _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
        }
        std::cout << "Loaded: " << fileName.Get() << std::endl;
      }
    };

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        mIRPath = fileName;
        const dsp::wav::LoadReturnCode retCode = _StageIR(fileName);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::stringstream message;
          message << "Failed to load IR file " << fileName.Get() << ":\n";
          message << dsp::wav::GetMsgForLoadReturnCode(retCode);

          _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
        }
      }
    };

    pGraphics->AttachBackground(BACKGROUND_FN);
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap));
    pGraphics->AttachControl(new IVLabelControl(titleArea, "NEURAL AMP MODELER", titleStyle));
    pGraphics->AttachControl(new ISVGControl(modelIconArea, modelIconSVG));

#ifdef NAM_PICK_DIRECTORY
    const std::string defaultNamFileString = "Select model directory...";
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultNamFileString = "Select model...";
    const std::string defaultIRString = "Select IR...";
#endif
    pGraphics->AttachControl(new NAMFileBrowserControl(modelArea, kMsgTagClearModel, defaultNamFileString.c_str(),
                                                       "nam", loadModelCompletionHandler, style, fileSVG, crossSVG,
                                                       leftArrowSVG, rightArrowSVG, fileBackgroundBitmap),
                             kCtrlTagModelFileBrowser);
    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler, style,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap),
      kCtrlTagIRFileBrowser);
    pGraphics->AttachControl(
      new NAMSwitchControl(ngToggleArea, kNoiseGateActive, "Noise Gate", style, switchHandleBitmap));
    pGraphics->AttachControl(new NAMSwitchControl(eqToggleArea, kEQActive, "EQ", style, switchHandleBitmap));

    // The knobs
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(noiseGateArea, kNoiseGateThreshold, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // The meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // Settings/help/about box
    pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMSettingsPageControl>()->HideAnimated(false);
      },
      gearSVG));

    pGraphics
      ->AttachControl(new NAMSettingsPageControl(b, backgroundBitmap, inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });

    // pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
    // pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetMouseEventsWhenDisabled(false);

    // 添加模式切换和A/B控件区域
    const auto modeToggleHeight = 25.0f;
    const auto modeToggleWidth = 100.0f;
    const auto abControlsWidth = 120.0f;
    
    // 在顶部区域添加模式切换开关
    const auto modeToggleArea = titleArea.GetFromRight(modeToggleWidth).GetMidVPadded(modeToggleHeight/2);
    
    // 添加A/B切换和混合控制区域
    const auto abControlsArea = contentArea.GetFromTop(30.0f).GetFromLeft(abControlsWidth).GetVShifted(titleHeight);
    const auto abToggleArea = abControlsArea.GetFromLeft(40.0f);
    const auto abMixArea = abControlsArea.GetFromRight(abControlsArea.W() - 45.0f);

    // 添加模式切换开关 - 使用SVGSwitchControl替换TextButtonControl
    pGraphics->AttachControl(new ISVGSwitchControl(modeToggleArea, [this](IControl* pCaller) {
      const int currentMode = GetParam(kProcessingMode)->Int();
      GetParam(kProcessingMode)->Set(currentMode == 0 ? 1 : 0); // 切换模式
    }, 
    {pGraphics->LoadSVG(SLIDESWITCHRECT_FN), pGraphics->LoadSVG(SLIDESWITCHRECT_FN)}));
    
    // 添加模式文本标签
    pGraphics->AttachControl(new ITextControl(modeToggleArea.GetVShifted(-15), "处理模式", IText(15)));
    pGraphics->AttachControl(new ITextControl(modeToggleArea, [this](IControl* pCaller) {
      const int mode = GetParam(kProcessingMode)->Int();
      return mode == 0 ? "吉他模式" : "人声模式";
    }, IText(14)));
    
    // 添加A/B切换按钮 - 使用SVGSwitchControl
    pGraphics->AttachControl(new ISVGSwitchControl(abToggleArea, kABToggle, 
      {pGraphics->LoadSVG(SLIDESWITCHRECT_FN), pGraphics->LoadSVG(SLIDESWITCHRECT_FN)}));
    
    // 添加A/B标签
    pGraphics->AttachControl(new ITextControl(abToggleArea.GetVShifted(-15), "A/B槽位", IText(15)));
    pGraphics->AttachControl(new ITextControl(abToggleArea, [this](IControl* pCaller) {
      const int slot = GetParam(kABToggle)->Int();
      return slot == 0 ? "槽位 A" : "槽位 B";
    }, IText(14)));
    
    // 添加A/B混合滑块
    pGraphics->AttachControl(new IVKnobControl(abMixArea, kABMix, "A/B 混合", DEFAULT_STYLE, true, true));

    // 修改模型和IR加载按钮使用我们的新函数
    pGraphics->AttachControl(new SVGButtonControl(modelIconArea, [&](IControl* pCaller) {
      _OpenModelFileChooser();
    }, pGraphics->LoadSVG(DSPICON_FN)));
    
    pGraphics->AttachControl(new SVGButtonControl(irSwitchArea, [&](IControl* pCaller) {
      _OpenIRFileChooser(); 
    }, pGraphics->LoadSVG(IRICON_FN)), kCtrlTagIRToggle);
  };
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  _DeallocateIOPointers();
}

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const size_t numChannelsExternalIn = (size_t)NInChansConnected();
  const size_t numChannelsExternalOut = (size_t)NOutChansConnected();
  const size_t numChannelsInternal = kNumChannelsInternal;
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();
  
  // 获取A/B混合比例
  const double abMix = GetParam(kABMix)->Value();
  const bool useABMixing = abMix > 0.0 && abMix < 1.0 && mModelA && mModelB;

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _PrepareBuffers(numChannelsInternal, numFrames);
  // 保留立体声信息
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);
  _ApplyDSPStaging();
  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();

  // 噪声门处理（对每个通道单独处理）
  sample** triggerOutputL = mInputPointers;
  sample** triggerOutputR = mInputPointers + 1;
  
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value();
    const double ratio = 0.1;
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    
    // 对左右声道分别处理
    triggerOutputL = mNoiseGateTrigger.Process(mInputPointers, 1, numFrames);
    triggerOutputR = mNoiseGateTrigger.Process(mInputPointers + 1, 1, numFrames);
  }

  // 为AB混合准备临时缓冲区
  std::vector<float> tempOutputL;
  std::vector<float> tempOutputR;
  std::vector<float> tempOutput2L;
  std::vector<float> tempOutput2R;
  
  if (useABMixing) {
    tempOutputL.resize(numFrames);
    tempOutputR.resize(numFrames);
    tempOutput2L.resize(numFrames);
    tempOutput2R.resize(numFrames);
  }
  
  // 标准模型处理（当前槽位或槽位A）
  if (mModel != nullptr)
  {
    if (useABMixing) {
      // 处理A槽位
      if (mModelA != nullptr) {
        mModelA->process(triggerOutputL[0], tempOutputL.data(), nFrames);
        mModelA->process(triggerOutputR[0], tempOutputR.data(), nFrames);
      } else {
        // 如果A槽位没有模型，直接传递
        for (size_t s = 0; s < numFrames; s++) {
          tempOutputL[s] = triggerOutputL[0][s];
          tempOutputR[s] = triggerOutputR[0][s];
        }
      }
      
      // 处理B槽位
      if (mModelB != nullptr) {
        mModelB->process(triggerOutputL[0], tempOutput2L.data(), nFrames);
        mModelB->process(triggerOutputR[0], tempOutput2R.data(), nFrames);
      } else {
        // 如果B槽位没有模型，直接传递
        for (size_t s = 0; s < numFrames; s++) {
          tempOutput2L[s] = triggerOutputL[0][s];
          tempOutput2R[s] = triggerOutputR[0][s];
        }
      }
      
      // 混合两个槽位的输出
      for (size_t s = 0; s < numFrames; s++) {
        mOutputArray[0][s] = tempOutputL[s] * (1.0 - abMix) + tempOutput2L[s] * abMix;
        mOutputArray[1][s] = tempOutputR[s] * (1.0 - abMix) + tempOutput2R[s] * abMix;
      }
    } else {
      // 正常处理（无混合）
      mModel->process(triggerOutputL[0], mOutputPointers[0], nFrames);
      mModel->process(triggerOutputR[0], mOutputPointers[1], nFrames);
    }
  }
  else
  {
    // 对左右声道分别应用备用DSP
    _FallbackDSP(&triggerOutputL[0], &mOutputPointers[0], 1, numFrames);
    _FallbackDSP(&triggerOutputR[0], &mOutputPointers[1], 1, numFrames);
  }

  // 处理噪声门和后续效果
  
  // 临时存储后处理前的输出
  sample** processingSignalL = &mOutputPointers[0];
  sample** processingSignalR = &mOutputPointers[1];
  
  // 对左右声道分别应用噪声门
  sample** gateGainOutputL = noiseGateActive ? mNoiseGateGain.Process(processingSignalL, 1, numFrames) : processingSignalL;
  sample** gateGainOutputR = noiseGateActive ? mNoiseGateGain.Process(processingSignalR, 1, numFrames) : processingSignalR;

  // 对左右声道分别应用音调控制
  sample** toneStackOutPointersL = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutputL, 1, numFrames)
                                    : gateGainOutputL;
  sample** toneStackOutPointersR = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutputR, 1, numFrames)
                                    : gateGainOutputR;

  // 对左右声道分别应用IR
  sample** irPointersL = toneStackOutPointersL;
  sample** irPointersR = toneStackOutPointersR;
  if (mIR != nullptr && GetParam(kIRToggle)->Value())
  {
    irPointersL = mIR->Process(toneStackOutPointersL, 1, numFrames);
    irPointersR = mIR->Process(toneStackOutPointersR, 1, numFrames);
  }

  // 对左右声道分别应用高通滤波器
  const double highPassCutoffFreq = kDCBlockerFrequency;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  sample** hpfPointersL = mHighPass.Process(irPointersL, 1, numFrames);
  sample** hpfPointersR = mHighPass.Process(irPointersR, 1, numFrames);

  // 还原左右声道的处理结果到输出缓冲区
  if (!useABMixing) { // 如果使用了AB混合，上面已经设置了mOutputArray
    for (size_t s = 0; s < numFrames; s++)
    {
      mOutputArray[0][s] = hpfPointersL[0][s];
      mOutputArray[1][s] = hpfPointersR[0][s];
    }
  } else {
    // 应用后处理效果到混合后的信号
    for (size_t s = 0; s < numFrames; s++)
    {
      // 应用噪声门增益
      if (noiseGateActive) {
        mOutputArray[0][s] *= gateGainOutputL[0][s] / mOutputPointers[0][s];
        mOutputArray[1][s] *= gateGainOutputR[0][s] / mOutputPointers[1][s];
      }
      
      // 此处可以应用其他后处理效果
      // ...
    }
  }

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(mOutputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  _UpdateMeters(mInputPointers, mOutputPointers, numFrames, numChannelsInternal, numChannelsInternal);
}

void NeuralAmpModeler::OnReset()
{
  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = GetBlockSize();

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  SetTailSize(tailCycles * (int)(sampleRate / kDCBlockerFrequency));
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);
  // If there is a model or IR loaded, they need to be checked for resampling.
  _ResetModelAndIR(sampleRate, GetBlockSize());
  mToneStack->Reset(sampleRate, maxBlockSize);
  _UpdateLatency();
}

void NeuralAmpModeler::OnIdle()
{
  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  if (mNewModelLoadedInDSP)
  {
    if (auto* pGraphics = GetUI())
    {
      _UpdateControlsFromModel();
      mNewModelLoadedInDSP = false;
    }
  }
  if (mModelCleared)
  {
    if (auto* pGraphics = GetUI())
    {
      // FIXME -- need to disable only the "normalized" model
      // pGraphics->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->ClearModelInfo();
      mModelCleared = false;
    }
  }
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralAmpModeler###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model directory (don't serialize the model itself; we'll just load it again
  // when we unserialize)
  chunk.PutStr(mNAMPath.Get());
  chunk.PutStr(mIRPath.Get());
  return SerializeParams(chunk);
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // Look for the expected header. If it's there, then we'll know what to do.
  WDL_String header;
  int pos = startPos;
  pos = chunk.GetStr(header, pos);

  const char* kExpectedHeader = "###NeuralAmpModeler###";
  if (strcmp(header.Get(), kExpectedHeader) == 0)
  {
    return _UnserializeStateWithKnownVersion(chunk, pos);
  }
  else
  {
    return _UnserializeStateWithUnknownVersion(chunk, startPos);
  }
}

void NeuralAmpModeler::OnUIOpen()
{
  Plugin::OnUIOpen();

  if (mNAMPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
    // If it's not loaded yet, then mark as failed.
    // If it's yet to be loaded, then the completion handler will set us straight once it runs.
    if (mModel == nullptr && mStagedModel == nullptr)
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);
  }

  if (mIRPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
    if (mIR == nullptr && mStagedIR == nullptr)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  if (mModel != nullptr)
  {
    _UpdateControlsFromModel();
  }
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    // Changes to the input gain
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kInputLevel: _SetInputGain(); break;
    // Changes to the output gain
    case kOutputLevel:
    case kOutputMode: _SetOutputGain(); break;
    // Tone stack:
    case kToneBass: mToneStack->SetParam("bass", GetParam(paramIdx)->Value()); break;
    case kToneMid: mToneStack->SetParam("middle", GetParam(paramIdx)->Value()); break;
    case kToneTreble: mToneStack->SetParam("treble", GetParam(paramIdx)->Value()); break;
    case kProcessingMode:
    {
      const int modeIdx = GetParam(kProcessingMode)->Int();
      ProcessingMode newMode = static_cast<ProcessingMode>(modeIdx);
      _UpdateParamsForMode(newMode);
      break;
    }
    case kABToggle:
    {
      const bool useSlotB = GetParam(kABToggle)->Int() == 1;
      _SwitchABSlot(useSlotB);
      break;
    }
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();

    switch (paramIdx)
    {
      case kNoiseGateActive: pGraphics->GetControlWithParamIdx(kNoiseGateThreshold)->SetDisabled(!active); break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active); break;
      default: break;
    }
  }
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel: mShouldRemoveModel = true; return true;
    case kMsgTagClearIR: mShouldRemoveIR = true; return true;
    case kMsgTagHighlightColor:
    {
      mHighLightColor.Set((const char*)pData);

      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());

            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }

      return true;
    }
    default: return false;
  }
}

// Private methods ============================================================

void NeuralAmpModeler::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_ApplyDSPStaging()
{
  // Remove marked modules
  if (mShouldRemoveModel)
  {
    mModel = nullptr;
    mNAMPath.Set("");
    mShouldRemoveModel = false;
    mModelCleared = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mShouldRemoveIR)
  {
    mIR = nullptr;
    mIRPath.Set("");
    mShouldRemoveIR = false;
  }
  // Move things from staged to live
  if (mStagedModel != nullptr)
  {
    mModel = std::move(mStagedModel);
    mStagedModel = nullptr;
    mNewModelLoadedInDSP = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mStagedIR != nullptr)
  {
    mIR = std::move(mStagedIR);
    mStagedIR = nullptr;
  }
}

void NeuralAmpModeler::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mInputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      mOutputArray[c][s] = mInputArray[c][s];
}

void NeuralAmpModeler::_ResetModelAndIR(const double sampleRate, const int maxBlockSize)
{
  // Model
  if (mStagedModel != nullptr)
  {
    mStagedModel->Reset(sampleRate, maxBlockSize);
  }
  else if (mModel != nullptr)
  {
    mModel->Reset(sampleRate, maxBlockSize);
  }

  // IR
  if (mStagedIR != nullptr)
  {
    const double irSampleRate = mStagedIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mStagedIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
  else if (mIR != nullptr)
  {
    const double irSampleRate = mIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
}

void NeuralAmpModeler::_SetInputGain()
{
  iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
  // Input calibration
  if ((mModel != nullptr) && (mModel->HasInputLevel()) && GetParam(kCalibrateInput)->Bool())
  {
    inputGainDB += GetParam(kInputCalibrationLevel)->Value() - mModel->GetInputLevel();
  }
  mInputGain = DBToAmp(inputGainDB);
}

void NeuralAmpModeler::_SetOutputGain()
{
  double gainDB = GetParam(kOutputLevel)->Value();
  if (mModel != nullptr)
  {
    const int outputMode = GetParam(kOutputMode)->Int();
    switch (outputMode)
    {
      case 1: // Normalized
        if (mModel->HasLoudness())
        {
          const double loudness = mModel->GetLoudness();
          const double targetLoudness = -18.0;
          gainDB += (targetLoudness - loudness);
        }
        break;
      case 2: // Calibrated
        if (mModel->HasOutputLevel())
        {
          const double inputLevel = GetParam(kInputCalibrationLevel)->Value();
          const double outputLevel = mModel->GetOutputLevel();
          gainDB += (outputLevel - inputLevel);
        }
        break;
      case 0: // Raw
      default: break;
    }
  }
  mOutputGain = DBToAmp(gainDB);
}

std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath)
{
  WDL_String previousNAMPath = mNAMPath;
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());
    std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);
    std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate());
    temp->Reset(GetSampleRate(), GetBlockSize());
    mStagedModel = std::move(temp);
    mNAMPath = modelPath;
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
  }
  catch (std::runtime_error& e)
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);

    if (mStagedModel != nullptr)
    {
      mStagedModel = nullptr;
    }
    mNAMPath = previousNAMPath;
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralAmpModeler::_StageIR(const WDL_String& irPath)
{
  // FIXME it'd be better for the path to be "staged" as well. Just in case the
  // path and the model got caught on opposite sides of the fence...
  WDL_String previousIRPath = mIRPath;
  const double sampleRate = GetSampleRate();
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  try
  {
    auto irPathU8 = std::filesystem::u8path(irPath.Get());
    mStagedIR = std::make_unique<dsp::ImpulseResponse>(irPathU8.string().c_str(), sampleRate);
    wavState = mStagedIR->GetWavState();
  }
  catch (std::runtime_error& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
  {
    mIRPath = irPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
  }
  else
  {
    if (mStagedIR != nullptr)
    {
      mStagedIR = nullptr;
    }
    mIRPath = previousIRPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  return wavState;
}

size_t NeuralAmpModeler::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralAmpModeler::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralAmpModeler::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}
void NeuralAmpModeler::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
}

void NeuralAmpModeler::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralAmpModeler::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  // 支持立体声处理
  if (nChansOut != 2)
  {
    std::stringstream ss;
    ss << "Expected stereo output, but " << nChansOut << " output channels are requested!";
    throw std::runtime_error(ss.str());
  }

  double gain = mInputGain;
#ifndef APP_API
  gain /= (float)nChansIn;
#endif
  // 保留立体声信息，不再将输入混合为单声道
  // 假设_PrepareBuffers()已经被调用
  for (size_t c = 0; c < nChansIn && c < nChansOut; c++)
  {
    for (size_t s = 0; s < nFrames; s++)
    {
      mInputArray[c][s] = gain * inputs[c][s];
    }
  }
  
  // 如果输入是单声道但输出需要立体声，则复制到两个通道
  if (nChansIn == 1 && nChansOut == 2)
  {
    for (size_t s = 0; s < nFrames; s++)
    {
      mInputArray[1][s] = mInputArray[0][s];
    }
  }
}

void NeuralAmpModeler::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  // 支持立体声输出
  const double gain = mOutputGain;
  
  // 处理所有可用的输出通道
  for (size_t c = 0; c < nChansOut && c < nChansIn; c++)
  {
    for (size_t s = 0; s < nFrames; s++)
    {
#ifdef APP_API // Ensure valid output to interface
      outputs[c][s] = std::clamp(gain * inputs[c][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large values.
      outputs[c][s] = gain * inputs[c][s];
#endif
    }
  }
  
  // 如果输入是单声道但输出需要多通道，则复制到所有通道
  if (nChansIn == 1 && nChansOut > 1)
  {
    for (size_t c = 1; c < nChansOut; c++)
    {
      for (size_t s = 0; s < nFrames; s++)
      {
        outputs[c][s] = outputs[0][s];
      }
    }
  }
}

void NeuralAmpModeler::_UpdateControlsFromModel()
{
  if (mModel == nullptr)
  {
    return;
  }
  if (auto* pGraphics = GetUI())
  {
    ModelInfo modelInfo;
    modelInfo.sampleRate.known = true;
    modelInfo.sampleRate.value = mModel->GetEncapsulatedSampleRate();
    modelInfo.inputCalibrationLevel.known = mModel->HasInputLevel();
    modelInfo.inputCalibrationLevel.value = mModel->HasInputLevel() ? mModel->GetInputLevel() : 0.0;
    modelInfo.outputCalibrationLevel.known = mModel->HasOutputLevel();
    modelInfo.outputCalibrationLevel.value = mModel->HasOutputLevel() ? mModel->GetOutputLevel() : 0.0;

    static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->SetModelInfo(modelInfo);

    const bool disableInputCalibrationControls = !mModel->HasInputLevel();
    pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetDisabled(disableInputCalibrationControls);
    pGraphics->GetControlWithTag(kCtrlTagInputCalibrationLevel)->SetDisabled(disableInputCalibrationControls);
    {
      auto* c = static_cast<OutputModeControl*>(pGraphics->GetControlWithTag(kCtrlTagOutputMode));
      c->SetNormalizedDisable(!mModel->HasLoudness());
      c->SetCalibratedDisable(!mModel->HasOutputLevel());
    }
  }
}

void NeuralAmpModeler::_UpdateLatency()
{
  int latency = 0;
  if (mModel)
  {
    latency += mModel->GetLatency();
  }
  // Other things that add latency here...

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralAmpModeler::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  // 支持立体声电平表
  // 对于立体声，我们可以选择显示两个通道的平均值或最大值
  // 这里我们选择显示最大值
  
  // 创建临时缓冲区来存储合并后的信号
  std::vector<sample> inputMerged(nFrames);
  std::vector<sample> outputMerged(nFrames);
  
  for (size_t s = 0; s < nFrames; s++)
  {
    // 取左右声道的最大值
    inputMerged[s] = std::max(std::abs(inputPointer[0][s]), std::abs(inputPointer[1][s]));
    outputMerged[s] = std::max(std::abs(outputPointer[0][s]), std::abs(outputPointer[1][s]));
  }
  
  // 使用合并后的信号更新电平表
  const int nChansHack = 1; // 仍然使用单通道电平表
  sample* inputMergedPtr = inputMerged.data();
  sample* outputMergedPtr = outputMerged.data();
  mInputSender.ProcessBlock(&inputMergedPtr, (int)nFrames, kCtrlTagInputMeter, nChansHack);
  mOutputSender.ProcessBlock(&outputMergedPtr, (int)nFrames, kCtrlTagOutputMeter, nChansHack);
}

// HACK
#include "Unserialization.cpp"

// 实现模式切换功能
void NeuralAmpModeler::_UpdateParamsForMode(ProcessingMode mode)
{
  mCurrentMode = mode;
  
  // 根据模式更新参数范围和默认值
  if (mode == ProcessingMode::VOCAL)
  {
    // 人声模式使用扩展的输入/输出范围
    GetParam(kInputLevel)->SetBounds(-30.0, 30.0);
    GetParam(kOutputLevel)->SetBounds(-40.0, 40.0);
    
    // 噪声门阈值调整为更适合人声的范围
    GetParam(kNoiseGateThreshold)->SetBounds(-100.0, -40.0);
    
    // 对EQ进行小幅调整以适应人声
    if (mToneStack) {
      mToneStack->SetBassFreq(100.0);   // 适合人声的低频
      mToneStack->SetMidFreq(1000.0);   // 适合人声的中频
      mToneStack->SetTrebleFreq(5000.0); // 适合人声的高频
    }
  }
  else // Guitar模式
  {
    // 恢复吉他模式的参数范围
    GetParam(kInputLevel)->SetBounds(-20.0, 20.0);
    GetParam(kOutputLevel)->SetBounds(-40.0, 40.0);
    
    // 恢复吉他模式的噪声门阈值范围
    GetParam(kNoiseGateThreshold)->SetBounds(-100.0, 0.0);
    
    // 恢复吉他模式的EQ设置
    if (mToneStack) {
      mToneStack->SetBassFreq(82.0);    // 默认吉他低频
      mToneStack->SetMidFreq(500.0);    // 默认吉他中频
      mToneStack->SetTrebleFreq(2000.0); // 默认吉他高频
    }
  }
  
  // 更新UI，如果界面已经加载
  if (GetUI()) {
    GetUI()->SetAllControlsDirty();
  }
}

// 实现A/B槽位切换
void NeuralAmpModeler::_SwitchABSlot(bool useSlotB)
{
  mUsingSlotB = useSlotB;
  
  // 切换模型和IR
  if (useSlotB) {
    // 从B槽位加载
    mModel = std::move(mModelB);
    mIR = std::move(mIRB);
  } else {
    // 从A槽位加载
    mModel = std::move(mModelA);
    mIR = std::move(mIRA);
  }
  
  // 更新UI
  if (GetUI()) {
    GetUI()->SetAllControlsDirty();
  }
}

bool NeuralAmpModeler::LoadModel(const std::string& path)
{
  if (path.empty())
  {
    mModelPath = "";
    mModel.reset(nullptr);
    
    // 根据当前槽位存储
    if (mUsingSlotB) {
      mModelPathB = "";
      mModelB.reset(nullptr);
    } else {
      mModelPathA = "";
      mModelA.reset(nullptr);
    }
    
    return true;
  }
  
  try
  {
    auto model = nam::get_dsp(path);
    
    // 存储模型和路径
    if (mUsingSlotB) {
      mModelPathB = path;
      mModelB = std::move(model);
      mModel = std::move(mModelB);
    } else {
      mModelPathA = path;
      mModelA = std::move(model);
      mModel = std::move(mModelA);
    }
    
    // 如果是B槽位，需要复制回原始指针
    if (mUsingSlotB) {
      mModelB = nam::get_dsp(path); // 重新创建一个B槽位的模型
    } else {
      mModelA = nam::get_dsp(path); // 重新创建一个A槽位的模型
    }
    
    mModelPath = path;
    
    _UpdateControlsFromModel();
    _UpdateLatency();
    
    return true;
  }
  catch (const std::exception& e)
  {
    mModelPath = "";
    mModel.reset(nullptr);
    
    // 根据当前槽位清除
    if (mUsingSlotB) {
      mModelPathB = "";
      mModelB.reset(nullptr);
    } else {
      mModelPathA = "";
      mModelA.reset(nullptr);
    }
    
    if (GetUI() != nullptr)
    {
      std::stringstream ss;
      ss << "Failed to load model: " << e.what();
      _ShowMessageBox(GetUI(), ss.str().c_str(), "Error", EMsgBoxType::kMB_OK);
    }
    
    return false;
  }
}

bool NeuralAmpModeler::LoadIR(const std::string& path)
{
  if (path.empty())
  {
    // 清除IR
    if (mUsingSlotB) {
      mIRPathB = "";
      mIRB.reset(nullptr);
    } else {
      mIRPathA = "";
      mIRA.reset(nullptr);
    }
    
    mIRPath = "";
    mIR.reset(nullptr);
    return true;
  }
  
  try
  {
    auto irData = dsp::wav::read<kNumChannelsInternal>(path);
    auto ir = std::make_unique<dsp::IR>(irData, GetSampleRate(), dsp::IR::Mode::kZeroPhase);
    
    // 存储IR和路径
    if (mUsingSlotB) {
      mIRPathB = path;
      mIRB = std::move(ir);
      mIR = std::move(mIRB);
    } else {
      mIRPathA = path;
      mIRA = std::move(ir);
      mIR = std::move(mIRA);
    }
    
    // 如果是B槽位，需要复制回原始指针
    if (mUsingSlotB) {
      mIRB = std::make_unique<dsp::IR>(dsp::wav::read<kNumChannelsInternal>(path), GetSampleRate(), dsp::IR::Mode::kZeroPhase);
    } else {
      mIRA = std::make_unique<dsp::IR>(dsp::wav::read<kNumChannelsInternal>(path), GetSampleRate(), dsp::IR::Mode::kZeroPhase);
    }
    
    mIRPath = path;
    return true;
  }
  catch (const std::exception& e)
  {
    // 清除IR
    if (mUsingSlotB) {
      mIRPathB = "";
      mIRB.reset(nullptr);
    } else {
      mIRPathA = "";
      mIRA.reset(nullptr);
    }
    
    mIRPath = "";
    mIR.reset(nullptr);
    
    if (GetUI() != nullptr)
    {
      std::stringstream ss;
      ss << "Failed to load IR: " << e.what();
      _ShowMessageBox(GetUI(), ss.str().c_str(), "Error", EMsgBoxType::kMB_OK);
    }
    
    return false;
  }
}

// 修改模型加载界面函数
void NeuralAmpModeler::_OpenModelFileChooser()
{
  if (GetUI() == nullptr) return;
  
  const std::string fileChooserStartPath = mCurrentMode == ProcessingMode::VOCAL 
                                         ? "Vocal Models" // 人声模型文件夹
                                         : "Models";      // 吉他模型文件夹
  
  using namespace iplug;
  WDL_String dir;
  GetUI()->PromptForDirectory(dir, fileChooserStartPath.c_str(), "Choose model folder...");
  if (dir.GetLength()) {
    WDL_String fileName;
    GetUI()->PromptForFile(fileName, EFileAction::kFileOpen, dir.Get(), "nam", "Choose NAM model...");
    if (fileName.GetLength()) {
      const std::string msg = _StageModel(fileName.Get());
      if (msg.size()) {
        std::stringstream ss;
        ss << "Failed to load NAM model. Message:\n\n" << msg;
        _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
      }
      std::cout << "Loaded: " << fileName.Get() << std::endl;
    }
  }
}

// 修改IR加载界面函数
void NeuralAmpModeler::_OpenIRFileChooser()
{
  if (GetUI() == nullptr) return;
  
  // 根据当前模式设置不同的默认IR文件夹
  const std::string fileChooserStartPath = mCurrentMode == ProcessingMode::VOCAL 
                                         ? "Vocal IRs" // 人声IR文件夹
                                         : "IRs";      // 吉他IR文件夹
  
  using namespace iplug;
  WDL_String dir;
  GetUI()->PromptForDirectory(dir, fileChooserStartPath.c_str(), "Choose IR folder...");
  if (dir.GetLength()) {
    WDL_String fileName;
    GetUI()->PromptForFile(fileName, EFileAction::kFileOpen, dir.Get(), "wav", "Choose IR wav file...");
    if (fileName.GetLength()) {
      mIRPath = fileName.Get();
      const dsp::wav::LoadReturnCode retCode = _StageIR(fileName.Get());
      if (retCode != dsp::wav::LoadReturnCode::SUCCESS) {
        std::stringstream message;
        message << "Failed to load IR file " << fileName.Get() << ":\n";
        message << dsp::wav::GetMsgForLoadReturnCode(retCode);
        _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
      }
    }
  }
}
