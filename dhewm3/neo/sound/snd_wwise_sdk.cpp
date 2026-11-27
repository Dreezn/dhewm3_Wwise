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
#include <AK/SoundEngine/Common/AkQueryParameters.h>
#include <AK/Comm/AkCommunication.h>
// Static plug-ins: including a factory header registers the plug-in with the sound engine
// (Wwise convention). Vorbis decodes the content banks (D-055).
#include <AK/Plugin/AkVorbisDecoderFactory.h>

#include <objbase.h>
#include <AK/Tools/Common/AkMonitorError.h>
#include <stdio.h>
#include <mutex>

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
// Kept at the listener position: global sounds and the player's own sounds play here (D-054).
static const AkGameObjectID	kAtListenerGameObject = 3;
static AkOutputDeviceID		mainOutputID = 0;

// The last mixed frames of the main output, for the HUD and PDA waveform (D-058). The capture
// callback fills it on the audio thread; ReadMixTap copies it out on the game thread.
static const int				MIX_TAP_FRAMES = 4096;		// as OpenAL's MIXBUFFER_SAMPLES
static const int				MIX_TAP_MAX_CHANNELS = 8;
static float					mixTap[ MIX_TAP_FRAMES * MIX_TAP_MAX_CHANNELS ];
static int						mixTapWrite = 0;
static int						mixTapChannels = 0;
static std::mutex				mixTapLock;
static bool						mixTapRegistered = false;
static int						mixTapResult = 0;			// SDK result of RegisterCaptureCallback
static int						mixTapCalls = 0;			// capture callbacks so far
static int						mixTapRenders = 0;			// RenderAudio calls since Start, up to the last registration try

static void MixTapCallback( AkAudioBuffer & buffer, AkOutputDeviceID, void * ) {
	const int channels = (int)buffer.NumChannels();
	const int frames = (int)buffer.uValidFrames;
	const float * data = (const float *)buffer.GetInterleavedData();	// always float interleaved
	mixTapCalls++;
	if ( data == NULL || channels <= 0 ) {
		return;
	}
	const int keep = channels < MIX_TAP_MAX_CHANNELS ? channels : MIX_TAP_MAX_CHANNELS;
	std::lock_guard<std::mutex> lock( mixTapLock );
	mixTapChannels = keep;
	for ( int f = 0; f < frames; f++ ) {
		float * out = &mixTap[ mixTapWrite * MIX_TAP_MAX_CHANNELS ];
		for ( int c = 0; c < keep; c++ ) {
			out[ c ] = data[ f * channels + c ];
		}
		mixTapWrite = ( mixTapWrite + 1 ) % MIX_TAP_FRAMES;
	}
}

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
	// The bank files come from the Wwise project (wwise/dhewm3_Audio), not from the repo.
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

	// Init.bnk is required (project settings, buses, audio devices).
	AkBankID initBankID = 0;
	res = AK::SoundEngine::LoadBank( "Init.bnk", initBankID );
	if ( res != AK_Success ) {
		*failedStage = 5;
		*akResult = (int)res;
		AK::SoundEngine::Term();
		wwiseLowLevelIO.Term();
		AK::IAkStreamMgr::Get()->Destroy();
		AK::MemoryMgr::Term();
		return false;
	}
	// The content banks are loaded by the caller (LoadBank), from the bank list next to the banks.

	// One listener game object, set as the default listener for every emitter.
	// One game object for the debug command, one kept at the listener for global sounds (D-054).
	res = AK::SoundEngine::RegisterGameObj( kListenerGameObject );
	if ( res == AK_Success ) {
		res = AK::SoundEngine::SetDefaultListeners( &kListenerGameObject, 1 );
	}
	if ( res == AK_Success ) {
		res = AK::SoundEngine::RegisterGameObj( kDebugGameObject );
	}
	if ( res == AK_Success ) {
		res = AK::SoundEngine::RegisterGameObj( kAtListenerGameObject );
	}
	mainOutputID = AK::SoundEngine::GetOutputID( AK::SoundEngine::GetIDFromString( "System" ), outputDeviceID );
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
	// The capture callback is registered from RenderAudio: the main output is not an active device yet here.
	mixTapRenders = 0;
	return true;
}

void MixTapStatus( int * registerResult, int * calls ) {
	*registerResult = mixTapResult;
	*calls = mixTapCalls;
}

