/*
===========================================================================

dhewm3 Wwise backend: engine life cycle, as seen by dhewm3 (see snd_wwise.h).
The SDK calls are in snd_wwise_sdk.cpp.

===========================================================================
*/

#include "sys/platform.h"
#include "sound/snd_wwise.h"
#include "sound/snd_local.h"
#include "sound/snd_wwise_sdk.h"

idWwiseEngine wwiseEngine;

idWwiseEngine::idWwiseEngine() {
	initialized = false;
}

idWwiseEngine::~idWwiseEngine() {
	Shutdown();
}

bool idWwiseEngine::Init() {
#ifdef WWISE_ENABLED
	if ( initialized ) {
		return true;
	}

	int stage = 0;
	int result = 0;
	if ( !WwiseSDK::Start( &stage, &result ) ) {
		common->Warning( "Wwise: engine failed to start at stage %d (AKRESULT %d)", stage, result );
		return false;
	}

	int major = 0, minor = 0, subminor = 0, build = 0;
	WwiseSDK::Version( &major, &minor, &subminor, &build );
	common->Printf( "Wwise %d.%d.%d (build %d) engine started\n", major, minor, subminor, build );

	initialized = true;
	return true;
#else
	return false;
#endif
}

void idWwiseEngine::Shutdown() {
#ifdef WWISE_ENABLED
	if ( !initialized ) {
		return;
	}
	WwiseSDK::Stop();
	common->Printf( "Wwise engine stopped\n" );
	initialized = false;
#endif
}
