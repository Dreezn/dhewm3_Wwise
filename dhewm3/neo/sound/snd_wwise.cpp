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

idCVar s_wwiseDevice( "s_wwiseDevice", "0", CVAR_SOUND | CVAR_INTEGER | CVAR_NOCHEAT, "Wwise output device ID from wwise_devices (0 = default)" );
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

/*
================
Wwise_Devices_f

Lists the Wwise audio output devices: wwise_devices
================
*/
static void Wwise_Devices_f( const idCmdArgs &args ) {
#ifdef WWISE_ENABLED
	if ( !wwiseEngine.IsInitialized() ) {
		common->Printf( "wwise_devices: Wwise must be running (+set s_backend wwise)\n" );
		return;
	}
	char report[ 2048 ];
	int res = WwiseSDK::OutputDeviceReport( report, sizeof( report ) );
	common->Printf( "wwise_devices: AKRESULT %d\n%s", res, report );
#endif
}

// Wwise engine messages go through dhewm3's normal console and warning flow.
static void WwiseLog( const char * text, bool isError ) {
	if ( isError ) {
		common->Warning( "Wwise: %s", text );
	} else {
		common->Printf( "Wwise: %s\n", text );
	}
}

/*
================
Wwise_Output_f

Prints the output IDs the engine uses: wwise_output
================
*/
static void Wwise_Output_f( const idCmdArgs &args ) {
#ifdef WWISE_ENABLED
	if ( !wwiseEngine.IsInitialized() ) {
		common->Printf( "wwise_output: Wwise must be running (+set s_backend wwise)\n" );
		return;
	}
	char report[ 512 ];
	WwiseSDK::OutputState( report, sizeof( report ) );
	common->Printf( "wwise_output: %s", report );
#endif
}

/*
================
Wwise_AddOutput_f

Opens the 'System' output explicitly and reports the SDK result: wwise_addoutput
================
*/
static void Wwise_AddOutput_f( const idCmdArgs &args ) {
#ifdef WWISE_ENABLED
	if ( !wwiseEngine.IsInitialized() ) {
		common->Printf( "wwise_addoutput: Wwise must be running (+set s_backend wwise)\n" );
		return;
	}
	unsigned int outputID = 0;
	int res = WwiseSDK::AddSystemOutput( &outputID );
	common->Printf( "wwise_addoutput: AKRESULT %d, output id %u\n", res, outputID );
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

	WwiseSDK::SetLogFunc( WwiseLog );
	int stage = 0;
	int result = 0;
	if ( !WwiseSDK::Start( &stage, &result, (unsigned int)s_wwiseDevice.GetInteger() ) ) {
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
	cmdSystem->AddCommand( "wwise_addoutput", Wwise_AddOutput_f, CMD_FL_SOUND, "opens the System audio output explicitly" );
	cmdSystem->AddCommand( "wwise_output", Wwise_Output_f, CMD_FL_SOUND, "prints the Wwise output IDs" );
	cmdSystem->AddCommand( "wwise_devices", Wwise_Devices_f, CMD_FL_SOUND, "lists the Wwise audio output devices" );
	cmdSystem->AddCommand( "wwise_status", Wwise_Status_f, CMD_FL_SOUND, "prints the play position of a Wwise playing ID" );
	cmdSystem->AddCommand( "wwise_play", Wwise_Play_f, CMD_FL_SOUND, "posts a Wwise event for testing (wwise_play Play_TestTone)" );

	initialized = true;
	return true;
#else
	return false;
#endif
}

void idWwiseEngine::Update() {
#ifdef WWISE_ENABLED
	if ( initialized ) {
		WwiseSDK::RenderAudio();
	}
#endif
}

void idWwiseEngine::Shutdown() {
#ifdef WWISE_ENABLED
	if ( !initialized ) {
		return;
	}
	// Clear the flag first so the sound tick stops calling Update() before the engine goes away.
	initialized = false;
	WwiseSDK::StopCapture();
	WwiseSDK::Stop();
	common->Printf( "Wwise engine stopped\n" );
#endif
}
