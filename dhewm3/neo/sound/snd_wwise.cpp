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
#include "framework/FileSystem.h"
#include <SDL.h>

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
Lists the event banks by level (section 8.8): wwise_banks [list]
================
*/
static void Wwise_Banks_f( const idCmdArgs &args ) {
	soundSystemWwise.PrintBanks( args.Argc() > 1 && !idStr::Icmp( args.Argv( 1 ), "list" ) );
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
	const idStrList &undefined = soundSystemWwise.UndefinedShaders();
	for ( int i = 0; i < undefined.Num(); i++ ) {
		common->Printf( "  %s\n", undefined[i].c_str() );
	}
	common->Printf( "wwise_missing: %d shader(s) with no definition and no sample file (map data errors, not played)\n", undefined.Num() );
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
	wwise_test modify <dB>				ModifySound on the debug emitter: new emitter volume (the heartbeat route)
	wwise_test wave						prints what the HUD waveform image (ImageForTime) shows now
	wwise_test fadeclass <n> <dB> <s>	FadeSoundClasses on the playing world
	wwise_test slowmo <speed>			slow motion at that speed (0 = off)
	wwise_test mute 0|1					SetMute
	wwise_test place <fwd> <left>		next wwise_startsound plays at that offset (inches); "place off" = at the listener
	wwise_test volume <dB>				emitter volume for the next wwise_startsound (0 = the shader's)
	wwise_test omni 0|1					omnidirectional at run time for the next wwise_startsound
	wwise_test maxdist <m>				emitter maxDistance for the next wwise_startsound (0 = the shader's)
	wwise_test enviro 0|1				SetEnviroSuit
	wwise_test key <SDL key name>		presses and releases a key through SDL (Escape: in-game menu, Tab: PDA)
================
*/
static void Wwise_Test_f( const idCmdArgs &args ) {
	idSoundWorldWwise *world = static_cast<idSoundWorldWwise *>( soundSystemWwise.GetPlayingSoundWorld() );
	if ( !soundSystemWwise.IsStarted() || world == NULL || args.Argc() < 2 ) {
		common->Printf( "usage: wwise_test mark|pause|resume|fade|modify|wave|fadeclass|slowmo|mute|place|volume|omni|maxdist|enviro|key ... (Wwise running, playing world)\n" );
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
	} else if ( !idStr::Icmp( what, "wave" ) ) {
		// what a soundMap material would show now: lit pixels of the waveform and the meter image
		int tapResult, tapCalls;
		WwiseSDK::MixTapStatus( &tapResult, &tapCalls );
		common->Printf( "wwise_test wave: capture callback registered with AKRESULT %d, %d calls\n", tapResult, tapCalls );
		for ( int mode = 0; mode < 2; mode++ ) {
			cinData_t image = soundSystemWwise.ImageForTime( 0, mode == 0 );
			int lit = 0, top = 128, bottom = -1;
			const dword *pixels = (const dword *)image.image;
			for ( int p = 0; pixels != NULL && p < image.imageWidth * image.imageHeight; p++ ) {
				if ( pixels[p] != 0 ) {
					lit++;
					top = Min( top, p / image.imageWidth );
					bottom = Max( bottom, p / image.imageWidth );
				}
			}
			common->Printf( "wwise_test wave: %s %dx%d, %d lit pixels, rows %d..%d, at %d ms of the capture\n", mode == 0 ? "waveform" : "meter",
				image.imageWidth, image.imageHeight, lit, top, bottom, CaptureMs() );
		}
	} else if ( !idStr::Icmp( what, "modify" ) ) {
		if ( debugEmitter != NULL ) {
			soundShaderParms_t parms;
			memset( &parms, 0, sizeof( parms ) );
			parms.volume = a;
			debugEmitter->ModifySound( SCHANNEL_ANY, &parms );
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
	} else if ( !idStr::Icmp( what, "key" ) ) {
		// A key press and release as SDL delivers it (SDL key name, e.g. Escape, Tab), so the whole
		// input path runs: the session (Esc opens and closes the in-game menu) and the bindings (Tab = PDA).
		SDL_Event ev;
		memset( &ev, 0, sizeof( ev ) );
		ev.key.keysym.sym = SDL_GetKeyFromName( args.Argv( 2 ) );
		ev.key.keysym.scancode = SDL_GetScancodeFromKey( ev.key.keysym.sym );
		if ( ev.key.keysym.sym == SDLK_UNKNOWN ) {
			common->Printf( "wwise_test key: unknown SDL key name %s\n", args.Argv( 2 ) );
			return;
		}
		ev.type = SDL_KEYDOWN;
		ev.key.state = SDL_PRESSED;
		SDL_PushEvent( &ev );
		ev.type = SDL_KEYUP;
		ev.key.state = SDL_RELEASED;
		SDL_PushEvent( &ev );
	} else if ( !idStr::Icmp( what, "bank" ) ) {
		// Event bank tests (section 8.8): bank load|unload <event>, bank mem, bank loadall.
		// load and unload go straight to the SDK (not the level bookkeeping): use events the level does not use.
		const char *op = args.Argv( 2 );
		unsigned long long total, media, structures;
		if ( !idStr::Icmp( op, "prepare" ) || !idStr::Icmp( op, "unprepare" ) ) {
			const char *names[16];
			int n = 0;
			for ( int i = 3; i < args.Argc() && n < 16; i++ ) {
				names[ n++ ] = args.Argv( i );
			}
			const double t0 = Sys_MillisecondsPrecise();
			const int res = WwiseSDK::PrepareEvents( names, n, !idStr::Icmp( op, "prepare" ) );
			const double ms = Sys_MillisecondsPrecise() - t0;
			WwiseSDK::MemoryUsed( &total, &media, &structures );
			common->Printf( "wwise_test bank: %s %d event(s) -> AKRESULT %d in %.2f ms; memory %llu bytes, media %llu bytes\n", op, n, res, ms, total, media );
		} else if ( !idStr::Icmp( op, "load" ) || !idStr::Icmp( op, "unload" ) ) {
			const bool load = !idStr::Icmp( op, "load" );
			const double t0 = Sys_MillisecondsPrecise();
			const int res = load ? WwiseSDK::LoadEventBank( args.Argv( 3 ) ) : WwiseSDK::UnloadEventBank( args.Argv( 3 ) );
			const double ms = Sys_MillisecondsPrecise() - t0;
			WwiseSDK::MemoryUsed( &total, &media, &structures );
			common->Printf( "wwise_test bank: %s %s -> AKRESULT %d in %.2f ms; memory %llu bytes, media %llu bytes\n", op, args.Argv( 3 ), res, ms, total, media );
		} else if ( !idStr::Icmp( op, "loadall" ) ) {
			// Cost of loading every event bank at once (the 'all at start' layout), then unloading them again.
			idStrList files;
			Sys_ListFiles( "base/wwise/Windows/Event", ".bnk", files );
			WwiseSDK::MemoryUsed( &total, &media, &structures );
			const unsigned long long before = total, mediaBefore = media;
			idStrList loaded;
			const double t0 = Sys_MillisecondsPrecise();
			for ( int i = 0; i < files.Num(); i++ ) {
				idStr name = files[i];
				name.StripFileExtension();
				if ( WwiseSDK::LoadEventBank( name.c_str() ) == WwiseSDK::RESULT_SUCCESS ) {
					loaded.Append( name );
				}
			}
			// and their media (PrepareEvent), in batches; a failed batch is retried one by one
			idList<const char *> names;
			for ( int i = 0; i < loaded.Num(); i++ ) {
				names.Append( loaded[i].c_str() );
			}
			int prepared = 0;
			for ( int i = 0; i < names.Num(); i += 256 ) {
				const int n = Min( 256, names.Num() - i );
				if ( WwiseSDK::PrepareEvents( names.Ptr() + i, n, true ) == WwiseSDK::RESULT_SUCCESS ) {
					prepared += n;
				} else {
					for ( int k = 0; k < n; k++ ) {
						prepared += WwiseSDK::PrepareEvents( names.Ptr() + i + k, 1, true ) == WwiseSDK::RESULT_SUCCESS ? 1 : 0;
					}
				}
			}
			const double ms = Sys_MillisecondsPrecise() - t0;
			WwiseSDK::MemoryUsed( &total, &media, &structures );
			common->Printf( "wwise_test bank: loadall: %d of %d event banks loaded, %d prepared, in %.0f ms; memory +%.1f MB (media +%.1f MB, %.1f MB in all)\n",
				loaded.Num(), files.Num(), prepared, ms, ( total - before ) / 1048576.0, ( media - mediaBefore ) / 1048576.0, total / 1048576.0 );
			WwiseSDK::PrepareEvents( names.Ptr(), names.Num(), false );
			for ( int i = 0; i < loaded.Num(); i++ ) {
				WwiseSDK::UnloadEventBank( loaded[i].c_str() );
			}
			WwiseSDK::MemoryUsed( &total, &media, &structures );
			common->Printf( "wwise_test bank: loadall: unloaded again; memory %.1f MB\n", total / 1048576.0 );
		} else {
			WwiseSDK::MemoryUsed( &total, &media, &structures );
			common->Printf( "wwise_test bank: memory %llu bytes, media %llu bytes, structures %llu bytes\n", total, media, structures );
		}
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
	insideLevelLoad = false;
	firstUseLoads = 0;
	firstUseMs = 0.0;
	firstUseMaxMs = 0.0;
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
	LoadResidentBanks();
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
	cmdSystem->AddCommand( "wwise_banks", Wwise_Banks_f, CMD_FL_SOUND, "lists the Wwise event banks by level: loaded, resident, missing, first-use loads, memory (wwise_banks list: names)" );
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
	cmdSystem->RemoveCommand( "wwise_banks" );
	cmdSystem->RemoveCommand( "wwise_startsound" );
	loadedBanks.Clear();
	eventLengths.Clear();
	eventBanks.Clear();
	eventBankIndex.Clear();
	userBankEvents.Clear();
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
		ReadEventLengths( names[i].c_str(), true );
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

void idSoundSystemWwise::ReadEventLengths( const char *bankName, bool userBank ) {
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
		if ( userBank && JsonStringAfter( text, NAME_KEY, pos, limit, name ) ) {
			name.ToLower();
			int one = 1;
			userBankEvents.Set( name.c_str(), one );
		}
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

/*
===============================================================================

	Banks by level (section 8.8, D-064)

	Each event has its own auto-defined bank, Event/<event>.bnk, which loads the event and its
	media (Media/<id>.wem; media longer than 15 s streams, D-065). As OpenAL does with samples:
	- BeginLevelLoad clears the level marks;
	- every sound shader parsed during the level load marks its event (ShaderParsed);
	- EndLevelLoad loads the marked banks and unloads the loaded ones the level did not mark;
	- an event posted while its bank is not loaded loads it at once (EnsureEventLoaded), as
	  OpenAL loads a purged sample when it is played.
	The events in resident.txt (menu and player sounds) load at start and stay. Events in the
	banks of banks.txt (user-defined banks, a student's own layout) need no event bank.

===============================================================================
*/

int idSoundSystemWwise::FindEventBank( const char *eventName, bool add ) {
	idStr key = eventName;
	key.ToLower();
	int *index = NULL;
	if ( eventBankIndex.Get( key.c_str(), &index ) && index != NULL ) {
		return *index;
	}
	if ( !add ) {
		return -1;
	}
	eventBank_t bank;
	bank.name = eventName;
	bank.state = EB_UNLOADED;
	bank.marked = false;
	int n = eventBanks.Append( bank );
	eventBankIndex.Set( key.c_str(), n );
	return n;
}

/*
================
idSoundSystemWwise::LoadEventBanks

Loads the event banks (structures) one by one, then prepares their events in one batch, which
loads their loose media from Media/ (PrepareEvent needs the event's bank loaded first, S-112).
The bank manager counts media references, so media shared by several events is loaded once.
Returns how many events are ready.
================
*/
int idSoundSystemWwise::LoadEventBanks( const idList<int> &indices ) {
	idList<int> loaded;
	idList<const char *> names;
	for ( int k = 0; k < indices.Num(); k++ ) {
		eventBank_t &bank = eventBanks[ indices[k] ];
		idStr file = WWISE_BANK_DIR;
		file += "Event/";
		file += bank.name;
		file += ".bnk";
		FILE *f = fopen( file.c_str(), "rb" );
		if ( f == NULL ) {
			bank.state = EB_MISSING;
			continue;
		}
		fclose( f );
		const int res = WwiseSDK::LoadEventBank( bank.name.c_str() );
		if ( res != WwiseSDK::RESULT_SUCCESS ) {
			common->Warning( "Wwise: could not load the event bank %s (AKRESULT %d)", bank.name.c_str(), res );
			bank.state = EB_MISSING;
			continue;
		}
		loaded.Append( indices[k] );
		names.Append( bank.name.c_str() );
	}
	if ( loaded.Num() == 0 ) {
		return 0;
	}
	int ready = 0;
	if ( WwiseSDK::PrepareEvents( names.Ptr(), names.Num(), true ) == WwiseSDK::RESULT_SUCCESS ) {
		for ( int k = 0; k < loaded.Num(); k++ ) {
			eventBanks[ loaded[k] ].state = EB_LOADED;
		}
		ready = loaded.Num();
	} else {
		// One failure cancels the whole batch (SDK): prepare them one by one.
		for ( int k = 0; k < loaded.Num(); k++ ) {
			eventBank_t &bank = eventBanks[ loaded[k] ];
			const char *one = bank.name.c_str();
			const int res = WwiseSDK::PrepareEvents( &one, 1, true );
			if ( res == WwiseSDK::RESULT_SUCCESS ) {
				bank.state = EB_LOADED;
				ready++;
			} else {
				common->Warning( "Wwise: could not prepare %s (AKRESULT %d)", one, res );
				WwiseSDK::UnloadEventBank( one );
				bank.state = EB_MISSING;
			}
		}
	}
	for ( int k = 0; k < loaded.Num(); k++ ) {
		if ( eventBanks[ loaded[k] ].state == EB_LOADED ) {
			ReadEventLengths( va( "Event/%s", eventBanks[ loaded[k] ].name.c_str() ), false );
		}
	}
	return ready;
}

// Unprepares the events in one batch (frees media no other prepared event uses), then unloads their banks.
void idSoundSystemWwise::UnloadEventBanks( const idList<int> &indices ) {
	idList<const char *> names;
	for ( int k = 0; k < indices.Num(); k++ ) {
		names.Append( eventBanks[ indices[k] ].name.c_str() );
	}
	if ( names.Num() == 0 ) {
		return;
	}
	WwiseSDK::PrepareEvents( names.Ptr(), names.Num(), false );
	for ( int k = 0; k < indices.Num(); k++ ) {
		WwiseSDK::UnloadEventBank( eventBanks[ indices[k] ].name.c_str() );
		eventBanks[ indices[k] ].state = EB_UNLOADED;
	}
}

bool idSoundSystemWwise::LoadEventBankAt( int index, bool firstUse ) {
	idList<int> one;
	one.Append( index );
	const double t0 = Sys_MillisecondsPrecise();
	const bool ok = LoadEventBanks( one ) == 1;
	const double ms = Sys_MillisecondsPrecise() - t0;
	if ( ok && firstUse ) {
		firstUseLoads++;
		firstUseMs += ms;
		if ( ms > firstUseMaxMs ) {
			firstUseMaxMs = ms;
		}
		if ( s_wwiseShowStartSound.GetBool() ) {
			common->Printf( "Wwise: event bank %s loaded at first use in %.2f ms\n", eventBanks[ index ].name.c_str(), ms );
		}
	}
	return ok;
}

void idSoundSystemWwise::LoadResidentBanks( void ) {
	idStr list;
	if ( !ReadBankDirFile( "resident.txt", list ) ) {
		return;		// a project without auto-defined banks (banks.txt only)
	}
	list.Replace( "\r", "" );
	const double t0 = Sys_MillisecondsPrecise();
	int loaded = 0, failed = 0;
	idList<int> wanted;
	int start = 0;
	while ( start <= list.Length() ) {
		int end = list.Find( '\n', start );
		if ( end < 0 ) {
			end = list.Length();
		}
		idStr name = list.Mid( start, end - start );
		name.StripTrailingWhitespace();
		name.StripLeading( ' ' );
		start = end + 1;
		if ( name.Length() == 0 || name[0] == '#' ) {
			continue;
		}
		const int index = FindEventBank( name.c_str(), true );
		if ( eventBanks[ index ].state == EB_UNLOADED ) {
			wanted.Append( index );
		}
	}
	loaded = LoadEventBanks( wanted );
	failed = wanted.Num() - loaded;
	for ( int k = 0; k < wanted.Num(); k++ ) {
		if ( eventBanks[ wanted[k] ].state == EB_LOADED ) {
			eventBanks[ wanted[k] ].state = EB_RESIDENT;
		}
	}
	common->Printf( "Wwise: %d resident event bank(s) loaded in %.0f ms, %d failed\n", loaded, Sys_MillisecondsPrecise() - t0, failed );
}

void idSoundSystemWwise::ShaderParsed( const char *shaderName ) {
	if ( !started || !insideLevelLoad || shaderName == NULL ) {
		return;
	}
	idStr eventName;
	idSoundEmitterWwise::EventNameForShader( shaderName, eventName );
	idStr key = eventName;
	key.ToLower();
	if ( userBankEvents.Get( key.c_str() ) ) {
		return;
	}
	eventBanks[ FindEventBank( eventName.c_str(), true ) ].marked = true;
}

bool idSoundSystemWwise::EnsureEventLoaded( const char *eventName ) {
	if ( !started ) {
		return false;
	}
	idStr key = eventName;
	key.ToLower();
	if ( userBankEvents.Get( key.c_str() ) ) {
		return true;
	}
	eventBank_t &bank = eventBanks[ FindEventBank( eventName, true ) ];
	bank.marked = true;		// used by this level
	switch ( bank.state ) {
		case EB_LOADED:
		case EB_RESIDENT:
			return true;
		case EB_MISSING:
			return false;
		default:
			return LoadEventBankAt( FindEventBank( eventName, false ), !insideLevelLoad );
	}
}

void idSoundSystemWwise::BeginLevelLoad( void ) {
	if ( !started ) {
		return;
	}
	insideLevelLoad = true;
	for ( int i = 0; i < eventBanks.Num(); i++ ) {
		eventBanks[i].marked = false;
	}
}

void idSoundSystemWwise::EndLevelLoad( const char *mapString ) {
	if ( !started ) {
		return;
	}
	const double t0 = Sys_MillisecondsPrecise();
	int kept = 0, resident = 0, missing = 0;
	idList<int> toLoad, toUnload;
	for ( int i = 0; i < eventBanks.Num(); i++ ) {
		const eventBank_t &bank = eventBanks[i];
		if ( bank.state == EB_RESIDENT ) {
			resident++;
		} else if ( bank.marked && bank.state == EB_UNLOADED ) {
			toLoad.Append( i );
		} else if ( bank.marked && bank.state == EB_LOADED ) {
			kept++;
		} else if ( !bank.marked && bank.state == EB_LOADED ) {
			toUnload.Append( i );
		} else if ( bank.marked && bank.state == EB_MISSING ) {
			missing++;
		}
	}
	// Unload first, so media the new level does not share is freed before the new media loads.
	UnloadEventBanks( toUnload );
	const int unloaded = toUnload.Num();
	const int loaded = LoadEventBanks( toLoad );
	missing += toLoad.Num() - loaded;
	insideLevelLoad = false;
	firstUseLoads = 0;
	firstUseMs = 0.0;
	firstUseMaxMs = 0.0;
	unsigned long long total, media, structures;
	WwiseSDK::MemoryUsed( &total, &media, &structures );
	common->Printf( "Wwise: level %s: %d event bank(s) loaded, %d kept, %d unloaded, %d resident, %d without a bank; %.0f ms; memory %.1f MB (media %.1f MB)\n",
		mapString, loaded, kept, unloaded, resident, missing, Sys_MillisecondsPrecise() - t0, total / 1048576.0, media / 1048576.0 );
}

void idSoundSystemWwise::PrintBanks( bool list ) {
	int count[4] = { 0, 0, 0, 0 };
	for ( int i = 0; i < eventBanks.Num(); i++ ) {
		count[ eventBanks[i].state ]++;
		if ( list && eventBanks[i].state != EB_UNLOADED ) {
			static const char *names[4] = { "unloaded", "loaded", "resident", "missing" };
			common->Printf( "  %-48s %s%s\n", eventBanks[i].name.c_str(), names[ eventBanks[i].state ], eventBanks[i].marked ? "" : " (not used by this level)" );
		}
	}
	unsigned long long total, media, structures;
	WwiseSDK::MemoryUsed( &total, &media, &structures );
	common->Printf( "wwise_banks: %d loaded, %d resident, %d missing, %d known and unloaded; %d first-use load(s) since the level load (%.2f ms in all, %.2f ms at most); memory %.1f MB (media %.2f MB, structures %.2f MB); %d user-defined bank(s)\n",
		count[ EB_LOADED ], count[ EB_RESIDENT ], count[ EB_MISSING ], count[ EB_UNLOADED ], firstUseLoads, firstUseMs, firstUseMaxMs,
		total / 1048576.0, media / 1048576.0, structures / 1048576.0, loadedBanks.Num() );
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
/*
================
idSoundSystemWwise::ImageForTime

The image of a "soundMap" material: the HUD and PDA waveform while a radio message or an audio
log plays (D-058). Same drawing as idSoundSystemLocal::ImageForTime (snd_system.cpp), a 256 x 128
waveform or level meter of the last 4096 mixed frames, from the Wwise main output (capture callback)
instead of OpenAL's final mix. Game thread.
================
*/
static const int	WAVE_FRAMES = 4096;					// MIXBUFFER_SAMPLES
static dword		waveGraph[ 256 * 128 ];
static float		waveMix[ WAVE_FRAMES * 8 ];
static float		waveAccum[ WAVE_FRAMES * 6 ];
static int			waveMeterTops[ 256 ];
static int			waveMeterTopsTime[ 256 ];

cinData_t idSoundSystemWwise::ImageForTime( const int milliseconds, const bool waveform ) {
	cinData_t ret;
	memset( &ret, 0, sizeof( ret ) );
	if ( !started ) {
		return ret;
	}
	memset( waveGraph, 0, sizeof( waveGraph ) );

	// OpenAL's mix is 2 or 6 speakers in 16-bit range; Wwise's is -1..1 with its own channel count
	int channels = 0;
	const int frames = WwiseSDK::ReadMixTap( waveMix, WAVE_FRAMES, &channels );
	const int numSpeakers = ( channels == 6 ) ? 6 : 2;
	memset( waveAccum, 0, sizeof( waveAccum ) );
	for ( int f = 0; f < frames; f++ ) {
		for ( int c = 0; c < numSpeakers && c < channels; c++ ) {
			waveAccum[ f * numSpeakers + c ] = idMath::ClampFloat( -32768.0f, 32767.0f, waveMix[ f * channels + c ] * 32768.0f );
		}
		if ( channels == 1 ) {
			waveAccum[ f * numSpeakers + 1 ] = waveAccum[ f * numSpeakers ];
		}
	}
	const float *accum = waveAccum;
	const int time = Sys_Milliseconds();
	int i, j;

	if ( !waveform ) {
		for ( j = 0; j < numSpeakers; j++ ) {
			int meter = 0;
			for ( i = 0; i < WAVE_FRAMES; i++ ) {
				float result = idMath::Fabs( accum[i*numSpeakers+j] );
				if ( result > meter ) {
					meter = result;
				}
			}
			meter /= 256;		// 32768 becomes 128
			if ( meter > 128 ) {
				meter = 128;
			}
			int offset, xsize;
			if ( numSpeakers == 6 ) {
				offset = j * 40;
				xsize = 20;
			} else {
				offset = j * 128;
				xsize = 63;
			}
			dword color = 0xff00ff00;
			for ( int y = 0; y < 128; y++ ) {
				for ( int x = 0; x < xsize; x++ ) {
					waveGraph[(127-y)*256 + offset + x ] = color;
				}
				if ( y > meter ) {
					break;
				}
			}
			if ( meter > waveMeterTops[j] ) {
				waveMeterTops[j] = meter;
				waveMeterTopsTime[j] = time + cvarSystem->GetCVarInteger( "s_meterTopTime" );
			} else if ( time > waveMeterTopsTime[j] && waveMeterTops[j] > 0 ) {
				waveMeterTops[j]--;
				if ( waveMeterTops[j] ) {
					waveMeterTops[j]--;
				}
			}
		}
		for ( j = 0; j < numSpeakers; j++ ) {
			int meter = waveMeterTops[j];
			int offset, xsize;
			if ( numSpeakers == 6 ) {
				offset = j*40;
				xsize = 20;
			} else {
				offset = j*128;
				xsize = 63;
			}
			dword color;
			if ( meter <= 80 ) {
				color = 0xff007f00;
			} else if ( meter <= 112 ) {
				color = 0xff007f7f;
			} else {
				color = 0xff00007f;
			}
			for ( int y = meter; y < 128 && y < meter + 4; y++ ) {
				for ( int x = 0; x < xsize; x++ ) {
					waveGraph[(127-y)*256 + offset + x ] = color;
				}
			}
		}
	} else {
		dword colors[] = { 0xff007f00, 0xff007f7f, 0xff00007f, 0xff00ff00, 0xff00ffff, 0xff0000ff };

		for ( j = 0; j < numSpeakers; j++ ) {
			int xx = 0;
			const int step = WAVE_FRAMES / 256;
			for ( i = 0; i < WAVE_FRAMES; i += step ) {
				float fmeter = 0.0f;
				for ( int x = 0; x < step; x++ ) {
					fmeter += accum[(i+x)*numSpeakers+j] / 32768.0f;
				}
				fmeter /= 4.0f;
				if ( fmeter < -1.0f ) {
					fmeter = -1.0f;
				} else if ( fmeter > 1.0f ) {
					fmeter = 1.0f;
				}
				int meter = (int)( fmeter * 63.0f );
				waveGraph[ (meter + 64) * 256 + xx ] = colors[j];

				if ( meter < 0 ) {
					meter = -meter;
				}
				if ( meter > waveMeterTops[xx] ) {
					waveMeterTops[xx] = meter;
					waveMeterTopsTime[xx] = time + 100;
				} else if ( time > waveMeterTopsTime[xx] && waveMeterTops[xx] > 0 ) {
					waveMeterTops[xx]--;
					if ( waveMeterTops[xx] ) {
						waveMeterTops[xx]--;
					}
				}
				xx++;
			}
		}
		// OpenAL draws the envelope in the colour after the last speaker's (colors[numSpeakers]);
		// with 6 speakers that is past its array, so the last colour is used then
		const dword envelope = colors[ numSpeakers < 6 ? numSpeakers : 5 ];
		for ( i = 0; i < 256; i++ ) {
			int meter = waveMeterTops[i];
			for ( int y = -meter; y < meter; y++ ) {
				waveGraph[ (y+64)*256 + i ] = envelope;
			}
		}
	}
	ret.imageHeight = 128;
	ret.imageWidth = 256;
	ret.image = (unsigned char *)waveGraph;
	return ret;
}

// No dhewm3 decoders in Wwise mode (Wwise decodes its own media).
int idSoundSystemWwise::GetSoundDecoderInfo( int index, soundDecoderInfo_t &decoderInfo ) {
	return -1;
}

idSoundWorld * idSoundSystemWwise::AllocSoundWorld( idRenderWorld *rw ) {
	// rw is used by OpenAL for portal occlusion; the Wwise provider uses it only for s_drawSounds (D-067).
	idSoundWorldWwise *world = new idSoundWorldWwise( ++worldCount, rw );
	if ( playingWorld != NULL ) {
		world->SetActive( false );		// another world plays: this one is silent until it plays (S-095)
	}
	return world;
}

void idSoundSystemWwise::SetPlayingSoundWorld( idSoundWorld *soundWorld ) {
	// Only the Wwise provider's own worlds are ever passed in (all worlds come from AllocSoundWorld).
	idSoundWorldWwise *next = static_cast<idSoundWorldWwise *>( soundWorld );
	if ( next != playingWorld ) {
		// OpenAL mixes only the playing world: hold the sounds of the world left (the menu music after
		// the menu closes), release those of the world that plays now (S-095).
		if ( playingWorld != NULL ) {
			playingWorld->SetActive( false );
		}
		if ( next != NULL ) {
			next->SetActive( true );
		}
	}
	playingWorld = next;
	if ( playingWorld != NULL ) {
		playingWorld->ApplyListener();
	}
}

idSoundWorld * idSoundSystemWwise::GetPlayingSoundWorld( void ) {
	return playingWorld;
}

// Banks are loaded at start-up for now; per-map bank loading is a later step.

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

// An implicit shader (no .sndshd definition) plays the sample of its name; OpenAL looks for
// <name>.wav, then .ogg, and plays its missing-sound beep when neither exists (D-066).
bool idSoundSystemWwise::IsUndefinedShader( const char *shaderName, bool implicit ) {
	if ( !implicit ) {
		return false;
	}
	if ( undefinedShaders.FindIndex( shaderName ) >= 0 ) {
		return true;
	}
	if ( implicitWithFile.FindIndex( shaderName ) >= 0 ) {
		return false;
	}
	idStr wav = shaderName;
	wav.DefaultFileExtension( ".wav" );
	idStr ogg = wav;
	ogg.SetFileExtension( ".ogg" );
	if ( fileSystem->ReadFile( wav.c_str(), NULL ) != -1 || fileSystem->ReadFile( ogg.c_str(), NULL ) != -1 ) {
		implicitWithFile.Append( shaderName );
		return false;
	}
	undefinedShaders.Append( shaderName );
	common->Printf( "Wwise: sound shader %s has no definition and no sample file (map data error), not played\n", shaderName );
	return true;
}

// EFX is OpenAL's reverb; Wwise has its own effects.
int idSoundSystemWwise::IsEFXAvailable( void ) {
	return 0;
}
