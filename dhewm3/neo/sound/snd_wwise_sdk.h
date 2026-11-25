/*
===========================================================================

dhewm3 Wwise backend: plain interface to the Wwise SDK.

No Wwise types are visible here. The implementation (snd_wwise_sdk.cpp) is
compiled only when WWISE_ROOT is set.

===========================================================================
*/

#ifndef __SND_WWISE_SDK_H__
#define __SND_WWISE_SDK_H__

namespace WwiseSDK {
	// Starts memory manager, stream manager (SDK default file I/O) and sound engine.
	// On failure returns false, with failedStage (1 memory, 2 stream, 3 I/O, 4 engine)
	// and akResult set to the SDK result code.
	bool	Start( int * failedStage, int * akResult );
	// Stops everything Start() started, in reverse order.
	void	Stop();
	// Version of the SDK the game was built against.
	void	Version( int * major, int * minor, int * subminor, int * build );
}

#endif
