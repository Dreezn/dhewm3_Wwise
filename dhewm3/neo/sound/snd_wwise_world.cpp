/*
===========================================================================

dhewm3 Wwise backend: idSoundWorldWwise, the Wwise provider of idSoundWorld.

Holds the listener and the emitters of one dhewm3 sound world. See snd_wwise.h.

Coordinates: dhewm3 uses inches with X forward, Y left, Z up (right-handed).
Wwise uses X right, Y up, Z forward (left-handed). Positions are sent to Wwise
in meters (DOOM_TO_METERS), the unit the Doom 3 sound shaders already use for
minDistance / maxDistance.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/File.h"
#include "framework/Session.h"
#include "framework/DeclManager.h"
#include "idlib/Lexer.h"
#include "idlib/math/Random.h"

#include "sound/snd_wwise.h"
#include "sound/snd_wwise_sdk.h"

// Savegame layout constants of the OpenAL provider (idSoundWorldLocal::WriteToSaveGame).
// The Wwise provider writes and reads the same layout, so a savegame can be loaded with either backend.
#include "sound/snd_local.h"

/*
================
idSoundWorldWwise::ToWwisePosition / ToWwiseDirection
================
*/
void idSoundWorldWwise::ToWwisePosition( const idVec3 &doomPos, float out[3] ) {
	out[0] = -doomPos.y * DOOM_TO_METERS;	// right = -left
	out[1] =  doomPos.z * DOOM_TO_METERS;	// up
	out[2] =  doomPos.x * DOOM_TO_METERS;	// forward
}

void idSoundWorldWwise::ToWwiseDirection( const idVec3 &doomDir, float out[3] ) {
	out[0] = -doomDir.y;
	out[1] =  doomDir.z;
	out[2] =  doomDir.x;
}

idSoundWorldWwise::idSoundWorldWwise( int worldNumber ) {
	this->worldNumber = worldNumber;
	emitters.Append( NULL );		// index 0 means "no emitter" in savegames and demos
	listenerOrigin.Zero();
	listenerAxis.Identity();
	listenerId = 0;
	gameMsec = 0;
	paused = false;
	inactive = false;
	silenced = false;
	slowmoActive = false;
	slowmoSpeed = 0.0f;
	enviroSuitActive = false;
	localSound = NULL;
}

// Freed emitters' sounds kept for Pause (AdoptPlaying). Old IDs that finished are harmless; the list is capped.
static const int WWISE_MAX_ORPHANS = 256;

// Sound class volumes (FadeSoundClasses) and slow motion pitch: global game parameters built by
// tools/wwise_doom_sounds.js. The enviro suit is a state the sound designers can bind to an effect.
static const char *RTPC_SOUNDCLASS_PREFIX = "RTPC_SoundClass";
static const char *RTPC_SLOWMO = "RTPC_Slowmo";
static const char *STATE_ENVIROSUIT = "EnviroSuit";

idSoundWorldWwise::~idSoundWorldWwise( void ) {
	if ( soundSystemWwise.GetPlayingSoundWorld() == this ) {
		soundSystemWwise.SetPlayingSoundWorld( NULL );
	}
	for ( int i = 1; i < emitters.Num(); i++ ) {
		delete emitters[i];		// unregisters its game object
	}
	emitters.Clear();
}

idSoundEmitterWwise * idSoundWorldWwise::NewEmitter( void ) {
	idSoundEmitterWwise *def = new idSoundEmitterWwise( this, emitters.Num() );
	emitters.Append( def );
	return def;
}

/*
================
idSoundWorldWwise::ClearAllSoundEmitters

Called at each map start: stops and frees every emitter (they stay allocated for reuse).
================
*/
void idSoundWorldWwise::ClearAllSoundEmitters( void ) {
	for ( int i = 1; i < emitters.Num(); i++ ) {
		emitters[i]->Free( true );
	}
	localSound = NULL;		// as idSoundWorldLocal::ClearAllSoundEmitters
	orphanIDs.Clear();
}

void idSoundWorldWwise::AdoptPlaying( unsigned int playingID ) {
	if ( orphanIDs.Num() >= WWISE_MAX_ORPHANS ) {
		orphanIDs.RemoveIndex( 0 );
	}
	orphanIDs.Append( playingID );
}

