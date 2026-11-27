/*
===========================================================================

dhewm3 Wwise backend: idSoundEmitterWwise, the Wwise provider of idSoundEmitter.

One emitter is one Wwise game object. Its ID is unique across sound worlds:
worldNumber * 0x100000 + index. IDs 1 and 2 are reserved (debug object, listener).

StartSound posts the event Play_<shader> (D-052). Code keeps only the channel
contract of sound.h and what the game changes at run time (section 8.5 of
PROJECT_NOTES.md); the shader's own values are Wwise event setup. See snd_wwise.h.

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

// Names of the game parameters the game sets (built by tools/wwise_doom_sounds.js).
static const char *RTPC_VOLUME = "RTPC_Volume";
static const char *RTPC_FADE = "RTPC_Fade";
static const char *RTPC_SPATIALIZATION = "RTPC_Spatialization";
static const char *RTPC_SLOWMO = "RTPC_Slowmo";

// Marks the emitter as in use again (by AllocSoundEmitter or a savegame).
void idSoundEmitterWwise::Revive( void ) {
	alive = true;
	listenerId = 0;
	memset( &parms, 0, sizeof( parms ) );
	playing.Clear();
	WwiseSDK::SetScalingFactor( gameObjectID, 1.0f );
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
	} else {
		// The sounds play on; the world keeps their IDs so a world pause still reaches them.
		for ( int i = 0; i < playing.Num(); i++ ) {
			world->AdoptPlaying( playing[i].playingID );
		}
	}
	playing.Clear();
	alive = false;
}

void idSoundEmitterWwise::PauseAll( bool pause ) {
	for ( int i = 0; i < playing.Num(); i++ ) {
		if ( pause ) {
			WwiseSDK::PausePlayingID( playing[i].playingID );
		} else {
			WwiseSDK::ResumePlayingID( playing[i].playingID );
		}
	}
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
OverrideParms

Same rule as idSoundEmitterLocal::OverrideParms (snd_emitter.cpp): every non-zero value of
the emitter (speaker spawnargs, game code) replaces the shader's; the flags are combined.
================
*/
static void OverrideParms( const soundShaderParms_t &base, const soundShaderParms_t &over, soundShaderParms_t &out ) {
	out.minDistance = over.minDistance ? over.minDistance : base.minDistance;
	out.maxDistance = over.maxDistance ? over.maxDistance : base.maxDistance;
	out.shakes = over.shakes ? over.shakes : base.shakes;
	out.volume = over.volume ? over.volume : base.volume;
	out.soundClass = over.soundClass ? over.soundClass : base.soundClass;
	out.soundShaderFlags = base.soundShaderFlags | over.soundShaderFlags;
}

void idSoundEmitterWwise::ApplyVolume( const playingSound_t &sound, const soundShaderParms_t &over ) const {
	// The event carries the shader volume; RTPC_Volume is the emitter's change on top of it (dB).
	if ( over.volume != 0.0f ) {
		WwiseSDK::SetRTPCOnPlayingID( RTPC_VOLUME, over.volume - sound.shaderVolume, sound.playingID, 0 );
	}
}

