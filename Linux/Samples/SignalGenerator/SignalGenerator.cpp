/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
** 
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/
//
//  SignalGenerator.cpp
//  Signal Generator
//

#include "SignalGenerator.h"

#include <math.h>
#include <stdio.h>

const uint32_t		kAudioWaterlevel = 48000;

// SD 75% Colour Bars
static uint32_t gSD75pcColourBars[8] =
{
	0xeb80eb80, 0xa28ea22c, 0x832c839c, 0x703a7048,
	0x54c654b8, 0x41d44164, 0x237223d4, 0x10801080
};

// HD 75% Colour Bars
static uint32_t gHD75pcColourBars[8] =
{
	0xeb80eb80, 0xa888a82c, 0x912c9193, 0x8534853f,
	0x3fcc3fc1, 0x33d4336d, 0x1c781cd4, 0x10801080
};

class CDeckLinkGLWidget : public QGLWidget, public IDeckLinkScreenPreviewCallback
{
private:
	QAtomicInt refCount;
	QMutex mutex;
	IDeckLinkOutput* deckLinkOutput;
	IDeckLinkGLScreenPreviewHelper* deckLinkScreenPreviewHelper;
	
public:
	CDeckLinkGLWidget(QWidget* parent);
	
	// IUnknown
	virtual HRESULT QueryInterface(REFIID iid, LPVOID *ppv);
	virtual ULONG AddRef();
	virtual ULONG Release();

	// IDeckLinkScreenPreviewCallback
	virtual HRESULT DrawFrame(IDeckLinkVideoFrame* theFrame);
	
protected:
	void initializeGL();
	void paintGL();
	void resizeGL(int width, int height);
};

CDeckLinkGLWidget::CDeckLinkGLWidget(QWidget* parent) : QGLWidget(parent)
{
	refCount = 1;
	
	deckLinkOutput = deckLinkOutput;
	deckLinkScreenPreviewHelper = CreateOpenGLScreenPreviewHelper();
}

void	CDeckLinkGLWidget::initializeGL ()
{
	if (deckLinkScreenPreviewHelper != NULL)
	{
		mutex.lock();
			deckLinkScreenPreviewHelper->InitializeGL();
		mutex.unlock();
	}
}

void	CDeckLinkGLWidget::paintGL ()
{
	mutex.lock();
		glLoadIdentity();

		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		deckLinkScreenPreviewHelper->PaintGL();
	mutex.unlock();
}

void	CDeckLinkGLWidget::resizeGL (int width, int height)
{
	mutex.lock();
		glViewport(0, 0, width, height);
	mutex.unlock();
}

HRESULT		CDeckLinkGLWidget::QueryInterface (REFIID, LPVOID *ppv)
{
	*ppv = NULL;
	return E_NOINTERFACE;
}

ULONG		CDeckLinkGLWidget::AddRef ()
{
	int		oldValue;
	
	oldValue = refCount.fetchAndAddAcquire(1);
	return (ULONG)(oldValue + 1);
}

ULONG		CDeckLinkGLWidget::Release ()
{
	int		oldValue;
	
	oldValue = refCount.fetchAndAddAcquire(-1);
	if (oldValue == 1)
	{
		delete this;
	}
	
	return (ULONG)(oldValue - 1);
}

HRESULT		CDeckLinkGLWidget::DrawFrame (IDeckLinkVideoFrame* theFrame)
{
	if (deckLinkScreenPreviewHelper != NULL)
	{
		deckLinkScreenPreviewHelper->SetFrame(theFrame);
		update();
	}
	return S_OK;
}