void idSoundWorldWwise::StopAllSounds( void ) {
	for ( int i = 1; i < emitters.Num(); i++ ) {
		emitters[i]->StopSound( SCHANNEL_ANY );
	}
}

idSoundEmitter * idSoundWorldWwise::AllocSoundEmitter( void ) {
	for ( int i = 1; i < emitters.Num(); i++ ) {
		if ( !emitters[i]->IsAlive() ) {
			emitters[i]->Revive();
			return emitters[i];
		}
	}
	idSoundEmitterWwise *def = NewEmitter();
	def->Revive();
	return def;
}

idSoundEmitter * idSoundWorldWwise::EmitterForIndex( int index ) {
	if ( index == 0 ) {
		return NULL;
	}
	if ( index >= emitters.Num() ) {
		common->Error( "idSoundWorldWwise::EmitterForIndex: %i > %i", index, emitters.Num() );
	}
	return emitters[index];
}

// Screen shake from loud sounds: no Wwise equivalent yet (triage item).
float idSoundWorldWwise::CurrentShakeAmplitudeForPosition( const int time, const idVec3 &listenerPosition ) {
	return 0.0f;
}

/*
================
idSoundWorldWwise::PlaceListener

Stores the camera position; the Wwise listener follows the playing world only.
================
*/
void idSoundWorldWwise::PlaceListener( const idVec3 &origin, const idMat3 &axis, const int listenerId, const int gameTime, const idStr& areaName ) {
	if ( paused ) {
		return;
	}
	listenerOrigin = origin;
	listenerAxis = axis;
	this->listenerId = listenerId;
	gameMsec = gameTime;
	if ( soundSystemWwise.GetPlayingSoundWorld() == this ) {
		ApplyListener();
	}
}

void idSoundWorldWwise::ApplyListener( void ) const {
	float pos[3], front[3], top[3];
	ToWwisePosition( listenerOrigin, pos );
	ToWwiseDirection( listenerAxis[0], front );		// axis[0]: view direction
	ToWwiseDirection( listenerAxis[2], top );		// axis[2]: up
	WwiseSDK::SetListenerPosition( pos, front, top );
}

/*
================
idSoundWorldWwise::FadeSoundClasses

Fades every sound of a sound class to "to" dB over "over" seconds (cinematics, menus):
the global RTPC_SoundClass<n>, which drives the volume of the bus SoundClass_<n>.
================
*/
void idSoundWorldWwise::FadeSoundClasses( const int soundClass, const float to, const float over ) {
	if ( soundClass < 0 || soundClass >= SOUND_MAX_CLASSES ) {
		common->Error( "idSoundWorldWwise::FadeSoundClasses: bad soundClass %i", soundClass );
	}
	const float value = idMath::ClampFloat( -96.0f, 0.0f, to );
	const int ms = over > 0.0f ? (int)( over * 1000.0f ) : 0;
	WwiseSDK::SetGlobalRTPC( va( "%s%d", RTPC_SOUNDCLASS_PREFIX, soundClass ), value, ms );
}

/*
================
idSoundWorldWwise::PlayShaderDirectly

Menu music and GUI sounds, with the rules of idSoundWorldLocal::PlayShaderDirectly: channel -1
stops everything of the local sound first, another channel stops that channel; an empty name
only stops. The sound is global (SSF_GLOBAL), so it plays on the at-listener game object.
================
*/
void idSoundWorldWwise::PlayShaderDirectly( const char *name, int channel ) {
	if ( localSound != NULL ) {
		localSound->StopSound( channel == -1 ? SCHANNEL_ANY : (s_channelType)channel );
	}
	if ( name == NULL || name[0] == '\0' ) {
		return;
	}
	const idSoundShader *shader = declManager->FindSound( name );
	if ( shader == NULL ) {
		return;
	}
	if ( localSound == NULL ) {
		localSound = static_cast<idSoundEmitterWwise *>( AllocSoundEmitter() );
	}
	static idRandom rnd;
	localSound->StartSound( shader, channel == -1 ? SCHANNEL_ONE : (s_channelType)channel, rnd.RandomFloat(), SSF_GLOBAL, true );
}

