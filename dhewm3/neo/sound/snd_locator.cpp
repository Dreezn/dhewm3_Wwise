/*
===========================================================================

dhewm3 sound system service locator. See snd_locator.h.

===========================================================================
*/

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"

#include "sound/snd_local.h"		// soundSystemLocal: the OpenAL provider
#ifdef WWISE_ENABLED
#include "sound/snd_wwise.h"		// soundSystemWwise: the Wwise provider
#endif
#include "sound/snd_locator.h"

static idCVar s_backend( "s_backend", "openal", CVAR_SOUND | CVAR_ARCHIVE | CVAR_NOCHEAT,
	"sound system provider, read at start-up: openal (default) or wwise (needs a build with WWISE_ROOT)" );

static const char * providerName = "openal";

void idSoundSystemLocator::Init( void ) {
	soundSystem = &soundSystemLocal;
	providerName = "openal";

	if ( idStr::Icmp( s_backend.GetString(), "wwise" ) == 0 ) {
#ifdef WWISE_ENABLED
		if ( soundSystemWwise.Start() ) {
			soundSystem = &soundSystemWwise;
			providerName = "wwise";
			common->Printf( "Sound system provider: wwise\n" );
			return;
		}
		common->Warning( "s_backend wwise: Wwise did not start, falling back to OpenAL" );
#else
		common->Warning( "s_backend wwise: this build has no Wwise (configure with WWISE_ROOT), using OpenAL" );
#endif
	} else if ( idStr::Icmp( s_backend.GetString(), "openal" ) != 0 ) {
		common->Warning( "s_backend %s: unknown provider, using OpenAL", s_backend.GetString() );
	}

	common->Printf( "Sound system provider: openal\n" );
	soundSystem->Init();
}

const char * idSoundSystemLocator::ProviderName( void ) {
	return providerName;
}
