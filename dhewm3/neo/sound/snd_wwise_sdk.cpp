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
#include <AK/Comm/AkCommunication.h>

#include <objbase.h>
#include <AK/Tools/Common/AkMonitorError.h>
#include <stdio.h>

#include "AkDefaultIOHookDeferred.h"

#include "sound/snd_wwise_sdk.h"

// Wwise reports engine errors and messages through AK::Monitor. Without a handler
// they are not shown anywhere in a game build, so failures would be silent.
// Everything is written to wwise_monitor.txt in the game folder.
static WwiseSDK::LogFunc	gLogFunc = NULL;

static void WwiseMonitorOutput( AK::Monitor::ErrorCode in_eErrorCode, const AkOSChar * in_pszError,
		AK::Monitor::ErrorLevel in_eErrorLevel, AkPlayingID in_playingID, AkGameObjectID in_gameObjID ) {
	char text[ 512 ];
	size_t n = 0;
	for ( const AkOSChar * q = in_pszError; q != NULL && *q != 0 && n < sizeof( text ) - 1; q++ ) {
		text[ n++ ] = (char)( *q & 0x7f );
	}
	text[ n ] = 0;
	if ( gLogFunc != NULL ) {
		gLogFunc( text, in_eErrorLevel == AK::Monitor::ErrorLevel_Error );
	}
	FILE * f = fopen( "wwise_monitor.txt", "a" );
	if ( f == NULL ) {
		return;
	}
	fprintf( f, "%s code=%d playing=%u object=%llu: ",
		in_eErrorLevel == AK::Monitor::ErrorLevel_Error ? "ERROR" : "MESSAGE",
		(int)in_eErrorCode, (unsigned int)in_playingID, (unsigned long long)in_gameObjID );
	for ( const AkOSChar * p = in_pszError; p != NULL && *p != 0 && p - in_pszError < 400; p++ ) {
		fputc( (int)( *p & 0x7f ), f );
	}
	fputc( '\n', f );
	fclose( f );
}

static void WwiseMonitorBegin() {
	FILE * f = fopen( "wwise_monitor.txt", "w" );
	if ( f != NULL ) {
		fclose( f );
	}
	AK::Monitor::SetLocalOutput( AK::Monitor::ErrorLevel_All, WwiseMonitorOutput );
}

static CAkDefaultIOHookDeferred	wwiseLowLevelIO;
static bool						wwiseStarted = false;
static bool						commSystemStarted = false;
static const AkGameObjectID	kDebugGameObject = 1;
// The listener (the player's ears). Emitters are only heard through a listener.
static const AkGameObjectID	kListenerGameObject = 2;