int ReadMixTap( float * out, int maxFrames, int * channels ) {
	std::lock_guard<std::mutex> lock( mixTapLock );
	*channels = mixTapChannels;
	if ( !mixTapRegistered || mixTapChannels == 0 ) {
		return 0;
	}
	const int frames = maxFrames < MIX_TAP_FRAMES ? maxFrames : MIX_TAP_FRAMES;
	int read = ( mixTapWrite - frames + MIX_TAP_FRAMES ) % MIX_TAP_FRAMES;	// oldest first
	for ( int f = 0; f < frames; f++ ) {
		for ( int c = 0; c < mixTapChannels; c++ ) {
			out[ f * mixTapChannels + c ] = mixTap[ read * MIX_TAP_MAX_CHANNELS + c ];
		}
		read = ( read + 1 ) % MIX_TAP_FRAMES;
	}
	return frames;
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
		// Register the capture callback once the main output is active (D-058). Two tries, a few frames apart,
		// so a failure gives one Wwise warning, not one per frame.
		if ( !mixTapRegistered && ( mixTapRenders == 10 || mixTapRenders == 100 ) ) {
			mixTapResult = (int)AK::SoundEngine::RegisterCaptureCallback( MixTapCallback, mainOutputID );
			mixTapRegistered = ( mixTapResult == AK_Success );
		}
		if ( mixTapRenders <= 100 ) {
			mixTapRenders++;
		}
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

int RegisterObject( unsigned long long objectID ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::RegisterGameObj( (AkGameObjectID)objectID );
}

int UnregisterObject( unsigned long long objectID ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::UnregisterGameObj( (AkGameObjectID)objectID );
}

static AkSoundPosition MakePosition( const float pos[3], const float front[3], const float top[3] ) {
	AkSoundPosition p;
	p.SetPosition( pos[0], pos[1], pos[2] );
	p.SetOrientation( front[0], front[1], front[2], top[0], top[1], top[2] );
	return p;
}

int SetObjectPosition( unsigned long long objectID, const float pos[3], const float front[3], const float top[3] ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SetPosition( (AkGameObjectID)objectID, MakePosition( pos, front, top ) );
}

int SetListenerPosition( const float pos[3], const float front[3], const float top[3] ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	const AkSoundPosition p = MakePosition( pos, front, top );
	AK::SoundEngine::SetPosition( kAtListenerGameObject, p );
	return (int)AK::SoundEngine::SetPosition( kListenerGameObject, p );
}

unsigned long long AtListenerObjectID() {
	return (unsigned long long)kAtListenerGameObject;
}

int LoadBank( const char * bankFile ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	AkBankID bankID = 0;
	return (int)AK::SoundEngine::LoadBank( bankFile, bankID );
}

int SetGlobalRTPC( const char * name, float value, int transitionMs ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SetRTPCValue( name, (AkRtpcValue)value, AK_INVALID_GAME_OBJECT, (AkTimeMs)transitionMs );
}

int SetRTPCOnPlayingID( const char * name, float value, unsigned int playingID, int transitionMs ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SetRTPCValueByPlayingID( name, (AkRtpcValue)value, (AkPlayingID)playingID, (AkTimeMs)transitionMs );
}

int SetScalingFactor( unsigned long long objectID, float factor ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SetScalingFactor( (AkGameObjectID)objectID, (AkReal32)factor );
}

int SetOutputVolume( float volume ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SetOutputVolume( mainOutputID, (AkReal32)volume );
}

int SetState( const char * group, const char * state ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SetState( group, state );
}

void PausePlayingID( unsigned int playingID ) {
	if ( wwiseStarted ) {
		AK::SoundEngine::ExecuteActionOnPlayingID( AK::SoundEngine::AkActionOnEventType_Pause, (AkPlayingID)playingID );
	}
}

void ResumePlayingID( unsigned int playingID ) {
	if ( wwiseStarted ) {
		AK::SoundEngine::ExecuteActionOnPlayingID( AK::SoundEngine::AkActionOnEventType_Resume, (AkPlayingID)playingID );
	}
}

unsigned int PostEventOnObject( const char * eventName, unsigned long long objectID ) {
	if ( !wwiseStarted ) {
		return 0;
	}
	return (unsigned int)AK::SoundEngine::PostEvent( eventName, (AkGameObjectID)objectID, AK_EnableGetSourcePlayPosition );
}

int SeekPlayingID( const char * eventName, unsigned long long objectID, int positionMs, unsigned int playingID ) {
	if ( !wwiseStarted ) {
		return (int)AK_Fail;
	}
	return (int)AK::SoundEngine::SeekOnEvent( eventName, (AkGameObjectID)objectID, (AkTimeMs)positionMs, false, (AkPlayingID)playingID );
}

int PlayingIDsOnObject( unsigned long long objectID, unsigned int * ids, int maxIDs ) {
	if ( !wwiseStarted || maxIDs <= 0 ) {
		return 0;
	}
	AkPlayingID found[ 64 ];
	AkUInt32 count = maxIDs < 64 ? (AkUInt32)maxIDs : 64;
	if ( AK::SoundEngine::Query::GetPlayingIDsFromGameObject( (AkGameObjectID)objectID, count, found ) != AK_Success ) {
		return 0;
	}
	for ( AkUInt32 i = 0; i < count; i++ ) {
		ids[ i ] = (unsigned int)found[ i ];
	}
	return (int)count;
}

void StopObject( unsigned long long objectID ) {
	if ( wwiseStarted ) {
		AK::SoundEngine::StopAll( (AkGameObjectID)objectID );
	}
}

void StopPlayingID( unsigned int playingID ) {
	if ( wwiseStarted ) {
		AK::SoundEngine::StopPlayingID( (AkPlayingID)playingID );
	}
}

void StopAll() {
	if ( wwiseStarted ) {
		AK::SoundEngine::StopAll();
	}
}

void StopCapture() {
	AK::SoundEngine::StopOutputCapture();
}

void Stop() {
	if ( !wwiseStarted ) {
		return;
	}
	// Clear the flag first: the async sound tick checks it before calling RenderAudio.
	wwiseStarted = false;
	if ( mixTapRegistered ) {
		AK::SoundEngine::UnregisterCaptureCallback( MixTapCallback, mainOutputID );
		mixTapRegistered = false;
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
