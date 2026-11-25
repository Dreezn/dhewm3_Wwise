/*
===========================================================================

dhewm3 Wwise backend: idSoundEmitterWwise, the Wwise provider of idSoundEmitter.

One emitter is one Wwise game object. Its ID is unique across sound worlds:
worldNumber * 0x100000 + index. IDs 1 and 2 are reserved (debug object, listener).

Phase 3: StartSound does not post events yet (it logs with s_wwiseShowStartSound).
Phase 4 maps sound shaders to Wwise events here. See snd_wwise.h.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/Common.h"

#include "sound/snd_wwise.h"
#include "sound/snd_wwise_sdk.h"

static const unsigned long long WWISE_WORLD_ID_STRIDE = 0x100000ULL;

idSoundEmitterWwise::idSoundEmitterWwise( idSoundWorldWwise *world, int index ) {
	this->world = world;
	this->index = index;
	gameObjectID = (unsigned long long)world->WorldNumber() * WWISE_WORLD_ID_STRIDE + (unsigned long long)index;
	alive = false;
	origin.Zero();
	listenerId = 0;
	memset( &parms, 0, sizeof( parms ) );
	WwiseSDK::RegisterObject( gameObjectID );
}

idSoundEmitterWwise::~idSoundEmitterWwise( void ) {
	WwiseSDK::StopObject( gameObjectID );
	WwiseSDK::UnregisterObject( gameObjectID );
}

// Marks the emitter as in use again (by AllocSoundEmitter or a savegame).
void idSoundEmitterWwise::Revive( void ) {
	alive = true;
	listenerId = 0;
	memset( &parms, 0, sizeof( parms ) );
	playing.Clear();
}

void idSoundEmitterWwise::RestoreState( const idVec3 &origin, int listenerId, const soundShaderParms_t &parms ) {
	UpdateEmitter( origin, listenerId, &parms );
}

/*
================
idSoundEmitterWwise::Free

A non-immediate free lets playing sounds finish (Wwise keeps playing them);
an immediate free stops them. Either way the emitter can be reused at once.
================
*/
void idSoundEmitterWwise::Free( bool immediate ) {
	if ( immediate ) {
		StopSound( SCHANNEL_ANY );
	}
	playing.Clear();
	alive = false;
}

void idSoundEmitterWwise::UpdateEmitter( const idVec3 &origin, int listenerId, const soundShaderParms_t *parms ) {
	this->origin = origin;
	this->listenerId = listenerId;
	if ( parms != NULL ) {
		this->parms = *parms;
	}
	static const float front[3] = { 0.0f, 0.0f, 1.0f };
	static const float top[3] = { 0.0f, 1.0f, 0.0f };
	float pos[3];
	idSoundWorldWwise::ToWwisePosition( origin, pos );
	WwiseSDK::SetObjectPosition( gameObjectID, pos, front, top );
}

/*
================
idSoundEmitterWwise::StartSound

Returns the length of the started sound in msec. Nothing is posted in Phase 3.
================
*/
int idSoundEmitterWwise::StartSound( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags, bool allowSlow ) {
	if ( shader == NULL ) {
		return 0;
	}
	if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() ) {
		common->Printf( "Wwise StartSound: emitter %d (object %llu) channel %d shader %s (not routed yet)\n",
			index, gameObjectID, (int)channel, shader->GetName() );
	}
	return 0;
}

// Per-channel parameter changes (volume, distances): mapped to RTPCs in a later phase.
void idSoundEmitterWwise::ModifySound( const s_channelType channel, const soundShaderParms_t *parms ) {
}

void idSoundEmitterWwise::StopSound( const s_channelType channel ) {
	if ( channel == SCHANNEL_ANY ) {
		WwiseSDK::StopObject( gameObjectID );
		playing.Clear();
		return;
	}
	for ( int i = playing.Num() - 1; i >= 0; i-- ) {
		if ( playing[i].channel == channel ) {
			WwiseSDK::StopPlayingID( playing[i].playingID );
			playing.RemoveIndex( i );
		}
	}
}

// Volume fades per channel: mapped to Wwise in a later phase.
void idSoundEmitterWwise::FadeSound( const s_channelType channel, float to, float over ) {
}

// Playing IDs are tracked from Phase 4; until then nothing plays on an emitter.
bool idSoundEmitterWwise::CurrentlyPlaying( void ) const {
	return playing.Num() > 0;
}

// Amplitude-driven effects (light flicker with sound): no Wwise equivalent yet (triage item).
float idSoundEmitterWwise::CurrentAmplitude( void ) {
	return 0.0f;
}

int idSoundEmitterWwise::Index( void ) const {
	return index;
}