SignalGenerator::SignalGenerator()
	: QDialog()
{
	running = false;
	deckLink = NULL;
	deckLinkOutput = NULL;
	videoFrameBlack = NULL;
	videoFrameBars = NULL;
	audioBuffer = NULL;
	timeCode = NULL;

	ui = new Ui::SignalGeneratorDialog();
	ui->setupUi(this);

	layout = new QGridLayout(ui->previewContainer);
	layout->setMargin(0);

	previewView = new CDeckLinkGLWidget(this);
	previewView->resize(ui->previewContainer->size());
	previewView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	layout->addWidget(previewView, 0, 0, 0, 0);
	previewView->DrawFrame(NULL);

	ui->outputSignalPopup->addItem("Pip", QVariant::fromValue((int)kOutputSignalPip));
	ui->outputSignalPopup->addItem("Dropout", QVariant::fromValue((int)kOutputSignalDrop));
	ui->audioChannelPopup->addItem("2", QVariant::fromValue(2));
	ui->audioChannelPopup->addItem("8", QVariant::fromValue(8));
	ui->audioChannelPopup->addItem("16", QVariant::fromValue(16));
	ui->audioSampleDepthPopup->addItem("16", QVariant::fromValue(16));
	ui->audioSampleDepthPopup->addItem("32", QVariant::fromValue(32));

	connect(ui->startButton, SIGNAL(clicked()), this, SLOT(toggleStart()));
	enableInterface(false);
	show();
}

SignalGenerator::~SignalGenerator()
{
	if (running)
		stopRunning();

	if (deckLinkOutput)
	{
		deckLinkOutput->Release();
		deckLinkOutput = NULL;
	}
	if (deckLink)
	{
		deckLink->Release();
		deckLink = NULL;
	}
	if (playerDelegate)
	{
		playerDelegate->Release();
		playerDelegate = NULL;
	}
	delete timeCode;
}

void SignalGenerator::setup()
{
	IDeckLinkIterator*					deckLinkIterator = NULL;
	IDeckLinkDisplayModeIterator*		displayModeIterator = NULL;
	IDeckLinkDisplayMode*				deckLinkDisplayMode = NULL;
	bool								success = false;
	
	// **** Find a DeckLink instance and obtain video output interface
	deckLinkIterator = CreateDeckLinkIteratorInstance();
	if (deckLinkIterator == NULL)
	{
		QMessageBox::critical(this, "This application requires the DeckLink drivers installed.", "Please install the Blackmagic DeckLink drivers to use the features of this application.");
		goto bail;
	}
	
	// Connect to the first DeckLink instance
	if (deckLinkIterator->Next(&deckLink) != S_OK)
	{
		QMessageBox::critical(this, "This application requires a DeckLink PCI card.", "You will not be able to use the features of this application until a DeckLink PCI card is installed.");
		goto bail;
	}
	
	// Obtain the audio/video output interface (IDeckLinkOutput)
	if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&deckLinkOutput) != S_OK)
		goto bail;
	
	// Create a delegate class to allow the DeckLink API to call into our code
	playerDelegate = new PlaybackDelegate(this, deckLinkOutput);
	if (playerDelegate == NULL)
		goto bail;
	// Provide the delegate to the audio and video output interfaces
	deckLinkOutput->SetScheduledFrameCompletionCallback(playerDelegate);
	deckLinkOutput->SetAudioCallback(playerDelegate);
	
	
	// Populate the display mode menu with a list of display modes supported by the installed DeckLink card
	ui->videoFormatPopup->clear();
	if (deckLinkOutput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
		goto bail;

	while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK)
	{
		char*		modeName;
		
		if (deckLinkDisplayMode->GetName(const_cast<const char**>(&modeName)) == S_OK)
		{
			ui->videoFormatPopup->addItem(modeName, QVariant::fromValue((unsigned int)deckLinkDisplayMode->GetDisplayMode()));

			free(modeName);
		}

		deckLinkDisplayMode->Release();
		deckLinkDisplayMode = NULL;
	}
	enableInterface(true);
	deckLinkOutput->SetScreenPreviewCallback(previewView);
	
	success = true;
	
bail:
	if (success == false)
	{
		// Release any resources that were partially allocated
		if (deckLinkOutput != NULL)
		{
			deckLinkOutput->Release();
			deckLinkOutput = NULL;
		}
		//
		if (deckLink != NULL)
		{
			deckLink->Release();
			deckLink = NULL;
		}
		if (playerDelegate != NULL)
		{
			playerDelegate->Release();
			playerDelegate = NULL;
		}

		// Disable the user interface if we could not succsssfully connect to a DeckLink device
		ui->startButton->setEnabled(false);
		enableInterface(false);
	}
	
	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();
	if (displayModeIterator != NULL)
		displayModeIterator->Release();
}

