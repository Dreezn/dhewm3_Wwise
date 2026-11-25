/*
===========================================================================

dhewm3 sound system service locator (D-049, PROJECT_NOTES.md section 8.3).

Service:   idSoundSystem (with idSoundWorld and idSoundEmitter), sound.h
Providers: idSoundSystemLocal (OpenAL, snd_system.cpp)
           idSoundSystemWwise (Wwise, snd_wwise.cpp; only in builds with WWISE_ROOT)

Init() runs once at start-up, in place of soundSystem->Init(). It reads s_backend,
starts that provider (Wwise falls back to OpenAL if it cannot start) and points
the global soundSystem at it. The game DLL copies soundSystem later, when it loads.

===========================================================================
*/

#ifndef __SND_LOCATOR_H__
#define __SND_LOCATOR_H__

class idSoundSystemLocator {
public:
	// Picks and starts the sound system provider, then sets soundSystem.
	static void			Init( void );
	// "openal" or "wwise": the provider chosen by Init().
	static const char *	ProviderName( void );
};

#endif /* !__SND_LOCATOR_H__ */