/*
================
idSoundEmitterWwise::StartSound

Posts Play_<shader> (D-052). A missing event gives Wwise's own warning and is listed by
wwise_missing. The shader's own values (samples, random choice, looping, volume, distances,
2D or 3D) are in the Wwise event. Code sends what the game changes at run time (section 8.5):
- global sounds, and the sounds of the listener's own emitter, play on the at-listener
  game object, as OpenAL plays them without position (D-054)
- private and anti-private sounds play only for (or not for) the listener's own emitter
- the emitter volume (RTPC_Volume), distances (scaling factor), omnidirectional at run
  time (RTPC_Spatialization), and no slow motion for !allowSlow (RTPC_Slowmo = 1)

Returns the estimated length of the event in msec from the bank metadata (D-053),
0 for looping or unknown events.
================
*/
int idSoundEmitterWwise::StartSound( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags, bool allowSlow ) {
	if ( shader == NULL ) {
		return 0;
	}

	idStr eventName;
	EventNameForShader( shader->GetName(), eventName );
	const int length = soundSystemWwise.EventLengthMs( eventName.c_str() );

	const soundShaderParms_t &shaderParms = *shader->GetParms();
	soundShaderParms_t merged;
	OverrideParms( shaderParms, parms, merged );
	merged.soundShaderFlags |= shaderFlags;

	// The listener's own emitter (the player): OpenAL plays these without spatialization.
	const bool listenersOwn = ( listenerId == world->ListenerId() );
	if ( ( merged.soundShaderFlags & SSF_PRIVATE_SOUND ) && !listenersOwn ) {
		return length;
	}
	if ( ( merged.soundShaderFlags & SSF_ANTI_PRIVATE_SOUND ) && listenersOwn ) {
		return length;
	}

	// Channel contract of sound.h: a new sound on a channel replaces the one playing there.
	if ( channel != SCHANNEL_ANY ) {
		StopSound( channel );
	}

	const bool atListener = listenersOwn || ( merged.soundShaderFlags & SSF_GLOBAL ) != 0;
	const unsigned long long objectID = atListener ? WwiseSDK::AtListenerObjectID() : gameObjectID;
	unsigned int playingID = WwiseSDK::PostEventOnObject( eventName.c_str(), objectID );

	if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() ) {
		common->Printf( "Wwise StartSound: emitter %d channel %d %s -> %s%s, %d ms\n",
			index, (int)channel, eventName.c_str(), playingID != 0 ? "posted" : "no event",
			atListener ? " at the listener" : "", length );
	}
	if ( playingID == 0 ) {
		soundSystemWwise.ReportMissing( shader->GetName() );
		return length;
	}

	PrunePlaying();
	playingSound_t sound;
	sound.channel = channel;
	sound.playingID = playingID;
	sound.postTime = Sys_Milliseconds();
	sound.objectID = objectID;
	sound.shaderVolume = shaderParms.volume;
	playing.Append( sound );
	if ( world->IsSilenced() ) {
		// Started in a paused world, or one that is not playing: OpenAL would not mix it yet.
		WwiseSDK::PausePlayingID( playingID );
	}

	ApplyVolume( sound, parms );
	if ( !atListener && parms.maxDistance != 0.0f && shaderParms.maxDistance > 0.0f ) {
		// Wwise scales the attenuation of the whole game object; speakers play one sound each.
		WwiseSDK::SetScalingFactor( gameObjectID, parms.maxDistance / shaderParms.maxDistance );
	}
	if ( !atListener && ( merged.soundShaderFlags & SSF_OMNIDIRECTIONAL ) && !( shaderParms.soundShaderFlags & SSF_OMNIDIRECTIONAL ) ) {
		WwiseSDK::SetRTPCOnPlayingID( RTPC_SPATIALIZATION, 0.0f, playingID, 0 );
	}
	if ( !allowSlow ) {
		WwiseSDK::SetRTPCOnPlayingID( RTPC_SLOWMO, 1.0f, playingID, 0 );
	}
	if ( ( merged.soundShaderFlags & SSF_LOOPING ) && !( shaderParms.soundShaderFlags & SSF_LOOPING ) ) {
		// Wwise cannot make an event loop per post (section 8.5): it plays as built.
		soundSystemWwise.WarnOnce( va( "loop:%s", shader->GetName() ),
			"Wwise: %s is looped by the game but its event does not loop", shader->GetName() );
	}
	return length;
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
	// Sounds can be on this emitter's object or on the at-listener object (global sounds).
	unsigned int active[ 128 ];
	int numActive = WwiseSDK::PlayingIDsOnObject( gameObjectID, active, 64 );
	bool atListener = false;
	for ( int i = 0; i < playing.Num() && !atListener; i++ ) {
		atListener = ( playing[i].objectID != gameObjectID );
	}
	if ( atListener ) {
		numActive += WwiseSDK::PlayingIDsOnObject( WwiseSDK::AtListenerObjectID(), active + numActive, 64 );
	}
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

// Changes the volume of what plays on a channel (the heartbeat). Distances are per game object
// in Wwise (scaling factor), so a per-channel distance change is not applied.
void idSoundEmitterWwise::ModifySound( const s_channelType channel, const soundShaderParms_t *parms ) {
	if ( parms == NULL ) {
		return;
	}
	for ( int i = 0; i < playing.Num(); i++ ) {
		if ( channel == SCHANNEL_ANY || playing[i].channel == channel ) {
			ApplyVolume( playing[i], *parms );
		}
	}
}

void idSoundEmitterWwise::StopSound( const s_channelType channel ) {
	if ( channel == SCHANNEL_ANY ) {
		WwiseSDK::StopObject( gameObjectID );
		for ( int i = 0; i < playing.Num(); i++ ) {
			if ( playing[i].objectID != gameObjectID ) {
				WwiseSDK::StopPlayingID( playing[i].playingID );	// global sounds of this emitter
			}
		}
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

// Fades what plays on a channel to "to" dB over "over" seconds: RTPC_Fade with Wwise's interpolation.
void idSoundEmitterWwise::FadeSound( const s_channelType channel, float to, float over ) {
	const float value = idMath::ClampFloat( -96.0f, 0.0f, to );
	const int ms = over > 0.0f ? (int)( over * 1000.0f ) : 0;
	for ( int i = 0; i < playing.Num(); i++ ) {
		if ( channel == SCHANNEL_ANY || playing[i].channel == channel ) {
			WwiseSDK::SetRTPCOnPlayingID( RTPC_FADE, value, playing[i].playingID, ms );
		}
	}
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