void SignalGenerator::closeEvent(QCloseEvent *)
{
	if (running)
		stopRunning();
}

void SignalGenerator::enableInterface(bool enable)
{
	// Set the enable state of user interface elements
	ui->groupBox->setEnabled(enable);
}

void SignalGenerator::toggleStart()
{
	if (running == false)
		startRunning();
	else
		stopRunning();
}

void SignalGenerator::startRunning()
{
	bool					success = false;
	BMDDisplayModeSupport	supported = bmdDisplayModeNotSupported;
	BMDDisplayMode			displayMode = bmdModeUnknown;
	IDeckLinkDisplayMode*	videoDisplayMode = NULL;
	BMDVideoOutputFlags		videoOutputFlags = 0;
	QVariant v;
	// Determine the audio and video properties for the output stream
	v = ui->outputSignalPopup->itemData(ui->outputSignalPopup->currentIndex());
	outputSignal = (OutputSignal)v.value<int>();
	
	v = ui->audioChannelPopup->itemData(ui->audioChannelPopup->currentIndex());
	audioChannelCount = v.value<int>();
	
	v = ui->audioSampleDepthPopup->itemData(ui->audioSampleDepthPopup->currentIndex());
	audioSampleDepth = v.value<int>();
	audioSampleRate = bmdAudioSampleRate48kHz;
	
	// - Extract the BMDDisplayMode from the display mode popup menu (stashed in the item's tag)
	v = ui->videoFormatPopup->itemData(ui->videoFormatPopup->currentIndex());
	displayMode = (BMDDisplayMode)v.value<unsigned int>();

	// - Use DoesSupportVideoMode to obtain an IDeckLinkDisplayMode instance representing the selected BMDDisplayMode
	if(deckLinkOutput->DoesSupportVideoMode(displayMode, bmdFormat8BitYUV, bmdFrameFlagDefault, &supported, &videoDisplayMode) != S_OK || supported == bmdDisplayModeNotSupported)
		goto bail;
	
	frameWidth = videoDisplayMode->GetWidth();
	frameHeight = videoDisplayMode->GetHeight();
	
	videoDisplayMode->GetFrameRate(&frameDuration, &frameTimescale);
	// Calculate the number of frames per second, rounded up to the nearest integer.  For example, for NTSC (29.97 FPS), framesPerSecond == 30.
	framesPerSecond = (frameTimescale + (frameDuration-1))  /  frameDuration;
	
	if (videoDisplayMode->GetDisplayMode() == bmdModeNTSC ||
			videoDisplayMode->GetDisplayMode() == bmdModeNTSC2398 ||
			videoDisplayMode->GetDisplayMode() == bmdModePAL)
	{
		timeCodeFormat = bmdTimecodeVITC;
		videoOutputFlags |= bmdVideoOutputVITC;
	}
	else
	{
		timeCodeFormat = bmdTimecodeRP188Any;
		videoOutputFlags |= bmdVideoOutputRP188;
	}

	if (timeCode)
		delete timeCode;
	timeCode = new Timecode(framesPerSecond);


	// Set the video output mode
	if (deckLinkOutput->EnableVideoOutput(videoDisplayMode->GetDisplayMode(), videoOutputFlags) != S_OK)
		goto bail;
	
	// Set the audio output mode
	if (deckLinkOutput->EnableAudioOutput(bmdAudioSampleRate48kHz, audioSampleDepth, audioChannelCount, bmdAudioOutputStreamTimestamped) != S_OK)
		goto bail;
	
	
	// Generate one second of audio tone
	audioSamplesPerFrame = ((audioSampleRate * frameDuration) / frameTimescale);
	audioBufferSampleLength = (framesPerSecond * audioSampleRate * frameDuration) / frameTimescale;
	audioBuffer = malloc(audioBufferSampleLength * audioChannelCount * (audioSampleDepth / 8));
	if (audioBuffer == NULL)
		goto bail;
	FillSine(audioBuffer, audioBufferSampleLength, audioChannelCount, audioSampleDepth);
	
	// Generate a frame of black
	if (deckLinkOutput->CreateVideoFrame(frameWidth, frameHeight, frameWidth*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &videoFrameBlack) != S_OK)
		goto bail;
	FillBlack(videoFrameBlack);
	
	// Generate a frame of colour bars
	if (deckLinkOutput->CreateVideoFrame(frameWidth, frameHeight, frameWidth*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &videoFrameBars) != S_OK)
		goto bail;
	FillColourBars(videoFrameBars);
	
	// Begin video preroll by scheduling a second of frames in hardware
	totalFramesScheduled = 0;
	for (unsigned int i = 0; i < framesPerSecond; i++)
		scheduleNextFrame(true);
	
	// Begin audio preroll.  This will begin calling our audio callback, which will start the DeckLink output stream.
	totalAudioSecondsScheduled = 0;
	if (deckLinkOutput->BeginAudioPreroll() != S_OK)
		goto bail;
	
	// Success; update the UI
	running = true;
	ui->startButton->setText("Stop");
	// Disable the user interface while running (prevent the user from making changes to the output signal)
	enableInterface(false);
	
	success = true;
	
bail:
	if(!success)
	{
		QMessageBox::critical(this, "Failed to start output", "Failed to start output");
		// *** Error-handling code.  Cleanup any resources that were allocated. *** //
		stopRunning();
	}

	if (videoDisplayMode != NULL)
		videoDisplayMode->Release();
}

