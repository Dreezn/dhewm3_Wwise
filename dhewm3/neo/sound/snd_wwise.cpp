/*
===========================================================================

dhewm3 Wwise backend: idSoundSystemWwise, the Wwise provider of idSoundSystem.

Owns the Wwise engine life cycle, calls RenderAudio from dhewm3's sound tick,
and registers the Wwise debug console commands. See snd_wwise.h.

===========================================================================
*/

#include "sys/platform.h"
#include "sys/sys_public.h"
#include "framework/Common.h"
#include "framework/CmdSystem.h"
#include "framework/DeclManager.h"

#include "sound/snd_wwise.h"
#include "sound/snd_wwise_sdk.h"

idSoundSystemWwise soundSystemWwise;

idCVar idSoundSystemWwise::s_wwiseDevice( "s_wwiseDevice", "0", CVAR_SOUND | CVAR_INTEGER | CVAR_NOCHEAT, "Wwise output device ID from wwise_devices (0 = default)" );
idCVar idSoundSystemWwise::s_wwiseCapture( "s_wwiseCapture", "", CVAR_SOUND | CVAR_NOCHEAT, "write the Wwise output to this .wav file while the engine runs (debug)" );
idCVar idSoundSystemWwise::s_wwiseShowStartSound( "s_wwiseShowStartSound", "0", CVAR_SOUND | CVAR_BOOL, "print every StartSound that reaches the Wwise provider (debug)" );

/*
===============================================================================

	Debug console commands

===============================================================================
*/

/*
================
Wwise_Play_f

Posts a Wwise event on the debug game object: wwise_play <event name>
================
*/
static void Wwise_Play_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 ) {
		common->Printf( "usage: wwise_play <event name>\n" );
		return;
	}
	if ( !soundSystemWwise.IsStarted() ) {
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
}

/*
================
Wwise_CaptureStop_f

Stops the output capture started with s_wwiseCapture, so the .wav file is written.
================
*/
static void Wwise_CaptureStop_f( const idCmdArgs &args ) {
	WwiseSDK::StopCapture();
	common->Printf( "wwise_capture_stop: capture stopped\n" );
}

/*
================
Wwise_Status_f

Prints the play position of a playing event: wwise_status <playing ID>
A position that keeps growing shows the engine is rendering that event.
================
*/
static void Wwise_Status_f( const idCmdArgs &args ) {
	if ( args.Argc() < 2 || !soundSystemWwise.IsStarted() ) {
		common->Printf( "usage: wwise_status <playing ID> (Wwise must be running)\n" );
		return;
	}
	unsigned int playingID = (unsigned int)atoi( args.Argv( 1 ) );
	unsigned int positionMs = 0;
	int res = WwiseSDK::PlayPosition( playingID, &positionMs );
	common->Printf( "wwise_status: playing %u position %u ms (AKRESULT %d)\n", playingID, positionMs, res );
}

/*
================
Wwise_Devices_f

Lists the Wwise audio output devices: wwise_devices
================
*/
static void Wwise_Devices_f( const idCmdArgs &args ) {
	if ( !soundSystemWwise.IsStarted() ) {
		common->Printf( "wwise_devices: Wwise must be running (+set s_backend wwise)\n" );
		return;
	}
	char report[ 2048 ];
	int res = WwiseSDK::OutputDeviceReport( report, sizeof( report ) );
	common->Printf( "wwise_devices: AKRESULT %d\n%s", res, report );
}

/*
================
Wwise_Missing_f

Lists the sound shaders whose event could not be posted so far: wwise_missing
This is the to-do list of events for the maps played in this session.
================
*/
static void Wwise_Missing_f( const idCmdArgs &args ) {
	const idStrList &missing = soundSystemWwise.MissingShaders();
	idStr eventName;
	for ( int i = 0; i < missing.Num(); i++ ) {
		idSoundEmitterWwise::EventNameForShader( missing[i].c_str(), eventName );
		common->Printf( "  %-40s %s\n", missing[i].c_str(), eventName.c_str() );
	}
	common->Printf( "wwise_missing: %d shader(s) without an event\n", missing.Num() );
}