// Demo recording of sound commands: not supported by the Wwise provider (triage item).
/*
================
Demos (recordDemo, playDemo, timeDemo): not supported by the Wwise provider (D-059). A demo
recorded with Wwise has no sound commands. A demo recorded with OpenAL has them: each one is
read and discarded here, in the layout of idSoundWorldLocal::ProcessDemoCommand, so the demo
stays in step and plays without sound.
================
*/
void idSoundWorldWwise::StartWritingDemo( idDemoFile *demo ) {
	soundSystemWwise.WarnOnce( "demo:record", "Wwise: demos are %s without sound (not supported by the Wwise provider)", "recorded" );
}

void idSoundWorldWwise::StopWritingDemo( void ) {
}

void idSoundWorldWwise::ProcessDemoCommand( idDemoFile *demo ) {
	if ( demo == NULL ) {
		return;
	}
	soundSystemWwise.WarnOnce( "demo:play", "Wwise: demos are %s without sound (not supported by the Wwise provider)", "played" );
	int dc, i;
	float f;
	idVec3 v;
	idMat3 m;
	if ( !demo->ReadInt( dc ) ) {
		return;
	}
	switch ( (soundDemoCommand_t)dc ) {
	case SCMD_STATE:
		// a whole sound world in the savegame layout: read it, then drop what it restored
		ReadFromSaveGame( demo );
		ClearAllSoundEmitters();
		paused = false;
		UpdateSilence();
		break;
	case SCMD_PLACE_LISTENER:
		demo->ReadVec3( v );
		demo->ReadMat3( m );
		demo->ReadInt( i );				// listenerId
		demo->ReadInt( i );				// gameTime
		break;
	case SCMD_ALLOC_EMITTER:
		demo->ReadInt( i );
		break;
	case SCMD_FREE:
		demo->ReadInt( i );
		demo->ReadInt( i );				// immediate
		break;
	case SCMD_UPDATE:
		demo->ReadInt( i );
		demo->ReadVec3( v );
		demo->ReadInt( i );				// listenerId
		for ( int k = 0; k < 4; k++ ) {
			demo->ReadFloat( f );		// min, max, volume, shakes
		}
		demo->ReadInt( i );				// flags
		demo->ReadInt( i );				// soundClass
		break;
	case SCMD_START:
		demo->ReadInt( i );
		demo->ReadHashString();			// shader
		demo->ReadInt( i );				// channel
		demo->ReadFloat( f );			// diversity
		demo->ReadInt( i );				// flags
		break;
	case SCMD_MODIFY:
		demo->ReadInt( i );
		demo->ReadInt( i );				// channel
		for ( int k = 0; k < 4; k++ ) {
			demo->ReadFloat( f );
		}
		demo->ReadInt( i );
		demo->ReadInt( i );
		break;
	case SCMD_STOP:
		demo->ReadInt( i );
		demo->ReadInt( i );				// channel
		break;
	case SCMD_FADE:
		demo->ReadInt( i );
		demo->ReadInt( i );				// channel
		demo->ReadFloat( f );			// to
		demo->ReadFloat( f );			// over
		break;
	}
}

// Holds or releases every sound of this world (the emitters' and the freed emitters' that still play)
// when "paused by the game or not the playing world" changes.
void idSoundWorldWwise::UpdateSilence( void ) {
	const bool want = paused || inactive;
	if ( want == silenced ) {
		return;
	}
	silenced = want;
	for ( int i = 1; i < emitters.Num(); i++ ) {
		emitters[i]->PauseAll( want );
	}
	for ( int i = 0; i < orphanIDs.Num(); i++ ) {
		if ( want ) {
			WwiseSDK::PausePlayingID( orphanIDs[i] );
		} else {
			WwiseSDK::ResumePlayingID( orphanIDs[i] );
		}
	}
}

void idSoundWorldWwise::Pause( void ) {
	paused = true;
	UpdateSilence();
}

void idSoundWorldWwise::UnPause( void ) {
	paused = false;
	UpdateSilence();
}

void idSoundWorldWwise::SetActive( bool active ) {
	inactive = !active;
	UpdateSilence();
}

bool idSoundWorldWwise::IsPaused( void ) {
	return paused;
}

// Writing the sound output next to AVI frames: not supported by the Wwise provider (triage item).
// AVI capture (aviDemo, aviGame) renders sound in game time; not supported by the Wwise
// provider (D-059). s_wwiseCapture records the Wwise output in real time.
void idSoundWorldWwise::AVIOpen( const char *path, const char *name ) {
	soundSystemWwise.WarnOnce( "avi", "Wwise: %s writes no sound (not supported by the Wwise provider; s_wwiseCapture records the output)", "AVI capture" );
}

