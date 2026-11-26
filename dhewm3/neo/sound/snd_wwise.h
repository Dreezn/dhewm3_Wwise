/*
===========================================================================

dhewm3 Wwise backend: the Wwise provider of the sound system service.

Implements the sound.h interfaces (idSoundSystem, idSoundWorld, idSoundEmitter)
on top of the Wwise sound engine. idSoundSystemLocator (snd_locator.cpp) picks
this provider when s_backend is "wwise". Design: PROJECT_NOTES.md section 8.3.

Only compiled when WWISE_ROOT is set at configure time (defines WWISE_ENABLED).
The Wwise SDK itself is only used in snd_wwise_sdk.cpp (no SDK types here).

Files:
	snd_wwise.cpp			idSoundSystemWwise (engine life cycle, sound tick, debug commands)
	snd_wwise_world.cpp		idSoundWorldWwise (listener, emitter list, savegames)
	snd_wwise_emitter.cpp	idSoundEmitterWwise (one Wwise game object per emitter)

===========================================================================
*/

#ifndef __SND_WWISE_H__
#define __SND_WWISE_H__

#include "idlib/containers/List.h"
#include "idlib/containers/StrList.h"
#include "idlib/containers/HashTable.h"
#include "idlib/math/Matrix.h"
#include "framework/CVarSystem.h"
#include "sound/sound.h"

class idSoundWorldWwise;

/*
===================================================================================

idSoundEmitterWwise

One Wwise game object. Emitters are reused by their world, never deleted
before the world is.

===================================================================================
*/

class idSoundEmitterWwise : public idSoundEmitter {
public:
							idSoundEmitterWwise( idSoundWorldWwise *world, int index );
	virtual					~idSoundEmitterWwise( void );

	// idSoundEmitter
	virtual void			Free( bool immediate );
	virtual void			UpdateEmitter( const idVec3 &origin, int listenerId, const soundShaderParms_t *parms );
	virtual int				StartSound( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags, bool allowSlow );
	virtual void			ModifySound( const s_channelType channel, const soundShaderParms_t *parms );
	virtual void			StopSound( const s_channelType channel );
	virtual void			FadeSound( const s_channelType channel, float to, float over );
	virtual bool			CurrentlyPlaying( void ) const;
	virtual float			CurrentAmplitude( void );
	virtual int				Index( void ) const;

	// used by idSoundWorldWwise
	bool					IsAlive( void ) const { return alive; }
	void					Revive( void );
	unsigned long long		GameObjectID( void ) const { return gameObjectID; }
	const idVec3 &			Origin( void ) const { return origin; }
	int						ListenerId( void ) const { return listenerId; }
	const soundShaderParms_t & Parms( void ) const { return parms; }
	void					RestoreState( const idVec3 &origin, int listenerId, const soundShaderParms_t &parms );
	// Pauses or resumes everything this emitter started (world Pause / UnPause).
	void					PauseAll( bool pause );

	// Event name for a sound shader: Play_ + shader name, other than [A-Za-z0-9_] becomes _ (D-052).
	static void				EventNameForShader( const char *shaderName, idStr &eventName );

private:
	// one started event: the dhewm3 channel it was started on, its Wwise playing ID, when it was
	// posted, the game object it was posted on, and the shader's own volume (for RTPC_Volume)
	struct playingSound_t {
		int					channel;
		unsigned int		playingID;
		int					postTime;			// Sys_Milliseconds()
		unsigned long long	objectID;			// this emitter, or the at-listener object (global sounds)
		float				shaderVolume;		// dB
	};

	// Drops the playing IDs that Wwise no longer plays.
	void					PrunePlaying( void ) const;
	// Sends the emitter volume as RTPC_Volume (offset from the shader volume) to one playing sound.
	void					ApplyVolume( const playingSound_t &sound, const soundShaderParms_t &over ) const;

	idSoundWorldWwise *		world;
	int						index;				// position in the world's emitter list (> 0)
	unsigned long long		gameObjectID;
	bool					alive;				// false: free for reuse
	idVec3					origin;
	int						listenerId;
	soundShaderParms_t		parms;
	mutable idList<playingSound_t> playing;	// the channel contract of sound.h (StopSound, CurrentlyPlaying)
};

/*
===================================================================================

idSoundWorldWwise

The listener and the emitters of one dhewm3 sound world (game, menu, editor).
Only the playing world (idSoundSystem::SetPlayingSoundWorld) moves the Wwise listener.

===================================================================================
*/

class idSoundWorldWwise : public idSoundWorld {
public:
							idSoundWorldWwise( int worldNumber );
	virtual					~idSoundWorldWwise( void );

