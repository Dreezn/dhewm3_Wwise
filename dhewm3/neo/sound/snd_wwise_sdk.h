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
	// Receives Wwise monitor messages and errors (text, isError).
	typedef void (*LogFunc)( const char * text, bool isError );
	void	SetLogFunc( LogFunc func );
	// Starts memory manager, stream manager (SDK default file I/O) and sound engine.
	// On failure returns false, with failedStage (1 memory, 2 stream, 3 I/O, 4 engine, 5 bank load)
	// and akResult set to the SDK result code. outputDeviceID 0 = default output device. Stage 5 is loading Init.bnk.
	// Content banks are loaded afterwards with LoadBank (the provider reads the bank list).
	bool	Start( int * failedStage, int * akResult, unsigned int outputDeviceID );
	// Loads a bank file from base/wwise/Windows/ (for example "weapons.bnk"). Returns the SDK result code.
	int		LoadBank( const char * bankFile );
	// Posts a Wwise event on the debug game object. Returns false if the event could not be posted (akResult 1: invalid playing ID).
	bool	PostEvent( const char * eventName, unsigned int * playingID, int * akResult );
	// Processes the sound engine's command queue (AK::SoundEngine::RenderAudio).
	// The SDK requires this once per game frame; posted events do nothing without it.
	void	RenderAudio();
	// Writes the mixed Wwise output to a .wav file (debug). Returns false on failure.
	bool	StartCapture( const char * path );
	void	StopCapture();
	// Text list of the audio output devices. Returns the SDK result code.
	int		OutputDeviceReport( char * out, int outSize );

	// Game objects (one per dhewm3 sound emitter). IDs 1 to 3 are reserved (debug object, listener, at-listener object).
	// Positions are Wwise coordinates in meters: X right, Y up, Z forward (see snd_wwise_world.cpp).
	// front and top are unit vectors. Return the SDK result code.
	int		RegisterObject( unsigned long long objectID );
	int		UnregisterObject( unsigned long long objectID );
	int		SetObjectPosition( unsigned long long objectID, const float pos[3], const float front[3], const float top[3] );
	int		SetListenerPosition( const float pos[3], const float front[3], const float top[3] );
	// Posts an event on a game object. Returns the playing ID, or 0 if the post failed
	// (for example an event that is in no loaded bank; Wwise then logs its own warning).
	unsigned int PostEventOnObject( const char * eventName, unsigned long long objectID );
	// Playing IDs still active on a game object (no audio lock). Returns how many were written to ids.
	int		PlayingIDsOnObject( unsigned long long objectID, unsigned int * ids, int maxIDs );
	// Stops every sound on one game object, or one playing ID, or everything.
	void	StopObject( unsigned long long objectID );
	void	StopPlayingID( unsigned int playingID );
	void	StopAll();
	// Pauses or resumes one playing ID.
	void	PausePlayingID( unsigned int playingID );
	void	ResumePlayingID( unsigned int playingID );

	// Game object kept at the listener (ID 3): global sounds are posted on it (D-054).
	unsigned long long AtListenerObjectID();

	// Game parameters (RTPCs) by name, globally or on one playing ID, interpolated over transitionMs.
	int		SetGlobalRTPC( const char * name, float value, int transitionMs );
	int		SetRTPCOnPlayingID( const char * name, float value, unsigned int playingID, int transitionMs );
	// Scales the attenuation distances of one game object (1 = as authored).
	int		SetScalingFactor( unsigned long long objectID, float factor );
	// Volume of the main output, 0 (muted) to 1.
	int		SetOutputVolume( float volume );
	// Sets a state of a state group (for example "EnviroSuit", "On").
	int		SetState( const char * group, const char * state );

	// Play position (ms) of a playing event. Returns the SDK result code.
	int		PlayPosition( unsigned int playingID, unsigned int * positionMs );
	// Stops everything Start() started, in reverse order.
	void	Stop();
	// Version of the SDK the game was built against.
	void	Version( int * major, int * minor, int * subminor, int * build );
}

#endif