void idSoundWorldWwise::AVIClose( void ) {
}

/*
================
idSoundWorldWwise::WriteToSaveGame

Same layout as idSoundWorldLocal::WriteToSaveGame (snd_world.cpp), with the sounds
still playing as active channels (D-057), so savegames, and the sounds in them, load
with either backend.
================
*/
static void WriteParms( idFile *savefile, const soundShaderParms_t &parms ) {
	savefile->WriteFloat( parms.minDistance );
	savefile->WriteFloat( parms.maxDistance );
	savefile->WriteFloat( parms.volume );
	savefile->WriteFloat( parms.shakes );
	savefile->WriteInt( parms.soundShaderFlags );
	savefile->WriteInt( parms.soundClass );
}

static void ReadParms( idFile *savefile, soundShaderParms_t &parms ) {
	savefile->ReadFloat( parms.minDistance );
	savefile->ReadFloat( parms.maxDistance );
	savefile->ReadFloat( parms.volume );
	savefile->ReadFloat( parms.shakes );
	savefile->ReadInt( parms.soundShaderFlags );
	savefile->ReadInt( parms.soundClass );
}

/*
Savegame sound time (D-057). The OpenAL layout keeps times in 44.1 kHz samples, and only
their differences matter: the header holds the sound time at the save, each channel its
start and fade times. The Wwise provider writes the save at SAVE_SOUND_TIME and every
time relative to it; positions and fades up to an hour before the save fit.
*/
static const int SAVE_SOUND_TIME = 44100 * 3600;
static const int SAVE_MAX_BACK_MS = 3500 * 1000;

static int MsToSaveTime( int relativeMs ) {
	relativeMs = idMath::ClampInt( -SAVE_MAX_BACK_MS, SAVE_MAX_BACK_MS, relativeMs );
	return SAVE_SOUND_TIME + (int)( (long long)relativeMs * 441 / 10 );
}

static int SaveTimeToMs( int time44kHz, int savedSoundTime ) {
	return (int)( (long long)( time44kHz - savedSoundTime ) * 10 / 441 );
}

// One idSoundChannel record (idSoundWorldLocal::WriteToSaveGameSoundChannel); sound NULL is idle.
static void WriteChannel( idFile *savefile, const idSoundEmitterWwise::savedSound_t *sound ) {
	soundShaderParms_t idle;
	memset( &idle, 0, sizeof( idle ) );
	const int trigger = sound != NULL ? MsToSaveTime( -sound->elapsedMs ) : 0;
	savefile->WriteBool( sound != NULL );	// triggerState
	savefile->WriteUnsignedChar( 0 );
	savefile->WriteUnsignedChar( 0 );
	savefile->WriteUnsignedChar( 0 );
	savefile->WriteInt( trigger );			// trigger44kHzTime
	savefile->WriteInt( trigger );			// triggerGame44kHzTime
	WriteParms( savefile, sound != NULL ? sound->parms : idle );
	savefile->WriteInt( 0 );				// leadinSample
	savefile->WriteInt( sound != NULL ? sound->channel : 0 );	// triggerChannel
	savefile->WriteInt( 0 );				// soundShader
	savefile->WriteInt( 0 );				// decoder
	savefile->WriteFloat( sound != NULL ? sound->diversity : 0.0f );
	savefile->WriteFloat( 0.0f );			// lastVolume
	for ( int m = 0; m < 6; m++ ) {
		savefile->WriteFloat( 0.0f );		// lastV
	}
	const bool fade = sound != NULL && sound->hasFade;
	savefile->WriteInt( fade ? MsToSaveTime( sound->fadeStartMs ) : 0 );	// fadeStart44kHz (0: no fade)
	savefile->WriteInt( fade ? MsToSaveTime( sound->fadeEndMs ) : 0 );		// fadeEnd44kHz
	savefile->WriteFloat( fade ? sound->fadeFrom : 0.0f );					// fadeStartVolume
	savefile->WriteFloat( fade ? sound->fadeTo : 0.0f );					// fadeEndVolume
}

