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
	// s_drawSounds overlay (D-067): a box, the index and a line per playing sound, if anything plays
	// and it is within its max distance (or all is set).
	void					DrawDebug( idRenderWorld *rw, const idVec3 &listenerPos, const idMat3 &viewAxis, bool all ) const;

	// One playing sound as a savegame channel record (the OpenAL layout, D-057).
	static const int		MAX_SAVED_SOUNDS = 8;	// SOUND_MAX_CHANNELS of the OpenAL layout
	struct savedSound_t {
		const idSoundShader *shader;
		int					channel;			// s_channelType it was started on
		soundShaderParms_t	parms;				// shader values merged with the emitter's (OverrideParms)
		float				diversity;
		int					elapsedMs;			// play position
		bool				hasFade;
		int					fadeStartMs;		// relative to the save (negative: before it)
		int					fadeEndMs;
		float				fadeFrom;			// dB
		float				fadeTo;
	};
	// Fills out with what still plays (at most MAX_SAVED_SOUNDS). Returns how many.
	int						SaveSounds( savedSound_t *out );
	// Starts a saved sound again at its saved position (loops too),
	// a fade in progress carries on (section 8.6). Returns true if it was started.
	bool					RestartSound( const savedSound_t &saved );

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
		// what a savegame needs to start it again (D-057)
		const idSoundShader *shader;
		soundShaderParms_t	parms;				// merged, changed by ModifySound as in OpenAL
		float				diversity;
		bool				hasFade;			// RTPC_Fade: fadeFrom to fadeTo between the two times
		int					fadeStartMs;		// Sys_Milliseconds()
		int					fadeEndMs;
		float				fadeFrom;
		float				fadeTo;
		int					pendingFadeMs;		// restored fade still to run when the world plays again
	};

	// Posts the event of a shader with the given emitter values. Returns the event length;
	// playingIndex gets the new entry in playing, or -1 if nothing was posted.
	int						PostShader( const idSoundShader *shader, const s_channelType channel, float diversity, int shaderFlags,
										bool allowSlow, const soundShaderParms_t &over, int *playingIndex );
	// Drops the playing IDs that Wwise no longer plays.
	void					PrunePlaying( void ) const;
	// RTPC_Fade of one playing sound at a time (dB).
	static float			FadeLevel( const playingSound_t &sound, int nowMs );
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
							idSoundWorldWwise( int worldNumber, idRenderWorld *renderWorld );
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
	const idMat3 &			ListenerAxis( void ) const { return listenerAxis; }
	int						ListenerId( void ) const { return listenerId; }
	void					ApplyListener( void ) const;
	// Keeps the playing IDs of a freed emitter whose sounds play on, so Pause still reaches them.
	void					AdoptPlaying( unsigned int playingID );
	// Sends the slow motion pitch (RTPC_Slowmo) for the current slowmo state.
	void					ApplySlowmo( void ) const;
	// OpenAL only mixes the playing world: a world that is not the playing one has its sounds
	// paused until it plays again (idSoundSystemWwise::SetPlayingSoundWorld, S-095).
	void					SetActive( bool active );
	// True while this world's sounds are held: paused by the game, or not the playing world.
	bool					IsSilenced( void ) const { return silenced; }

	// Converts a dhewm3 position (inches, X forward, Y left, Z up) to Wwise (meters, X right, Y up, Z forward).
	static void				ToWwisePosition( const idVec3 &doomPos, float out[3] );
	// Same axes for a direction vector, without scaling.
	static void				ToWwiseDirection( const idVec3 &doomDir, float out[3] );