void SignalGenerator::stopRunning()
{
	// Stop the audio and video output streams immediately
	deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
	//
	deckLinkOutput->DisableAudioOutput();
	deckLinkOutput->DisableVideoOutput();
	
	if (videoFrameBlack != NULL)
		videoFrameBlack->Release();
	videoFrameBlack = NULL;
	
	if (videoFrameBars != NULL)
		videoFrameBars->Release();
	videoFrameBars = NULL;
	
	if (audioBuffer != NULL)
		free(audioBuffer);
	audioBuffer = NULL;
	
	// Success; update the UI
	running = false;
	ui->startButton->setText("Start");
	enableInterface(true);
}


void SignalGenerator::scheduleNextFrame(bool prerolling)
{
	IDeckLinkMutableVideoFrame *currentFrame;
	if (prerolling == false)
	{
		// If not prerolling, make sure that playback is still active
		if (running == false)
			return;
	}
	
	if (outputSignal == kOutputSignalPip)
	{
		if ((totalFramesScheduled % framesPerSecond) == 0)
			currentFrame = videoFrameBars;
		else
			currentFrame = videoFrameBlack;
	}
	else
	{
		if ((totalFramesScheduled % framesPerSecond) == 0)
			currentFrame = videoFrameBlack;
		else
			currentFrame = videoFrameBars;
	}
	
	printf("frames: %d\n", timeCode->frames());
	currentFrame->SetTimecodeFromComponents(timeCodeFormat,
											timeCode->hours(),
											timeCode->minutes(),
											timeCode->seconds(),
											timeCode->frames(),
											bmdTimecodeFlagDefault);

	if (deckLinkOutput->ScheduleVideoFrame(currentFrame, (totalFramesScheduled * frameDuration), frameDuration, frameTimescale) != S_OK)
		goto out;
	
	totalFramesScheduled += 1;
out:	
	timeCode->update();
}

void SignalGenerator::writeNextAudioSamples()
{
	// Write one second of audio to the DeckLink API.
	
	if (outputSignal == kOutputSignalPip)
	{
		// Schedule one-frame of audio tone
		if (deckLinkOutput->ScheduleAudioSamples(audioBuffer, audioSamplesPerFrame, (totalAudioSecondsScheduled * audioBufferSampleLength), audioSampleRate, NULL) != S_OK)
			return;
	}
	else
	{
		// Schedule one-second (minus one frame) of audio tone
		if (deckLinkOutput->ScheduleAudioSamples(audioBuffer, (audioBufferSampleLength - audioSamplesPerFrame), (totalAudioSecondsScheduled * audioBufferSampleLength) + audioSamplesPerFrame, audioSampleRate, NULL) != S_OK)
			return;
	}
	
	totalAudioSecondsScheduled += 1;
}