// Reads one idSoundChannel record into sound (shader not set). Returns its triggerState.
static bool ReadChannel( idFile *savefile, int savedSoundTime, idSoundEmitterWwise::savedSound_t &sound ) {
	bool triggerState;
	char c;
	int i, trigger, fadeStart, fadeEnd;
	float f;
	memset( &sound, 0, sizeof( sound ) );
	savefile->ReadBool( triggerState );
	savefile->ReadChar( c );
	savefile->ReadChar( c );
	savefile->ReadChar( c );
	savefile->ReadInt( trigger );			// trigger44kHzTime
	savefile->ReadInt( i );					// triggerGame44kHzTime
	ReadParms( savefile, sound.parms );
	savefile->ReadInt( i );					// leadinSample
	savefile->ReadInt( sound.channel );		// triggerChannel
	savefile->ReadInt( i );					// soundShader
	savefile->ReadInt( i );					// decoder
	savefile->ReadFloat( sound.diversity );
	savefile->ReadFloat( f );				// lastVolume
	for ( int m = 0; m < 6; m++ ) {
		savefile->ReadFloat( f );			// lastV
	}
	savefile->ReadInt( fadeStart );
	savefile->ReadInt( fadeEnd );
	savefile->ReadFloat( sound.fadeFrom );
	savefile->ReadFloat( sound.fadeTo );

	sound.elapsedMs = idMath::ClampInt( 0, SAVE_MAX_BACK_MS, SaveTimeToMs( savedSoundTime, trigger ) );
	sound.hasFade = ( fadeStart != 0 );
	if ( sound.hasFade ) {
		sound.fadeStartMs = SaveTimeToMs( fadeStart, savedSoundTime );
		sound.fadeEndMs = SaveTimeToMs( fadeEnd, savedSoundTime );
	}
	return triggerState;
}

/*
The lead-in sample name of the OpenAL layout: the first lead-in or sample of the shader,
read from its text (the Wwise provider loads no samples). OpenAL plays that sample after
loading; it cannot know which random variation Wwise had chosen.
*/
static bool FirstSampleName( const idSoundShader *shader, idStr &name ) {
	const int length = shader->GetTextLength();
	if ( length <= 0 ) {
		// An implicit shader (a sample named directly): the decl manager frees its generated text
		// after parsing. Its sample is the name with .wav, as idSoundShader::SetDefaultText makes it.
		name = shader->GetName();
		name.DefaultFileExtension( ".wav" );
		return true;
	}
	char *text = (char *)Mem_Alloc( length + 1 );
	shader->GetText( text );
	idLexer src( text, length, shader->GetFileName(), LEXFL_NOSTRINGCONCAT | LEXFL_ALLOWPATHNAMES | LEXFL_ALLOWMULTICHARLITERALS | LEXFL_ALLOWBACKSLASHSTRINGCONCAT | LEXFL_NOFATALERRORS );
	idToken token;
	bool found = false;
	bool leadin = false;
	while ( !found && src.ReadToken( &token ) ) {
		if ( leadin || token.Find( ".wav", false ) != -1 || token.Find( ".ogg", false ) != -1 ) {
			name = token;
			name.BackSlashesToSlashes();
			found = true;
		}
		leadin = ( token.Icmp( "leadin" ) == 0 );
	}
	Mem_Free( text );
	return found;
}

