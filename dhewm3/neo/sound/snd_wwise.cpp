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
	WwiseSDK::Stop();
	common->Printf( "Wwise engine stopped\n" );
	initialized = false;
#endif
}
