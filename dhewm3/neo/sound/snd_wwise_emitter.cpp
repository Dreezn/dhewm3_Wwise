/*
===========================================================================

dhewm3 Wwise backend: idSoundEmitterWwise, the Wwise provider of idSoundEmitter.

One emitter is one Wwise game object. Its ID is unique across sound worlds:
worldNumber * 0x100000 + index. IDs 1 and 2 are reserved (debug object, listener).

StartSound posts the event Play_<shader> (D-052). Code keeps only the channel
contract of sound.h; sound behaviour is Wwise event setup. See snd_wwise.h.

===========================================================================
*/

#include "sys/platform.h"
#include "sys/sys_public.h"
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

void idSoundEmitterWwise::EventNameForShader( const char *shaderName, idStr &eventName ) {
	eventName = "Play_";
	for ( const char *c = shaderName; *c != '\0'; c++ ) {
		const bool keep = ( *c >= 'a' && *c <= 'z' ) || ( *c >= 'A' && *c <= 'Z' ) || ( *c >= '0' && *c <= '9' ) || *c == '_';
		eventName.Append( keep ? *c : '_' );
	}
}

/*
================
idSoundEmitterWwise::StartSound

Posts Play_<shader> on this emitter's game object (D-052). A missing event gives
Wwise's own warning and is listed by wwise_missing. What the sound does (random
choice, play once, looping, limits) is set up in the Wwise event, not here.

Returns the length of the started sound in msec: 0 for now (Phase 5).
================
*/
int idSoundEmitterWwise::StartSound( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags, bool allowSlow ) {
	if ( shader == NULL ) {
		return 0;
	}

	// Channel contract of sound.h: a new sound on a channel replaces the one playing there.
	if ( channel != SCHANNEL_ANY ) {
		StopSound( channel );
	}

	idStr eventName;
	EventNameForShader( shader->GetName(), eventName );
	unsigned int playingID = WwiseSDK::PostEventOnObject( eventName.c_str(), gameObjectID );

	if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() ) {
		common->Printf( "Wwise StartSound: emitter %d channel %d %s -> %s\n",
			index, (int)channel, eventName.c_str(), playingID != 0 ? "posted" : "no event" );
	}
	if ( playingID == 0 ) {
		soundSystemWwise.ReportMissing( shader->GetName() );
		return 0;
	}

	PrunePlaying();
	playingSound_t sound;
	sound.channel = channel;
	sound.playingID = playingID;
	sound.postTime = Sys_Milliseconds();
	playing.Append( sound );
	return 0;
}

/*
================
idSoundEmitterWwise::PrunePlaying

Drops playing IDs that Wwise no longer plays. An event posted in the last 500 ms is
kept even if Wwise does not list it yet: posts are only processed at the next RenderAudio.
================
*/
void idSoundEmitterWwise::PrunePlaying( void ) const {
	if ( playing.Num() == 0 ) {
		return;
	}
	unsigned int active[ 64 ];
	int numActive = WwiseSDK::PlayingIDsOnObject( gameObjectID, active, 64 );
	int now = Sys_Milliseconds();
	for ( int i = playing.Num() - 1; i >= 0; i-- ) {
		if ( now - playing[i].postTime < 500 ) {
			continue;
		}
		bool found = false;
		for ( int j = 0; j < numActive && !found; j++ ) {
			found = ( active[j] == playing[i].playingID );
		}
		if ( !found ) {
			playing.RemoveIndex( i );
		}
	}
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

bool idSoundEmitterWwise::CurrentlyPlaying( void ) const {
	PrunePlaying();
	return playing.Num() > 0;
}

// Amplitude-driven effects (light flicker with sound): no Wwise equivalent yet (triage item).
float idSoundEmitterWwise::CurrentAmplitude( void ) {
	return 0.0f;
}

int idSoundEmitterWwise::Index( void ) const {
	return index;
}
