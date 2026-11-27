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
	const int now = Sys_Milliseconds();
	for ( int i = 0; i < playing.Num(); i++ ) {
		playingSound_t &sound = playing[i];
		if ( pause ) {
			WwiseSDK::PausePlayingID( sound.playingID );
			continue;
		}
		if ( sound.pendingFadeMs > 0 ) {
			// a fade restored from a savegame runs on from here (RestartSound)
			WwiseSDK::SetRTPCOnPlayingID( RTPC_FADE, sound.fadeTo, sound.playingID, sound.pendingFadeMs );
			sound.fadeStartMs = now;
			sound.fadeEndMs = now + sound.pendingFadeMs;
			sound.pendingFadeMs = 0;
		}
		WwiseSDK::ResumePlayingID( sound.playingID );
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
	int playingIndex;
	return PostShader( shader, channel, diversity, shaderFlags, allowSlow, parms, &playingIndex );
}

int idSoundEmitterWwise::PostShader( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags,
									bool allowSlow, const soundShaderParms_t &over, int *playingIndex ) {
	*playingIndex = -1;
	if ( shader == NULL ) {
		return 0;
	}

	idStr eventName;
	EventNameForShader( shader->GetName(), eventName );
	const int length = soundSystemWwise.EventLengthMs( eventName.c_str() );

	const soundShaderParms_t &shaderParms = *shader->GetParms();
	soundShaderParms_t merged;
	OverrideParms( shaderParms, over, merged );
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
	sound.shader = shader;
	sound.parms = merged;
	sound.diversity = diversity;
	sound.hasFade = false;
	sound.fadeStartMs = sound.fadeEndMs = sound.postTime;
	sound.fadeFrom = sound.fadeTo = 0.0f;
	sound.pendingFadeMs = 0;
	*playingIndex = playing.Append( sound );
	if ( world->IsSilenced() ) {
		// Started in a paused world, or one that is not playing: OpenAL would not mix it yet.
		WwiseSDK::PausePlayingID( playingID );
	}

	ApplyVolume( sound, over );
	if ( !atListener && over.maxDistance != 0.0f && shaderParms.maxDistance > 0.0f ) {
		// Wwise scales the attenuation of the whole game object; speakers play one sound each.
		WwiseSDK::SetScalingFactor( gameObjectID, over.maxDistance / shaderParms.maxDistance );
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
			OverrideParms( playing[i].parms, *parms, playing[i].parms );	// as OpenAL keeps it for the savegame
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
	const int now = Sys_Milliseconds();
	for ( int i = 0; i < playing.Num(); i++ ) {
		playingSound_t &sound = playing[i];
		if ( channel == SCHANNEL_ANY || sound.channel == channel ) {
			WwiseSDK::SetRTPCOnPlayingID( RTPC_FADE, value, sound.playingID, ms );
			// kept for the savegame (D-057); Wwise interpolates the RTPC linearly, as OpenAL fades in dB
			sound.fadeFrom = FadeLevel( sound, now );
			sound.fadeTo = value;
			sound.fadeStartMs = now;
			sound.fadeEndMs = now + ms;
			sound.hasFade = true;
			sound.pendingFadeMs = 0;
		}
	}
}

float idSoundEmitterWwise::FadeLevel( const playingSound_t &sound, int nowMs ) {
	if ( !sound.hasFade ) {
		return 0.0f;
	}
	if ( sound.pendingFadeMs > 0 ) {
		return sound.fadeFrom;		// restored fade waiting for the world to play
	}
	if ( nowMs >= sound.fadeEndMs || sound.fadeEndMs <= sound.fadeStartMs ) {
		return sound.fadeTo;
	}
	if ( nowMs <= sound.fadeStartMs ) {
		return sound.fadeFrom;
	}
	const float f = (float)( nowMs - sound.fadeStartMs ) / (float)( sound.fadeEndMs - sound.fadeStartMs );
	return sound.fadeFrom + ( sound.fadeTo - sound.fadeFrom ) * f;
}

/*
================
idSoundEmitterWwise::SaveSounds

What a savegame keeps of each sound still playing (D-057). The play position comes from
Wwise (posted with the play position enabled); the post time is the fall-back.
================
*/
int idSoundEmitterWwise::SaveSounds( savedSound_t *out ) {
	const int before = playing.Num();
	PrunePlaying();
	if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() && before > 0 ) {
		common->Printf( "Wwise savegame: emitter %d has %d of %d sounds still playing\n", index, playing.Num(), before );
	}
	const int now = Sys_Milliseconds();
	int count = 0;
	for ( int i = 0; i < playing.Num() && count < MAX_SAVED_SOUNDS; i++ ) {
		const playingSound_t &sound = playing[i];
		if ( sound.shader == NULL ) {
			continue;
		}
		savedSound_t &s = out[count++];
		s.shader = sound.shader;
		s.channel = sound.channel;
		s.parms = sound.parms;
		s.diversity = sound.diversity;
		unsigned int positionMs = 0;
		if ( WwiseSDK::PlayPosition( sound.playingID, &positionMs ) == WwiseSDK::RESULT_SUCCESS ) {
			s.elapsedMs = (int)positionMs;
		} else {
			s.elapsedMs = now - sound.postTime;
		}
		s.hasFade = sound.hasFade;
		s.fadeFrom = sound.fadeFrom;
		s.fadeTo = sound.fadeTo;
		if ( sound.pendingFadeMs > 0 ) {
			s.fadeStartMs = 0;
			s.fadeEndMs = sound.pendingFadeMs;
		} else {
			s.fadeStartMs = sound.fadeStartMs - now;
			s.fadeEndMs = sound.fadeEndMs - now;
		}
	}
	return count;
}