void idSoundWorldWwise::WriteToSaveGame( idFile *savefile ) {
	// listener data
	savefile->WriteVec3( listenerOrigin );
	savefile->WriteMat3( listenerAxis );
	savefile->WriteInt( listenerId );
	savefile->WriteInt( gameMsec );
	savefile->WriteInt( 0 );				// game44kHz
	savefile->WriteInt( SAVE_SOUND_TIME );	// current sound time

	savefile->WriteInt( emitters.Num() );
	for ( int i = 1; i < emitters.Num(); i++ ) {
		idSoundEmitterWwise *def = emitters[i];
		if ( !def->IsAlive() ) {
			int skip = -1;
			savefile->Write( &skip, sizeof( skip ) );
			continue;
		}
		// the sounds still playing, each in one channel record; a sound without a sample
		// name (needed by the OpenAL layout) is left out
		idSoundEmitterWwise::savedSound_t sounds[ idSoundEmitterWwise::MAX_SAVED_SOUNDS ];
		idStr sampleNames[ idSoundEmitterWwise::MAX_SAVED_SOUNDS ];
		const int found = def->SaveSounds( sounds );
		int numSounds = 0;
		for ( int k = 0; k < found; k++ ) {
			if ( FirstSampleName( sounds[k].shader, sampleNames[numSounds] ) ) {
				sounds[numSounds++] = sounds[k];
			} else {
				common->Warning( "Wwise savegame: no sample name for %s, its sound is not saved", sounds[k].shader->GetName() );
			}
		}

		savefile->WriteInt( i );
		savefile->WriteVec3( def->Origin() );
		savefile->WriteInt( def->ListenerId() );
		WriteParms( savefile, def->Parms() );
		savefile->WriteFloat( 0.0f );		// amplitude
		savefile->WriteInt( 0 );			// ampTime
		for ( int k = 0; k < SOUND_MAX_CHANNELS; k++ ) {
			WriteChannel( savefile, k < numSounds ? &sounds[k] : NULL );
		}
		savefile->WriteFloat( 0.0f );		// distance
		savefile->WriteBool( false );		// hasShakes
		savefile->WriteInt( -1 );			// lastValidPortalArea
		// OpenAL recomputes the distances, but only mixes an emitter whose playing flag is set
		float maxDistance = 0.0f;
		for ( int k = 0; k < numSounds; k++ ) {
			maxDistance = Max( maxDistance, sounds[k].parms.maxDistance );
		}
		savefile->WriteFloat( maxDistance );	// maxDistance
		savefile->WriteBool( numSounds > 0 );	// playing
		savefile->WriteFloat( 0.0f );		// realDistance
		savefile->WriteInt( REMOVE_STATUS_ALIVE );
		savefile->WriteVec3( def->Origin() );	// spatializedOrigin
		// active channels: slot, shader name, lead-in sample name; -1 ends the list
		for ( int k = 0; k < numSounds; k++ ) {
			savefile->WriteInt( k );
			savefile->WriteString( sounds[k].shader->GetName() );
			savefile->WriteString( sampleNames[k] );
		}
		savefile->WriteInt( -1 );
	}

	savefile->Write( &slowmoActive, sizeof( slowmoActive ) );
	savefile->Write( &slowmoSpeed, sizeof( slowmoSpeed ) );
	savefile->Write( &enviroSuitActive, sizeof( enviroSuitActive ) );
}