private:
	idSoundEmitterWwise *	NewEmitter( void );
	// Pauses or resumes every playing sound of this world when paused || inactive changes.
	void					UpdateSilence( void );
	// s_drawSounds overlay (D-067), once per PlaceListener.
	void					DrawEmitters( void ) const;

	int						worldNumber;		// makes game object IDs unique across worlds
	idRenderWorld *			renderWorld;		// the world it was made with (debug drawing only); NULL for the menu
	idList<idSoundEmitterWwise *> emitters;		// index 0 is always NULL, as in idSoundWorldLocal
	idSoundEmitterWwise *	localSound;			// PlayShaderDirectly (menu and GUI sounds)
	idList<unsigned int>	orphanIDs;			// sounds of freed emitters that may still play
	idVec3					listenerOrigin;
	idMat3					listenerAxis;
	int						listenerId;
	int						gameMsec;
	bool					paused;				// Pause() / UnPause() by the game (IsPaused reports this)
	bool					inactive;			// not the playing world
	bool					silenced;			// the state last sent to Wwise: paused || inactive
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
	// A shader with no definition in any .sndshd file and no <name>.wav or .ogg (a map data error, such as
	// guisounds, D-066): true, and a one-time console line. Checked once per shader. Game thread only.
	bool					IsUndefinedShader( const char *shaderName, bool implicit );
	const idStrList &		UndefinedShaders( void ) const { return undefinedShaders; }
	// Prints a warning once per key (for example a run-time flag Wwise cannot apply).
	void					WarnOnce( const char *key, const char *fmt, const char *arg );
	// Estimated length of an event in ms, from the bank metadata (D-053). 0: looping, unknown or not loaded.
	int						EventLengthMs( const char *eventName ) const;
	int						NumBanks( void ) const { return loadedBanks.Num(); }

	// Banks by level (section 8.8, D-064). Each event has its own auto-defined bank, Event/<event>.bnk.
	// idSoundShader::Parse calls ShaderParsed: during a level load it marks the shader's event as used by
	// the level, and EndLevelLoad loads the marked banks and unloads the others (not the resident ones).
	void					ShaderParsed( const char *shaderName );
	// Loads the event's bank if it is not loaded (at first use, as OpenAL loads a sample when it is played).
	// Returns false if no bank holds the event: no Event/<event>.bnk and not in a banks.txt bank.
	bool					EnsureEventLoaded( const char *eventName );
	// D-068: if the content has a looping variant <event>_Loop (for a shader the game loops at run time while
	// its event does not loop), loads it and puts its name in eventName. Returns true if it did.
	bool					UseLoopVariant( idStr &eventName );
	// True when the event's bank metadata says it plays forever (DurationType Infinite).
	bool					EventLoops( const char *eventName ) const;
	// wwise_banks: loaded, resident and missing event banks, first-use loads, memory.
	void					PrintBanks( bool list );

	static idCVar			s_wwiseDevice;
	static idCVar			s_wwiseCapture;
	static idCVar			s_wwiseShowStartSound;

private:
	// Loads the banks named in base/wwise/Windows/banks.txt and reads their event lengths.
	void					LoadContentBanks( void );
	// userBank: also records the bank's event names (banks.txt banks hold events without an Event/ bank).
	void					ReadEventLengths( const char *bankName, bool userBank );
	// Loads the events listed in resident.txt (never unloaded).
	void					LoadResidentBanks( void );

	enum eventBankState_t {
		EB_UNLOADED,		// known, bank not loaded
		EB_LOADED,			// loaded for the current level (or at first use)
		EB_RESIDENT,		// loaded at start, never unloaded (resident.txt)
		EB_MISSING			// no bank file, or the bank failed to load: not tried again
	};
	struct eventBank_t {
		idStr				name;				// event name as the game posts it
		eventBankState_t	state;
		bool				marked;				// used by the current level
	};
	int						FindEventBank( const char *eventName, bool add );
	bool					LoadEventBankAt( int index, bool firstUse );
	// Batch load (banks, then one PrepareEvent for their media) and unload. Return / update the states.
	int						LoadEventBanks( const idList<int> &indices );
	void					UnloadEventBanks( const idList<int> &indices );

	idList<eventBank_t>		eventBanks;
	idHashTable<int>		eventBankIndex;		// lower-case event name -> index in eventBanks
	idHashTable<int>		userBankEvents;		// lower-case names in the banks.txt banks' metadata
	bool					insideLevelLoad;
	int						firstUseLoads;		// since the last EndLevelLoad
	double					firstUseMs;
	double					firstUseMaxMs;

	volatile bool			started;			// read by the async sound tick
	bool					muted;
	int						worldCount;
	idSoundWorldWwise *		playingWorld;
	idStrList				missingShaders;
	idStrList				undefinedShaders;	// map data errors (D-066)
	idStrList				implicitWithFile;	// implicit shaders whose sample file exists
	idStrList				warned;
	idStrList				loadedBanks;
	idHashTable<int>		eventLengths;		// lower-case event name -> ms; -1: plays forever (looping)
	idHashTable<int>		loopVariants;		// lower-case <event>_loop names that have an Event/ bank (D-068)
};

extern idSoundSystemWwise	soundSystemWwise;

#endif /* !__SND_WWISE_H__ */
