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

// Test settings for the debug emitter (wwise_test place / volume / omni), and the time the
// output capture started, so test output can be matched to the recording.
static bool				testPlaced = false;
static float			testForward = 0.0f;		// inches along the listener's view
static float			testLeft = 0.0f;		// inches to the listener's left
static float			testVolume = 0.0f;		// dB, 0 = the shader's own
static bool				testOmni = false;
static float			testMaxDistance = 0.0f;	// meters, 0 = the shader's own
static int				captureStartMs = -1;

static int CaptureMs( void ) {
	return captureStartMs < 0 ? -1 : Sys_Milliseconds() - captureStartMs;
}

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
	// Default: the listener's own emitter, as the player's: it plays on the at-listener game object
	// (D-054). After "wwise_test place", an ordinary emitter at that offset from the listener.
	soundShaderParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	parms.volume = testVolume;
	parms.maxDistance = testMaxDistance;
	parms.soundShaderFlags = testOmni ? SSF_OMNIDIRECTIONAL : 0;
	if ( testPlaced ) {
		const idVec3 origin = world->ListenerOrigin() + world->ListenerAxis()[0] * testForward + world->ListenerAxis()[1] * testLeft;
		debugEmitter->UpdateEmitter( origin, 0, &parms );
	} else {
		debugEmitter->UpdateEmitter( world->ListenerOrigin(), world->ListenerId(), &parms );
	}
	debugEmitter->StartSound( shader, channel, 0.0f, 0, true );
	common->Printf( "wwise_startsound: %s on channel %d, emitter %s, at %d ms of the capture\n", args.Argv( 1 ), channel,
		debugEmitter->CurrentlyPlaying() ? "playing" : "silent", CaptureMs() );
}

/*
================
Wwise_Test_f

Drives the run-time routes for automated tests (the results are read from the capture):
	wwise_test mark <label>				prints the time in the capture
	wwise_test pause | resume			pauses or resumes the playing sound world
	wwise_test fade <dB> <seconds>		FadeSound on the debug emitter
	wwise_test fadeclass <n> <dB> <s>	FadeSoundClasses on the playing world
	wwise_test slowmo <speed>			slow motion at that speed (0 = off)
	wwise_test mute 0|1					SetMute
	wwise_test place <fwd> <left>		next wwise_startsound plays at that offset (inches); "place off" = at the listener
	wwise_test volume <dB>				emitter volume for the next wwise_startsound (0 = the shader's)
	wwise_test omni 0|1					omnidirectional at run time for the next wwise_startsound
	wwise_test maxdist <m>				emitter maxDistance for the next wwise_startsound (0 = the shader's)
	wwise_test enviro 0|1				SetEnviroSuit
================
*/
static void Wwise_Test_f( const idCmdArgs &args ) {
	idSoundWorldWwise *world = static_cast<idSoundWorldWwise *>( soundSystemWwise.GetPlayingSoundWorld() );
	if ( !soundSystemWwise.IsStarted() || world == NULL || args.Argc() < 2 ) {
		common->Printf( "usage: wwise_test mark|pause|resume|fade|fadeclass|slowmo|mute|place|volume|omni|maxdist|enviro ... (Wwise running, playing world)\n" );
		return;
	}
	const char *what = args.Argv( 1 );
	const float a = (float)atof( args.Argv( 2 ) );
	const float b = (float)atof( args.Argv( 3 ) );
	const float c = (float)atof( args.Argv( 4 ) );
	if ( !idStr::Icmp( what, "pause" ) ) {
		world->Pause();
	} else if ( !idStr::Icmp( what, "resume" ) ) {
		world->UnPause();
	} else if ( !idStr::Icmp( what, "fade" ) ) {
		if ( debugEmitter != NULL ) {
			debugEmitter->FadeSound( SCHANNEL_ANY, a, b );
		}
	} else if ( !idStr::Icmp( what, "fadeclass" ) ) {
		world->FadeSoundClasses( (int)a, b, c );
	} else if ( !idStr::Icmp( what, "slowmo" ) ) {
		world->SetSlowmoSpeed( a );
		world->SetSlowmo( a > 0.0f );
	} else if ( !idStr::Icmp( what, "mute" ) ) {
		soundSystemWwise.SetMute( a != 0.0f );
	} else if ( !idStr::Icmp( what, "place" ) ) {
		testPlaced = idStr::Icmp( args.Argv( 2 ), "off" ) != 0;
		testForward = a;
		testLeft = b;
	} else if ( !idStr::Icmp( what, "volume" ) ) {
		testVolume = a;
	} else if ( !idStr::Icmp( what, "maxdist" ) ) {
		testMaxDistance = a;
	} else if ( !idStr::Icmp( what, "omni" ) ) {
		testOmni = a != 0.0f;
	} else if ( !idStr::Icmp( what, "enviro" ) ) {
		world->SetEnviroSuit( a != 0.0f );
	} else if ( idStr::Icmp( what, "mark" ) ) {
		common->Printf( "wwise_test: unknown test %s\n", what );
		return;
	}
	common->Printf( "wwise_test %s %s %s %s at %d ms of the capture\n", what, args.Argv( 2 ), args.Argv( 3 ), args.Argv( 4 ), CaptureMs() );
}