/*
================
idSoundWorldWwise::ReadFromSaveGame

Reads the idSoundWorldLocal layout (from either backend). Restores the emitters
(the game asks for them by index right after this) and their positions, then starts
the sounds that were playing again (D-057, section 8.6).
================
*/
void idSoundWorldWwise::ReadFromSaveGame( idFile *savefile ) {
	int i, num, handle, channel, ignored, savedSoundTime;
	idStr name;

	struct restart_t {
		int									emitter;
		idSoundEmitterWwise::savedSound_t	sound;
	};
	idList<restart_t> restarts;

	ClearAllSoundEmitters();

	savefile->ReadVec3( listenerOrigin );
	savefile->ReadMat3( listenerAxis );
	savefile->ReadInt( listenerId );
	savefile->ReadInt( gameMsec );
	savefile->ReadInt( ignored );			// game44kHz
	savefile->ReadInt( savedSoundTime );

	savefile->ReadInt( num );
	while ( emitters.Num() < num ) {
		NewEmitter();
	}

	for ( i = 1; i < num; i++ ) {
		savefile->ReadInt( handle );
		if ( handle < 0 ) {
			continue;
		}
		if ( handle != i ) {
			common->Error( "idSoundWorldWwise::ReadFromSaveGame: index mismatch" );
		}

		idVec3 origin, spatializedOrigin;
		int emitterListenerId, removeStatus;
		soundShaderParms_t parms;
		float f;
		bool b;

		savefile->ReadVec3( origin );
		savefile->ReadInt( emitterListenerId );
		ReadParms( savefile, parms );
		savefile->ReadFloat( f );			// amplitude
		savefile->ReadInt( ignored );		// ampTime
		idSoundEmitterWwise::savedSound_t channels[ SOUND_MAX_CHANNELS ];
		bool triggered[ SOUND_MAX_CHANNELS ];
		for ( int k = 0; k < SOUND_MAX_CHANNELS; k++ ) {
			triggered[k] = ReadChannel( savefile, savedSoundTime, channels[k] );
		}
		savefile->ReadFloat( f );			// distance
		savefile->ReadBool( b );			// hasShakes
		savefile->ReadInt( ignored );		// lastValidPortalArea
		savefile->ReadFloat( f );			// maxDistance
		savefile->ReadBool( b );			// playing
		savefile->ReadFloat( f );			// realDistance
		savefile->ReadInt( removeStatus );
		savefile->ReadVec3( spatializedOrigin );

		// active channels: shader name and lead-in sample name, until -1
		savefile->ReadInt( channel );
		while ( channel >= 0 ) {
			if ( channel >= SOUND_MAX_CHANNELS ) {
				common->Error( "idSoundWorldWwise::ReadFromSaveGame: channel >= SOUND_MAX_CHANNELS" );
			}
			savefile->ReadString( name );		// shader
			if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() ) {
				common->Printf( "Wwise savegame: emitter %d slot %d %s, %s, channel %d, %d ms\n", i, channel, name.c_str(),
					triggered[channel] ? "playing" : "not triggered", channels[channel].channel, channels[channel].elapsedMs );
			}
			if ( triggered[channel] ) {
				restart_t r;
				r.emitter = i;
				r.sound = channels[channel];
				r.sound.shader = declManager->FindSound( name );
				restarts.Append( r );
			}
			savefile->ReadString( name );		// lead-in sample (OpenAL only)
			savefile->ReadInt( channel );
		}

		emitters[i]->Revive();
		emitters[i]->RestoreState( origin, emitterListenerId, parms );
	}

	if ( session->GetSaveGameVersion() >= 17 ) {
		savefile->Read( &slowmoActive, sizeof( slowmoActive ) );
		savefile->Read( &slowmoSpeed, sizeof( slowmoSpeed ) );
		savefile->Read( &enviroSuitActive, sizeof( enviroSuitActive ) );
	} else {
		slowmoActive = false;
		slowmoSpeed = 0.0f;
		enviroSuitActive = false;
	}

	if ( soundSystemWwise.GetPlayingSoundWorld() == this ) {
		ApplyListener();
	}
	ApplySlowmo();
	WwiseSDK::SetState( STATE_ENVIROSUIT, enviroSuitActive ? "On" : "Off" );

	// As OpenAL (pause44kHz in idSoundWorldLocal::ReadFromSaveGame), a loaded world stays paused
	// until the session unpauses it after the level load ("when loading a save game the sound is
	// paused", Session.cpp), so restored sounds do not run on under the muted load screen.
	paused = true;
	UpdateSilence();

	// after the listener: private and global sounds depend on it
	int started = 0;
	for ( i = 0; i < restarts.Num(); i++ ) {
		if ( emitters[ restarts[i].emitter ]->RestartSound( restarts[i].sound ) ) {
			started++;
		}
	}
	if ( restarts.Num() > 0 ) {
		common->Printf( "Wwise: restored %d of the %d sounds playing in the savegame\n", started, restarts.Num() );
	}
}

/*
================
idSoundWorldWwise::SetSlowmo / SetSlowmoSpeed / SetEnviroSuit

Slow motion (Resurrection of Evil): OpenAL plays at pitch slowmoSpeed while active. Here
the global RTPC_Slowmo drives the pitch; sounds started with !allowSlow keep 1 on their
playing ID. The enviro suit is the state EnviroSuit (On / Off), for the designers to bind.
================
*/
void idSoundWorldWwise::ApplySlowmo( void ) const {
	const float value = slowmoActive ? idMath::ClampFloat( 0.1f, 1.0f, slowmoSpeed ) : 1.0f;
	WwiseSDK::SetGlobalRTPC( RTPC_SLOWMO, value, 0 );
}

void idSoundWorldWwise::SetSlowmo( bool active ) {
	slowmoActive = active;
	ApplySlowmo();
}

void idSoundWorldWwise::SetSlowmoSpeed( float speed ) {
	slowmoSpeed = speed;
	ApplySlowmo();
}

void idSoundWorldWwise::SetEnviroSuit( bool active ) {
	enviroSuitActive = active;
	WwiseSDK::SetState( STATE_ENVIROSUIT, active ? "On" : "Off" );
}