/*
================
idSoundEmitterWwise::RestartSound

Starts a saved sound again through the normal post (same channel and values, so the
private, global, omni, volume and distance rules apply), section 8.6:
- a looping shader plays from the start (where a loop had reached is not heard)
- a one-shot seeks to its saved position, and is not started if it would have ended
- a fade in progress continues over the time it had left: when the world plays again,
  since a restored world is held while the level loads (OpenAL shifts its fades the same way)
================
*/
bool idSoundEmitterWwise::RestartSound( const savedSound_t &saved ) {
	if ( saved.shader == NULL ) {
		return false;
	}
	idStr eventName;
	EventNameForShader( saved.shader->GetName(), eventName );
	const bool loops = ( saved.shader->GetParms()->soundShaderFlags & SSF_LOOPING ) != 0;
	const int length = soundSystemWwise.EventLengthMs( eventName.c_str() );
	if ( !loops && length > 0 && saved.elapsedMs >= length ) {
		if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() ) {
			common->Printf( "Wwise restore: emitter %d channel %d %s had ended (%d of %d ms), not started\n",
				index, saved.channel, eventName.c_str(), saved.elapsedMs, length );
		}
		return false;
	}

	// allowSlow is not in the savegame (OpenAL does not keep it either)
	int playingIndex = -1;
	PostShader( saved.shader, (s_channelType)saved.channel, saved.diversity, saved.parms.soundShaderFlags, true, saved.parms, &playingIndex );
	if ( playingIndex < 0 ) {
		return false;
	}
	playingSound_t &sound = playing[playingIndex];
	if ( !loops && saved.elapsedMs > 0 ) {
		WwiseSDK::SeekPlayingID( eventName.c_str(), sound.objectID, saved.elapsedMs, sound.playingID );
	}
	if ( idSoundSystemWwise::s_wwiseShowStartSound.GetBool() ) {
		common->Printf( "Wwise restore: emitter %d channel %d %s at %d ms%s\n", index, saved.channel, eventName.c_str(),
			loops ? 0 : saved.elapsedMs, saved.hasFade ? ", with fade" : "" );
	}
	if ( !saved.hasFade ) {
		return true;
	}

	// fade level at the moment of the save
	float level = saved.fadeTo;
	if ( saved.fadeEndMs > 0 && saved.fadeEndMs > saved.fadeStartMs ) {
		if ( saved.fadeStartMs >= 0 ) {
			level = saved.fadeFrom;
		} else {
			const float f = (float)( -saved.fadeStartMs ) / (float)( saved.fadeEndMs - saved.fadeStartMs );
			level = saved.fadeFrom + ( saved.fadeTo - saved.fadeFrom ) * f;
		}
	}
	const int now = Sys_Milliseconds();
	WwiseSDK::SetRTPCOnPlayingID( RTPC_FADE, level, sound.playingID, 0 );
	sound.hasFade = true;
	sound.fadeFrom = level;
	sound.fadeTo = saved.fadeTo;
	sound.fadeStartMs = sound.fadeEndMs = now;
	sound.pendingFadeMs = 0;
	const int remaining = saved.fadeEndMs > 0 ? saved.fadeEndMs : 0;
	if ( remaining > 0 ) {
		if ( world->IsSilenced() ) {
			sound.pendingFadeMs = remaining;
		} else {
			WwiseSDK::SetRTPCOnPlayingID( RTPC_FADE, saved.fadeTo, sound.playingID, remaining );
			sound.fadeEndMs = now + remaining;
		}
	} else {
		sound.fadeFrom = saved.fadeTo;
	}
	return true;
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