namespace WwiseSDK {

bool Start( int * failedStage, int * akResult, unsigned int outputDeviceID ) {
	if ( wwiseStarted ) {
		return true;
	}
	*failedStage = 0;
	*akResult = 0;

	WwiseMonitorBegin();

	// The Wwise Windows audio sinks (WASAPI) need COM initialised on the thread that starts the engine.
	CoInitializeEx( NULL, COINIT_MULTITHREADED );

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
	// Name the main output explicitly: the 'System' audio device shareset from the project.
	initSettings.settingsMainOutput.audioDeviceShareset = AK::SoundEngine::GetIDFromString( "System" );
	if ( outputDeviceID != 0 ) {
		initSettings.settingsMainOutput.idDevice = outputDeviceID;
	}
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

	// Remote connection for Wwise Authoring (test builds). Failing here does not stop the engine.
	AkCommSettings commSettings;
	AK::Comm::GetDefaultInitSettings( commSettings );
	if ( AK::Comm::Init( commSettings ) != AK_Success ) {
		commSystemStarted = false;
	} else {
		commSystemStarted = true;
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

	// One listener game object, set as the default listener for every emitter.
	// One game object for the debug command (later: one per emitter).
	res = AK::SoundEngine::RegisterGameObj( kListenerGameObject );
	if ( res == AK_Success ) {
		res = AK::SoundEngine::SetDefaultListeners( &kListenerGameObject, 1 );
	}
	if ( res == AK_Success ) {
		res = AK::SoundEngine::RegisterGameObj( kDebugGameObject );
	}
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
	// AK_EnableGetSourcePlayPosition lets PlayPosition() (wwise_status) track this event.
	*playingID = AK::SoundEngine::PostEvent( eventName, kDebugGameObject, AK_EnableGetSourcePlayPosition );
	*akResult = 0;
	if ( *playingID == AK_INVALID_PLAYING_ID ) {
		*akResult = 1;
		return false;
	}
	return true;
}

void RenderAudio() {
	if ( wwiseStarted ) {
		AK::SoundEngine::RenderAudio();
	}
}

bool StartCapture( const char * path ) {
	// AkOSChar is wchar_t in this file (UNICODE). Convert the ASCII path.
	wchar_t wpath[ 512 ];
	size_t i = 0;
	for ( ; path[ i ] != '\0' && i < 511; i++ ) {
		wpath[ i ] = (wchar_t)(unsigned char)path[ i ];
	}
	wpath[ i ] = L'\0';
	return AK::SoundEngine::StartOutputCapture( wpath ) == AK_Success;
}

int PlayPosition( unsigned int playingID, unsigned int * positionMs ) {
	AkTimeMs pos = 0;
	AKRESULT res = AK::SoundEngine::GetSourcePlayPosition( playingID, &pos, true );
	*positionMs = (unsigned int)pos;
	return (int)res;
}

int OutputDeviceReport( char * out, int outSize ) {
	// Lists the audio devices of the project's 'System' audio device shareset.
	int used = 0;
	AkUniqueID shareset = AK::SoundEngine::GetIDFromString( L"System" );
	AkDeviceDescription devices[ 16 ];
	AkUInt32 count = 16;
	AKRESULT res = AK::SoundEngine::GetDeviceList( shareset, count, devices );
	if ( res != AK_Success ) {
		return (int)res;
	}
	for ( AkUInt32 i = 0; i < count && used < outSize - 1; i++ ) {
		used += snprintf( out + used, outSize - used, "device %u default=%d name=", (unsigned int)devices[ i ].idDevice, devices[ i ].isDefaultDevice ? 1 : 0 );
		for ( int c = 0; devices[ i ].deviceName[ c ] != 0 && c < 120 && used < outSize - 2; c++ ) {
			out[ used++ ] = (char)( devices[ i ].deviceName[ c ] & 0x7f );
		}
		if ( used < outSize - 1 ) {
			out[ used++ ] = '\n';
		}
	}
	out[ used ] = '\0';
	return (int)AK_Success;
}

void SetLogFunc( LogFunc func ) {
	gLogFunc = func;
}

int OutputState( char * out, int outSize ) {
	// Main output as the engine sees it, and the output for the Realtek headphones device.
	AkOutputDeviceID mainOut = AK::SoundEngine::GetOutputID( (AkUniqueID)0, (AkUInt32)0 );
	AkOutputDeviceID realtek = AK::SoundEngine::GetOutputID( "System", (AkUInt32)1132560502 );
	AkUniqueID shareset = AK::SoundEngine::GetIDFromString( "System" );
	int n = _snprintf( out, outSize, "main output id=%u, System shareset id=%u, realtek output id=%u\n",
		(unsigned int)mainOut, (unsigned int)shareset, (unsigned int)realtek );
	return n;
}

int AddSystemOutput( unsigned int * outputID ) {
	// Opens the 'System' audio device shareset explicitly. Returns the SDK result code.
	AkOutputSettings settings( "System", (AkUniqueID)0 );
	AkOutputDeviceID id = 0;
	AKRESULT res = AK::SoundEngine::AddOutput( settings, &id );
	*outputID = (unsigned int)id;
	return (int)res;
}

void StopCapture() {
	AK::SoundEngine::StopOutputCapture();
}

void Stop() {
	if ( !wwiseStarted ) {
		return;
	}
	if ( commSystemStarted ) {
		AK::Comm::Term();
		commSystemStarted = false;
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
