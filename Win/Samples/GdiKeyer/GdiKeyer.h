
#include "DeckLinkAPI_h.h"

// The callback class is used for video input format detection in this example.
class DeckLinkKeyerDelegate : public IDeckLinkInputCallback
{
private:
	ULONG		m_refCount;

	~DeckLinkKeyerDelegate();
	
public:
	DeckLinkKeyerDelegate();

	// IUnknown
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv);
	virtual ULONG STDMETHODCALLTYPE AddRef(void);
	virtual ULONG STDMETHODCALLTYPE  Release(void);

	// IDeckLinkInputCallback
	virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode*, BMDDetectedVideoInputFormatFlags);
	virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);
};