/*****************************************/

PlaybackDelegate::PlaybackDelegate (SignalGenerator* owner, IDeckLinkOutput* deckLinkOutput)
{
	mRefCount = 1;
	mController = owner;
	mDeckLinkOutput = deckLinkOutput;
}

HRESULT		PlaybackDelegate::QueryInterface (REFIID, LPVOID *ppv)
{
	*ppv = NULL;
	return E_NOINTERFACE;
}

ULONG		PlaybackDelegate::AddRef ()
{
	int		oldValue;

	oldValue = mRefCount.fetchAndAddAcquire(1);
	return (ULONG)(oldValue + 1);
}

ULONG		PlaybackDelegate::Release ()
{
	int		oldValue;

	oldValue = mRefCount.fetchAndAddAcquire(-1);
	if (oldValue == 1)
	{
		delete this;
	}

	return (ULONG)(oldValue - 1);
}

HRESULT		PlaybackDelegate::ScheduledFrameCompleted (IDeckLinkVideoFrame*, BMDOutputFrameCompletionResult)
{
	// Schedule the next frame when a video frame has been completed
	mController->scheduleNextFrame(false);
	return S_OK;
}

HRESULT		PlaybackDelegate::ScheduledPlaybackHasStopped ()
{
	return S_OK;
}

HRESULT		PlaybackDelegate::RenderAudioSamples (bool preroll)
{
	// Provide further audio samples to the DeckLink API until our preferred buffer waterlevel is reached
	mController->writeNextAudioSamples();
	
	if (preroll)
	{
		// Start audio and video output
		mDeckLinkOutput->StartScheduledPlayback(0, 100, 1.0);
	}
	
	return S_OK;
}


/*****************************************/


void	FillSine (void* audioBuffer, uint32_t samplesToWrite, uint32_t channels, uint32_t sampleDepth)
{
	if (sampleDepth == 16)
	{
		int16_t*		nextBuffer;
		
		nextBuffer = (int16_t*)audioBuffer;
		for (uint32_t i = 0; i < samplesToWrite; i++)
		{
			int16_t		sample;
			
			sample = (int16_t)(24576.0 * sin((i * 2.0 * M_PI) / 48.0));
			for (uint32_t ch = 0; ch < channels; ch++)
				*(nextBuffer++) = sample;
		}
	}
	else if (sampleDepth == 32)
	{
		int32_t*		nextBuffer;
		
		nextBuffer = (int32_t*)audioBuffer;
		for (uint32_t i = 0; i < samplesToWrite; i++)
		{
			int32_t		sample;
			
			sample = (int32_t)(1610612736.0 * sin((i * 2.0 * M_PI) / 48.0));
			for (uint32_t ch = 0; ch < channels; ch++)
				*(nextBuffer++) = sample;
		}
	}
}

void	FillColourBars (IDeckLinkVideoFrame* theFrame)
{
	uint32_t*		nextWord;
	uint32_t		width;
	uint32_t		height;
	uint32_t*		bars;
	
	theFrame->GetBytes((void**)&nextWord);
	width = theFrame->GetWidth();
	height = theFrame->GetHeight();
	
	if (width > 720)
	{
		bars = gHD75pcColourBars;
	}
	else
	{
		bars = gSD75pcColourBars;
	}

	for (uint32_t y = 0; y < height; y++)
	{
		for (uint32_t x = 0; x < width; x+=2)
		{
			*(nextWord++) = bars[(x * 8) / width];
		}
	}
}

void	FillBlack (IDeckLinkVideoFrame* theFrame)
{
	uint32_t*		nextWord;
	uint32_t		width;
	uint32_t		height;
	uint32_t		wordsRemaining;
	
	theFrame->GetBytes((void**)&nextWord);
	width = theFrame->GetWidth();
	height = theFrame->GetHeight();
	
	wordsRemaining = (width*2 * height) / 4;
	
	while (wordsRemaining-- > 0)
		*(nextWord++) = 0x10801080;
}