/*
================
Wwise_StartSound_f

Plays a sound shader through a Wwise emitter at the listener, the same path the game uses:
wwise_startsound <shader> [channel]. Without arguments, prints whether that emitter still plays.
================
*/
static idSoundEmitter *	debugEmitter = NULL;
static idSoundWorld *	debugEmitterWorld = NULL;

static void Wwise_StartSound_f( const idCmdArgs &args ) {
	idSoundWorldWwise *world = static_cast<idSoundWorldWwise *>( soundSystemWwise.GetPlayingSoundWorld() );
	if ( !soundSystemWwise.IsStarted() || world == NULL ) {
		common->Printf( "wwise_startsound: Wwise must be running with a playing sound world\n" );
		return;
	}
	// A map change frees all emitters (ClearAllSoundEmitters); take a new one then. Test command only.
	if ( debugEmitterWorld != world || !static_cast<idSoundEmitterWwise *>( debugEmitter )->IsAlive() ) {
		debugEmitter = world->AllocSoundEmitter();
		debugEmitterWorld = world;
	}
	static int lastStart = 0;
	if ( args.Argc() < 2 ) {
		unsigned int ids[ 64 ];
		int active = WwiseSDK::PlayingIDsOnObject( static_cast<idSoundEmitterWwise *>( debugEmitter )->GameObjectID(), ids, 64 );
		common->Printf( "wwise_startsound: debug emitter %s (%d ms after the last start, Wwise lists %d playing ID(s))\n",
			debugEmitter->CurrentlyPlaying() ? "playing" : "silent", Sys_Milliseconds() - lastStart, active );
		return;
	}
	lastStart = Sys_Milliseconds();
	const idSoundShader *shader = declManager->FindSound( args.Argv( 1 ) );
	int channel = args.Argc() > 2 ? atoi( args.Argv( 2 ) ) : SCHANNEL_ANY;
	debugEmitter->UpdateEmitter( world->ListenerOrigin(), 0, NULL );
	debugEmitter->StartSound( shader, channel, 0.0f, 0, true );
	common->Printf( "wwise_startsound: %s on channel %d, emitter %s\n", args.Argv( 1 ), channel,
		debugEmitter->CurrentlyPlaying() ? "playing" : "silent" );
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
===============================================================================

	idSoundSystemWwise

===============================================================================
*/

idSoundSystemWwise::idSoundSystemWwise( void ) {
	started = false;
	muted = false;
	worldCount = 0;
	playingWorld = NULL;
}

/*
================
idSoundSystemWwise::Start

Called by idSoundSystemLocator. Returns false if the engine could not start.
================
*/
bool idSoundSystemWwise::Start( void ) {
	if ( started ) {
		return true;
	}
	common->Printf( "----- Initializing Wwise -----\n" );

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
	cmdSystem->AddCommand( "wwise_devices", Wwise_Devices_f, CMD_FL_SOUND, "lists the Wwise audio output devices" );
	cmdSystem->AddCommand( "wwise_status", Wwise_Status_f, CMD_FL_SOUND, "prints the play position of a Wwise playing ID" );
	cmdSystem->AddCommand( "wwise_play", Wwise_Play_f, CMD_FL_SOUND, "posts a Wwise event for testing (wwise_play Play_TestTone)" );
	cmdSystem->AddCommand( "wwise_missing", Wwise_Missing_f, CMD_FL_SOUND, "lists the sound shaders that had no Wwise event" );
	cmdSystem->AddCommand( "wwise_startsound", Wwise_StartSound_f, CMD_FL_SOUND, "plays a sound shader through a Wwise emitter at the listener (test)" );

	started = true;
	return true;
}

void idSoundSystemWwise::Init( void ) {
	// The locator calls Start() so it can fall back to OpenAL; Init() is kept for the interface.
	Start();
}

void idSoundSystemWwise::Shutdown( void ) {
	if ( !started ) {
		return;
	}
	// Clear the flag first so the sound tick stops calling RenderAudio before the engine goes away.
	started = false;
	WwiseSDK::StopCapture();
	WwiseSDK::Stop();
	cmdSystem->RemoveCommand( "wwise_capture_stop" );
	cmdSystem->RemoveCommand( "wwise_devices" );
	cmdSystem->RemoveCommand( "wwise_status" );
	cmdSystem->RemoveCommand( "wwise_play" );
	cmdSystem->RemoveCommand( "wwise_missing" );
	cmdSystem->RemoveCommand( "wwise_startsound" );
	common->Printf( "Wwise engine stopped\n" );
}

// Wwise opens its own output device when the engine starts, so there is no separate hardware step.
bool idSoundSystemWwise::InitHW( void ) {
	return started;
}

bool idSoundSystemWwise::ShutdownHW( void ) {
	return true;
}

/*
================
idSoundSystemWwise::AsyncUpdate / AsyncUpdateWrite

dhewm3's sound tick (async sound thread, or the main thread with com_asyncSound 0).
The SDK requires RenderAudio once per frame: it processes the queued commands.
================
*/
int idSoundSystemWwise::AsyncUpdate( int time ) {
	if ( started ) {
		WwiseSDK::RenderAudio();
	}
	return 0;
}

int idSoundSystemWwise::AsyncUpdateWrite( int time ) {
	return AsyncUpdate( time );
}

// Stored only; muting through the Wwise master bus comes with the game routes (Phase 4).
void idSoundSystemWwise::SetMute( bool mute ) {
	muted = mute;
}

// Sound level meter for GUIs: no Wwise equivalent yet (triage item), returns an empty image.
cinData_t idSoundSystemWwise::ImageForTime( const int milliseconds, const bool waveform ) {
	cinData_t ret;
	memset( &ret, 0, sizeof( ret ) );
	return ret;
}

// No dhewm3 decoders in Wwise mode (Wwise decodes its own media).
int idSoundSystemWwise::GetSoundDecoderInfo( int index, soundDecoderInfo_t &decoderInfo ) {
	return -1;
}

idSoundWorld * idSoundSystemWwise::AllocSoundWorld( idRenderWorld *rw ) {
	// rw is used by OpenAL for portal occlusion; not used by the Wwise provider yet.
	return new idSoundWorldWwise( ++worldCount );
}

void idSoundSystemWwise::SetPlayingSoundWorld( idSoundWorld *soundWorld ) {
	// Only the Wwise provider's own worlds are ever passed in (all worlds come from AllocSoundWorld).
	playingWorld = static_cast<idSoundWorldWwise *>( soundWorld );
	if ( playingWorld != NULL ) {
		playingWorld->ApplyListener();
	}
}

idSoundWorld * idSoundSystemWwise::GetPlayingSoundWorld( void ) {
	return playingWorld;
}

// Banks are loaded at start-up for now; per-map bank loading is a later step.
void idSoundSystemWwise::BeginLevelLoad( void ) {
}

void idSoundSystemWwise::EndLevelLoad( const char *mapString ) {
}

// Direct mixing into dhewm3's buffer is not used: Wwise mixes and outputs on its own.
int idSoundSystemWwise::AsyncMix( int soundTime, float *mixBuffer ) {
	return 0;
}

void idSoundSystemWwise::PrintMemInfo( MemInfo_t *mi ) {
}

void idSoundSystemWwise::ReportMissing( const char *shaderName ) {
	if ( missingShaders.FindIndex( shaderName ) < 0 ) {
		missingShaders.Append( shaderName );
	}
}

// EFX is OpenAL's reverb; Wwise has its own effects.
int idSoundSystemWwise::IsEFXAvailable( void ) {
	return 0;
}
