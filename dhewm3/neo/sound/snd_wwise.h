/*
===========================================================================

dhewm3 Wwise backend: owns the Wwise sound engine's life cycle.

Built only when WWISE_ROOT is set at configure time (defines WWISE_ENABLED).
Without it, Init() returns false and nothing else changes.

===========================================================================
*/

#ifndef __SND_WWISE_H__
#define __SND_WWISE_H__

class idWwiseEngine {
public:
	idWwiseEngine();
	~idWwiseEngine();

	// Starts memory manager, stream manager (SDK default file I/O) and sound engine.
	bool			Init();
	// Stops everything started by Init(), in reverse order.
	void			Shutdown();
	// Per-tick update (RenderAudio). Called from dhewm3's sound tick.
	void			Update();
	bool			IsInitialized() const { return initialized; }

private:
	bool			initialized;
};

extern idWwiseEngine wwiseEngine;

#endif
