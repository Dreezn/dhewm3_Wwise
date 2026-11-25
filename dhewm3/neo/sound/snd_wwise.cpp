/*
===========================================================================

dhewm3 Wwise backend: engine life cycle, as seen by dhewm3 (see snd_wwise.h).
The SDK calls are in snd_wwise_sdk.cpp.

===========================================================================
*/

#include "sys/platform.h"
#include "sound/snd_wwise.h"
#include "sound/snd_local.h"
#include "sound/snd_wwise_sdk.h"
#include "framework/CmdSystem.h"

idWwiseEngine wwiseEngine;

idCVar s_wwiseCapture( "s_wwiseCapture", "", CVAR_SOUND | CVAR_NOCHEAT, "write the Wwise output to this .wav file while the engine runs (debug)" );

/*
================
Wwise_Play_f

Posts a Wwise event on the debug game object: wwise_play <event name>
================
*/
static void Wwise_Play_f( const idCmdArgs &args ) {
#ifdef WWISE_ENABLED
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: wwise_play <event name>\n" );
		return;
	}
	if ( !wwiseEngine.IsInitialized() ) {
		common->Printf( "wwise_play: Wwise is not running (start with +set s_backend wwise)\n" );
		return;
	}
	unsigned int playingID = 0;
	int result = 0;
	if ( !WwiseSDK::PostEvent( args.Argv( 1 ), &playingID, &result ) ) {
		common->Warning( "wwise_play: could not post %s (AKRESULT %d)", args.Argv( 1 ), result );
		return;
	}
	common->Printf( "wwise_play: posted %s (playing ID %u)\n", args.Argv( 1 ), playingID );
#else
	common->Printf( "wwise_play: this build has no Wwise (configure with WWISE_ROOT)\n" );
#endif
}

/*
================
Wwise_CaptureStop_f

Stops the output capture started with s_wwiseCapture, so the .wav file is written.
================
*/
static void Wwise_CaptureStop_f( const idCmdArgs &args ) {
#ifdef WWISE_ENABLED
	WwiseSDK::StopCapture();
	common->Printf( "wwise_capture_stop: capture stopped\n" );
#endif
}

/*
================
Wwise_Status_f

Prints the play position of a playing event: wwise_status <playing ID>
A position that keeps growing shows the engine is rendering that event.
================
*/
static void Wwise_Status_f( const idCmdArgs &args ) {
#ifdef WWISE_ENABLED
	if ( args.Argc() < 2 || !wwiseEngine.IsInitialized() ) {
		common->Printf( "usage: wwise_status <playing ID> (Wwise must be running)\n" );
		return;
	}
	unsigned int playingID = (unsigned int)atoi( args.Argv( 1 ) );
	unsigned int positionMs = 0;
	int res = WwiseSDK::PlayPosition( playingID, &positionMs );
	common->Printf( "wwise_status: playing %u position %u ms (AKRESULT %d)\n", playingID, positionMs, res );
#endif
}

idWwiseEngine::idWwiseEngine() {
	initialized = false;
}

idWwiseEngine::~idWwiseEngine() {
	Shutdown();
}

bool idWwiseEngine::Init() {
#ifdef WWISE_ENABLED
	if ( initialized ) {
		return true;
	}

	int stage = 0;
	int result = 0;
	if ( !WwiseSDK::Start( &stage, &result ) ) {
		common->Warning( "Wwise: engine failed to start at stage %d (AKRESULT %d)", stage, result );
		return false;
	}

	int major = 0, minor = 0, subminor = 0, build = 0;
	WwiseSDK::Version( &major, &minor, &subminor, &build );
	common->Printf( "Wwise %d.%d.%d (build %d) engine started\n", major, minor, subminor, build );
	if ( s_wwiseCapture.GetString()[0] != '\0' ) {
		if ( WwiseSDK::StartCapture( s_wwiseCapture.GetString() ) ) {
			common->Printf( "Wwise output capture to %s\n", s_wwiseCapture.GetString() );
		} else {
			common->Warning( "Wwise output capture to %s failed", s_wwiseCapture.GetString() );
		}
	}
	cmdSystem->AddCommand( "wwise_capture_stop", Wwise_CaptureStop_f, CMD_FL_SOUND, "stops the Wwise output capture and writes the .wav file" );
	cmdSystem->AddCommand( "wwise_status", Wwise_Status_f, CMD_FL_SOUND, "prints the play position of a Wwise playing ID" );
	cmdSystem->AddCommand( "wwise_play", Wwise_Play_f, CMD_FL_SOUND, "posts a Wwise event for testing (wwise_play Play_TestTone)" );

	initialized = true;
	return true;
#else
	return false;
#endif
}

void idWwiseEngine::Shutdown() {
#ifdef WWISE_ENABLED
	if ( !initialized ) {
		return;
	}
	WwiseSDK::StopCapture();
	WwiseSDK::Stop();
	common->Printf( "Wwise engine stopped\n" );
	initialized = false;
#endif
}