	// idSoundWorld
	virtual void			ClearAllSoundEmitters( void );
	virtual void			StopAllSounds( void );
	virtual idSoundEmitter *AllocSoundEmitter( void );
	virtual idSoundEmitter *EmitterForIndex( int index );
	virtual	float			CurrentShakeAmplitudeForPosition( const int time, const idVec3 &listenerPosition );
	virtual	void			PlaceListener( const idVec3 &origin, const idMat3 &axis, const int listenerId, const int gameTime, const idStr& areaName );
	virtual void			FadeSoundClasses( const int soundClass, const float to, const float over );
	virtual	void			PlayShaderDirectly( const char *name, int channel );
	virtual void			StartWritingDemo( idDemoFile *demo );
	virtual void			StopWritingDemo( void );
	virtual void			ProcessDemoCommand( idDemoFile *demo );
	virtual void			Pause( void );
	virtual void			UnPause( void );
	virtual bool			IsPaused( void );
	virtual void			AVIOpen( const char *path, const char *name );
	virtual void			AVIClose( void );
	virtual void			WriteToSaveGame( idFile *savefile );
	virtual void			ReadFromSaveGame( idFile *savefile );
	virtual void			SetSlowmo( bool active );
	virtual void			SetSlowmoSpeed( float speed );
	virtual void			SetEnviroSuit( bool active );

	// used by idSoundSystemWwise and idSoundEmitterWwise
	int						WorldNumber( void ) const { return worldNumber; }
	const idVec3 &			ListenerOrigin( void ) const { return listenerOrigin; }
	int						ListenerId( void ) const { return listenerId; }
	void					ApplyListener( void ) const;
	// Keeps the playing IDs of a freed emitter whose sounds play on, so Pause still reaches them.
	void					AdoptPlaying( unsigned int playingID );
	// Sends the slow motion pitch (RTPC_Slowmo) for the current slowmo state.
	void					ApplySlowmo( void ) const;

	// Converts a dhewm3 position (inches, X forward, Y left, Z up) to Wwise (meters, X right, Y up, Z forward).
	static void				ToWwisePosition( const idVec3 &doomPos, float out[3] );
	// Same axes for a direction vector, without scaling.
	static void				ToWwiseDirection( const idVec3 &doomDir, float out[3] );

private:
	idSoundEmitterWwise *	NewEmitter( void );

	int						worldNumber;		// makes game object IDs unique across worlds
	idList<idSoundEmitterWwise *> emitters;		// index 0 is always NULL, as in idSoundWorldLocal
	idSoundEmitterWwise *	localSound;			// PlayShaderDirectly (menu and GUI sounds)
	idList<unsigned int>	orphanIDs;			// sounds of freed emitters that may still play
	idVec3					listenerOrigin;
	idMat3					listenerAxis;
	int						listenerId;
	int						gameMsec;
	bool					paused;
	bool					slowmoActive;
	float					slowmoSpeed;
	bool					enviroSuitActive;
};

/*
===================================================================================

idSoundSystemWwise

Owns the Wwise sound engine. Start() is called by idSoundSystemLocator.

===================================================================================
*/

class idSoundSystemWwise : public idSoundSystem {
public:
							idSoundSystemWwise( void );

	// Starts the engine and loads the banks. Returns false if Wwise could not start
	// (the locator then falls back to OpenAL).
	bool					Start( void );
	bool					IsStarted( void ) const { return started; }

	// idSoundSystem
	virtual void			Init( void );
	virtual	void			Shutdown( void );
	virtual bool			InitHW( void );
	virtual bool			ShutdownHW( void );
	virtual int				AsyncUpdate( int time );
	virtual int				AsyncUpdateWrite( int time );
	virtual void			SetMute( bool mute );
	virtual cinData_t		ImageForTime( const int milliseconds, const bool waveform );
	virtual int				GetSoundDecoderInfo( int index, soundDecoderInfo_t &decoderInfo );
	virtual idSoundWorld *	AllocSoundWorld( idRenderWorld *rw );
	virtual void			SetPlayingSoundWorld( idSoundWorld *soundWorld );
	virtual idSoundWorld *	GetPlayingSoundWorld( void );
	virtual	void			BeginLevelLoad( void );
	virtual	void			EndLevelLoad( const char *mapString );
	virtual int				AsyncMix( int soundTime, float *mixBuffer );
	virtual void			PrintMemInfo( MemInfo_t *mi );
	virtual int				IsEFXAvailable( void );

	// Shaders whose event could not be posted (listed by wwise_missing). Game thread only.
	void					ReportMissing( const char *shaderName );
	const idStrList &		MissingShaders( void ) const { return missingShaders; }
	// Prints a warning once per key (for example a run-time flag Wwise cannot apply).
	void					WarnOnce( const char *key, const char *fmt, const char *arg );
	// Estimated length of an event in ms, from the bank metadata (D-053). 0: looping, unknown or not loaded.
	int						EventLengthMs( const char *eventName ) const;
	int						NumBanks( void ) const { return loadedBanks.Num(); }

	static idCVar			s_wwiseDevice;
	static idCVar			s_wwiseCapture;
	static idCVar			s_wwiseShowStartSound;

private:
	// Loads the banks named in base/wwise/Windows/banks.txt and reads their event lengths.
	void					LoadContentBanks( void );
	void					ReadEventLengths( const char *bankName );

	volatile bool			started;			// read by the async sound tick
	bool					muted;
	int						worldCount;
	idSoundWorldWwise *		playingWorld;
	idStrList				missingShaders;
	idStrList				warned;
	idStrList				loadedBanks;
	idHashTable<int>		eventLengths;		// lower-case event name -> ms
};

extern idSoundSystemWwise	soundSystemWwise;

#endif /* !__SND_WWISE_H__ */
