/*
===========================================================================

dhewm3 Wwise backend: the only file that includes Wwise SDK headers.

It includes no dhewm3 headers on purpose: dhewm3's platform.h redefines
names such as vsnprintf, which the SDK headers use.

The SDK's default file I/O is taken from the user's installed SDK
(WWISE_ROOT/SDK/samples), compiled by CMake. Nothing is copied into the repo.

===========================================================================
*/

#include <AK/SoundEngine/Common/AkMemoryMgrModule.h>
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/AkStreamMgrModule.h>

#include "AkDefaultIOHookDeferred.h"

#include "sound/snd_wwise_sdk.h"

static CAkDefaultIOHookDeferred	wwiseLowLevelIO;
static bool						wwiseStarted = false;
static const AkGameObjectID	kDebugGameObject = 1;

namespace WwiseSDK {

bool Start( int * failedStage, int * akResult ) {
	if ( wwiseStarted ) {
		return true;
	}
	*failedStage = 0;
	*akResult = 0;

	AkMemSettings memSettings;
	AK::MemoryMgr::GetDefaultSettings( memSettings );
	AKRESULT res = AK::MemoryMgr::Init( &memSettings );
	if ( res != AK_Success ) {
		*failedStage = 1;
		*akResult = (int)res;
		return false;
	}

	AkStreamMgrSettings stmSettings;
	AK::StreamMgr::GetDefaultSettings( stmSettings );
	if ( !AK::StreamMgr::Create( stmSettings ) ) {
		*failedStage = 2;
		AK::MemoryMgr::Term();
		return false;
	}

	AkDeviceSettings deviceSettings;
	AK::StreamMgr::GetDefaultDeviceSettings( deviceSettings );
	res = wwiseLowLevelIO.Init( deviceSettings );
	if ( res != AK_Success ) {
		*failedStage = 3;
		*akResult = (int)res;
		AK::IAkStreamMgr::Get()->Destroy();
		AK::MemoryMgr::Term();
		return false;
	}

	AkInitSettings initSettings;
	AkPlatformInitSettings platformSettings;
	AK::SoundEngine::GetDefaultInitSettings( initSettings );
	AK::SoundEngine::GetDefaultPlatformInitSettings( platformSettings );
	res = AK::SoundEngine::Init( &initSettings, &platformSettings );
	if ( res != AK_Success ) {
		*failedStage = 4;
		*akResult = (int)res;
		wwiseLowLevelIO.Term();
		AK::IAkStreamMgr::Get()->Destroy();
		AK::MemoryMgr::Term();
		return false;
	}

	// Banks are read from base/wwise/Windows/ (relative to the game folder).
	// The bank files come from the Wwise project (wwise/DhewmTest), not from the repo.
	res = wwiseLowLevelIO.SetBasePath( L"base/wwise/Windows/" );
	if ( res != AK_Success ) {
		*failedStage = 5;
		*akResult = (int)res;
		AK::SoundEngine::Term();
		wwiseLowLevelIO.Term();
		AK::IAkStreamMgr::Get()->Destroy();
		AK::MemoryMgr::Term();
		return false;
	}

	static const char * const bankNames[] = { "Init.bnk", "TestBank.bnk" };
	for ( const char * bankName : bankNames ) {
		AkBankID bankID = 0;
		res = AK::SoundEngine::LoadBank( bankName, bankID );
		if ( res != AK_Success ) {
			*failedStage = 5;
			*akResult = (int)res;
			AK::SoundEngine::Term();
			wwiseLowLevelIO.Term();
			AK::IAkStreamMgr::Get()->Destroy();
			AK::MemoryMgr::Term();
			return false;
		}
	}

	// One game object for the debug command (later: one per emitter).
	res = AK::SoundEngine::RegisterGameObj( kDebugGameObject );
	if ( res != AK_Success ) {
		*failedStage = 4;
		*akResult = (int)res;
		AK::SoundEngine::Term();
		wwiseLowLevelIO.Term();
		AK::IAkStreamMgr::Get()->Destroy();
		AK::MemoryMgr::Term();
		return false;
	}

	wwiseStarted = true;
	return true;
}

bool PostEvent( const char * eventName, unsigned int * playingID, int * akResult ) {
	*playingID = AK::SoundEngine::PostEvent( eventName, kDebugGameObject );
	*akResult = 0;
	if ( *playingID == AK_INVALID_PLAYING_ID ) {
		*akResult = 1;
		return false;
	}
	return true;
}

void Stop() {
	if ( !wwiseStarted ) {
		return;
	}
	AK::SoundEngine::Term();
	wwiseLowLevelIO.Term();
	if ( AK::IAkStreamMgr::Get() ) {
		AK::IAkStreamMgr::Get()->Destroy();
	}
	if ( AK::MemoryMgr::IsInitialized() ) {
		AK::MemoryMgr::Term();
	}
	wwiseStarted = false;
}

void Version( int * major, int * minor, int * subminor, int * build ) {
	*major = AK_WWISESDK_VERSION_MAJOR;
	*minor = AK_WWISESDK_VERSION_MINOR;
	*subminor = AK_WWISESDK_VERSION_SUBMINOR;
	*build = AK_WWISESDK_VERSION_BUILD;
}

}
