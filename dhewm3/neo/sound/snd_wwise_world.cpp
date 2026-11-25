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
	slowmoActive = false;
	slowmoSpeed = 0.0f;
	enviroSuitActive = false;
}

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

// Sound class fades (used for cinematics and menus): mapped to Wwise in a later phase (triage item).
void idSoundWorldWwise::FadeSoundClasses( const int soundClass, const float to, const float over ) {
}

// Menu music and GUI sounds. Routed to Wwise events from Phase 4; logged for now.
void idSoundWorldWwise::PlayShaderDirectly( const char *name, int channel ) {
	if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() && name != NULL && name[0] != '\0' ) {
		common->Printf( "Wwise PlayShaderDirectly: world %d shader %s channel %d (not routed yet)\n", worldNumber, name, channel );
	}
}

// Demo recording of sound commands: not supported by the Wwise provider (triage item).
void idSoundWorldWwise::StartWritingDemo( idDemoFile *demo ) {
}

void idSoundWorldWwise::StopWritingDemo( void ) {
}

void idSoundWorldWwise::ProcessDemoCommand( idDemoFile *demo ) {
}

// Pause state is kept; pausing the Wwise voices comes with the game routes (Phase 4).
void idSoundWorldWwise::Pause( void ) {
	paused = true;
}

void idSoundWorldWwise::UnPause( void ) {
	paused = false;
}

bool idSoundWorldWwise::IsPaused( void ) {
	return paused;
}

// Writing the sound output next to AVI frames: not supported by the Wwise provider (triage item).
void idSoundWorldWwise::AVIOpen( const char *path, const char *name ) {
}

void idSoundWorldWwise::AVIClose( void ) {
}

/*
================
idSoundWorldWwise::WriteToSaveGame

Same layout as idSoundWorldLocal::WriteToSaveGame (snd_world.cpp), with no active
channels, so savegames stay loadable with either backend.
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

// One idSoundChannel record (idSoundWorldLocal::WriteToSaveGameSoundChannel), all idle.
static void WriteIdleChannel( idFile *savefile ) {
	soundShaderParms_t parms;
	memset( &parms, 0, sizeof( parms ) );
	savefile->WriteBool( false );			// triggerState
	savefile->WriteUnsignedChar( 0 );
	savefile->WriteUnsignedChar( 0 );
	savefile->WriteUnsignedChar( 0 );
	savefile->WriteInt( 0 );				// trigger44kHzTime
	savefile->WriteInt( 0 );				// triggerGame44kHzTime
	WriteParms( savefile, parms );
	savefile->WriteInt( 0 );				// leadinSample
	savefile->WriteInt( 0 );				// triggerChannel
	savefile->WriteInt( 0 );				// soundShader
	savefile->WriteInt( 0 );				// decoder
	savefile->WriteFloat( 0.0f );			// diversity
	savefile->WriteFloat( 0.0f );			// lastVolume
	for ( int m = 0; m < 6; m++ ) {
		savefile->WriteFloat( 0.0f );		// lastV
	}
	savefile->WriteInt( 0 );				// fadeStart44kHz
	savefile->WriteInt( 0 );				// fadeEnd44kHz
	savefile->WriteFloat( 0.0f );			// fadeStartVolume
	savefile->WriteFloat( 0.0f );			// fadeEndVolume
}

// Reads and discards one idSoundChannel record.
static void SkipChannel( idFile *savefile ) {
	bool b;
	char c;
	int i;
	float f;
	soundShaderParms_t parms;
	savefile->ReadBool( b );
	savefile->ReadChar( c );
	savefile->ReadChar( c );
	savefile->ReadChar( c );
	savefile->ReadInt( i );
	savefile->ReadInt( i );
	ReadParms( savefile, parms );
	savefile->ReadInt( i );
	savefile->ReadInt( i );
	savefile->ReadInt( i );
	savefile->ReadInt( i );
	savefile->ReadFloat( f );
	savefile->ReadFloat( f );
	for ( int m = 0; m < 6; m++ ) {
		savefile->ReadFloat( f );
	}
	savefile->ReadInt( i );
	savefile->ReadInt( i );
	savefile->ReadFloat( f );
	savefile->ReadFloat( f );
}

void idSoundWorldWwise::WriteToSaveGame( idFile *savefile ) {
	// listener data
	savefile->WriteVec3( listenerOrigin );
	savefile->WriteMat3( listenerAxis );
	savefile->WriteInt( listenerId );
	savefile->WriteInt( gameMsec );
	savefile->WriteInt( 0 );				// game44kHz
	savefile->WriteInt( 0 );				// current sound time

	savefile->WriteInt( emitters.Num() );
	for ( int i = 1; i < emitters.Num(); i++ ) {
		const idSoundEmitterWwise *def = emitters[i];
		if ( !def->IsAlive() ) {
			int skip = -1;
			savefile->Write( &skip, sizeof( skip ) );
			continue;
		}
		savefile->WriteInt( i );
		savefile->WriteVec3( def->Origin() );
		savefile->WriteInt( def->ListenerId() );
		WriteParms( savefile, def->Parms() );
		savefile->WriteFloat( 0.0f );		// amplitude
		savefile->WriteInt( 0 );			// ampTime
		for ( int k = 0; k < SOUND_MAX_CHANNELS; k++ ) {
			WriteIdleChannel( savefile );
		}
		savefile->WriteFloat( 0.0f );		// distance
		savefile->WriteBool( false );		// hasShakes
		savefile->WriteInt( -1 );			// lastValidPortalArea
		savefile->WriteFloat( 0.0f );		// maxDistance
		savefile->WriteBool( false );		// playing
		savefile->WriteFloat( 0.0f );		// realDistance
		savefile->WriteInt( REMOVE_STATUS_ALIVE );
		savefile->WriteVec3( def->Origin() );	// spatializedOrigin
		savefile->WriteInt( -1 );			// no active channels
	}

	savefile->Write( &slowmoActive, sizeof( slowmoActive ) );
	savefile->Write( &slowmoSpeed, sizeof( slowmoSpeed ) );
	savefile->Write( &enviroSuitActive, sizeof( enviroSuitActive ) );
}

/*
================
idSoundWorldWwise::ReadFromSaveGame

Reads the idSoundWorldLocal layout (from either backend). Restores the emitters
(the game asks for them by index right after this) and their positions. Sounds that
were playing when the game was saved are not restarted yet (Phase 4 and later).
================
*/
void idSoundWorldWwise::ReadFromSaveGame( idFile *savefile ) {
	int i, num, handle, channel, ignored;
	idStr name;

	ClearAllSoundEmitters();

	savefile->ReadVec3( listenerOrigin );
	savefile->ReadMat3( listenerAxis );
	savefile->ReadInt( listenerId );
	savefile->ReadInt( gameMsec );
	savefile->ReadInt( ignored );			// game44kHz
	savefile->ReadInt( ignored );			// saved sound time

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
		for ( int k = 0; k < SOUND_MAX_CHANNELS; k++ ) {
			SkipChannel( savefile );
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
			savefile->ReadString( name );
			savefile->ReadString( name );
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
}

// Slow motion and the enviro suit filter (Resurrection of Evil): state kept, no Wwise effect yet (triage items).
void idSoundWorldWwise::SetSlowmo( bool active ) {
	slowmoActive = active;
}

void idSoundWorldWwise::SetSlowmoSpeed( float speed ) {
	slowmoSpeed = speed;
}

void idSoundWorldWwise::SetEnviroSuit( bool active ) {
	enviroSuitActive = active;
}