// Plays a sound shader the way menus and GUIs do: wwise_playdirect <shader> [channel] (test).
static void Wwise_PlayDirect_f( const idCmdArgs &args ) {
	idSoundWorld *world = soundSystemWwise.GetPlayingSoundWorld();
	if ( !soundSystemWwise.IsStarted() || world == NULL ) {
		common->Printf( "wwise_playdirect: Wwise must be running with a playing sound world\n" );
		return;
	}
	world->PlayShaderDirectly( args.Argv( 1 ), args.Argc() > 2 ? atoi( args.Argv( 2 ) ) : -1 );
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
	LoadContentBanks();
	if ( s_wwiseCapture.GetString()[0] != '\0' ) {
		if ( WwiseSDK::StartCapture( s_wwiseCapture.GetString() ) ) {
			captureStartMs = Sys_Milliseconds();
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
	cmdSystem->AddCommand( "wwise_test", Wwise_Test_f, CMD_FL_SOUND, "drives pause, fades, slowmo, mute and emitter settings for automated tests" );
	cmdSystem->AddCommand( "wwise_playdirect", Wwise_PlayDirect_f, CMD_FL_SOUND, "plays a sound shader as menus do, through PlayShaderDirectly (test)" );

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
	loadedBanks.Clear();
	eventLengths.Clear();
	common->Printf( "Wwise engine stopped\n" );
}

/*
===============================================================================

	Banks and event lengths

	The banks live in base/wwise/Windows/ next to the game (the same folder the Wwise file
	I/O reads, relative to the game folder), with banks.txt listing the content banks to load
	at start (D-055). Wwise writes <bank>.json next to each bank; the estimated event lengths
	in it are what StartSound returns (D-053).

===============================================================================
*/

static const char *WWISE_BANK_DIR = "base/wwise/Windows/";

// Reads a whole file next to the banks. Returns false if it is missing.
static bool ReadBankDirFile( const char *name, idStr &text ) {
	idStr path = WWISE_BANK_DIR;
	path += name;
	FILE *f = fopen( path.c_str(), "rb" );
	if ( f == NULL ) {
		return false;
	}
	fseek( f, 0, SEEK_END );
	long size = ftell( f );
	fseek( f, 0, SEEK_SET );
	char *buf = new char[ size + 1 ];
	size_t got = fread( buf, 1, size, f );
	buf[ got ] = '\0';
	fclose( f );
	text = buf;
	delete[] buf;
	return true;
}

void idSoundSystemWwise::LoadContentBanks( void ) {
	idStr list;
	idStrList names;
	if ( ReadBankDirFile( "banks.txt", list ) ) {
		list.Replace( "\r", "" );
		int start = 0;
		while ( start <= list.Length() ) {
			int end = list.Find( '\n', start );
			if ( end < 0 ) {
				end = list.Length();
			}
			idStr name = list.Mid( start, end - start );
			name.StripTrailingWhitespace();
			name.StripLeading( ' ' );
			if ( name.Length() > 0 && name[0] != '#' ) {
				names.Append( name );
			}
			start = end + 1;
		}
	} else {
		common->Warning( "Wwise: no %sbanks.txt, loading TestBank only", WWISE_BANK_DIR );
		names.Append( "TestBank" );
	}

	int failed = 0;
	for ( int i = 0; i < names.Num(); i++ ) {
		int res = WwiseSDK::LoadBank( va( "%s.bnk", names[i].c_str() ) );
		if ( res != 1 ) {		// AK_Success
			common->Warning( "Wwise: could not load bank %s (AKRESULT %d)", names[i].c_str(), res );
			failed++;
			continue;
		}
		loadedBanks.Append( names[i] );
		ReadEventLengths( names[i].c_str() );
	}
	common->Printf( "Wwise: loaded %d bank(s), %d failed; %d event length(s) from the bank metadata\n", loadedBanks.Num(), failed, eventLengths.Num() );
}

/*
================
idSoundSystemWwise::ReadEventLengths

Reads "Name", "DurationType" and "DurationMax" (seconds, as a string) of each event in
<bank>.json (SoundbanksInfo). Looping ("Infinite") and "Unknown" events get no length.
Only these fields are needed, so this is a small scan rather than a JSON parser.
================
*/
static bool JsonStringAfter( const idStr &text, const char *key, int from, int limit, idStr &value ) {
	int k = text.Find( key, true, from, limit );
	if ( k < 0 ) {
		return false;
	}
	int q1 = text.Find( '"', k + idStr::Length( key ) );
	if ( q1 < 0 || q1 >= limit ) {
		return false;
	}
	int q2 = text.Find( '"', q1 + 1 );
	if ( q2 < 0 ) {
		return false;
	}
	value = text.Mid( q1 + 1, q2 - q1 - 1 );
	return true;
}

void idSoundSystemWwise::ReadEventLengths( const char *bankName ) {
	idStr text;
	if ( !ReadBankDirFile( va( "%s.json", bankName ), text ) ) {
		common->Warning( "Wwise: no %s%s.json: StartSound returns 0 ms for its events", WWISE_BANK_DIR, bankName );
		return;
	}
	static const char *NAME_KEY = "\"Name\":";
	int pos = text.Find( NAME_KEY, true, 0 );
	while ( pos >= 0 ) {
		int next = text.Find( NAME_KEY, true, pos + 1 );
		int limit = next >= 0 ? next : text.Length();
		idStr name, type, maxSeconds;
		if ( JsonStringAfter( text, NAME_KEY, pos, limit, name )
			&& JsonStringAfter( text, "\"DurationType\":", pos, limit, type )
			&& JsonStringAfter( text, "\"DurationMax\":", pos, limit, maxSeconds ) ) {
			if ( type.Icmp( "Infinite" ) != 0 && type.Icmp( "Unknown" ) != 0 ) {
				int ms = (int)( atof( maxSeconds.c_str() ) * 1000.0f + 0.5f );
				name.ToLower();
				eventLengths.Set( name.c_str(), ms );
			}
		}
		pos = next;
	}
}

int idSoundSystemWwise::EventLengthMs( const char *eventName ) const {
	idStr key = eventName;
	key.ToLower();
	int *ms = NULL;
	if ( eventLengths.Get( key.c_str(), &ms ) && ms != NULL ) {
		return *ms;
	}
	return 0;
}

void idSoundSystemWwise::WarnOnce( const char *key, const char *fmt, const char *arg ) {
	if ( warned.FindIndex( key ) >= 0 ) {
		return;
	}
	warned.Append( key );
	common->Warning( fmt, arg );
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

// Loading screens and menus mute the sound: the main output volume goes to 0 (no content needed).
void idSoundSystemWwise::SetMute( bool mute ) {
	if ( started && mute != muted ) {
		WwiseSDK::SetOutputVolume( mute ? 0.0f : 1.0f );
	}
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
