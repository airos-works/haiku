/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AUDIO_PREVIEW_H
#define AUDIO_PREVIEW_H


#include <OS.h>
#include <String.h>

#include <condition_variable>
#include <mutex>
#include <vector>


class BSoundPlayer;
struct AVCodecContext;
struct AVFormatContext;
struct SwrContext;
struct media_raw_audio_format;


/*!	Plays the selected song in the source view: FFmpeg decodes the first
	audio track on a thread of its own into a ring of float stereo samples,
	and a BSoundPlayer takes them from there. As in airTime, the player is
	started once and fed silence while paused - BSoundPlayer does not
	reliably start again after Stop(). */
class AudioPreview {
public:
								AudioPreview();
								~AudioPreview();

			status_t			Open(const char* path, BString* error);
			void				Close();
			bool				IsOpen() const { return fFormat != NULL; }

			void				Play();
			void				Pause();
			bool				IsPlaying() const { return fPlaying; }
			// at the end, playing stops by itself
			bool				AtEnd();

			void				Seek(bigtime_t time);
			bigtime_t			Position();
			bigtime_t			Duration() const { return fDuration; }

			void				SetVolume(float volume);
			void				SetMuted(bool muted);

private:
	static	status_t			_DecodeThread(void* data);
			void				_Decode();
	static	void				_PlayBuffer(void* cookie, void* buffer,
									size_t size,
									const media_raw_audio_format& format);
			void				_Fill(float* buffer, size_t frames);
			bool				_Write(const float* samples, size_t frames,
									int serial);

			AVFormatContext*	fFormat;
			AVCodecContext*		fDecoder;
			SwrContext*			fResampler;
			int					fStream;
			bigtime_t			fDuration;
			BSoundPlayer*		fPlayer;
			thread_id			fThread;

			std::mutex			fLock;
			std::condition_variable	fChanged;
			std::vector<float>	fRing;
			size_t				fCapacity;		// frames
			uint64				fWritten;
			uint64				fRead;
			bigtime_t			fBase;			// media time at fBaseFrame
			uint64				fBaseFrame;
			int					fSerial;		// bumped by every seek
			bigtime_t			fSeekTo;		// -1: none pending
			bool				fDecoded;		// the decoder reached the end
			bool				fQuit;
			bool				fPlaying;
			float				fVolume;
			bool				fMuted;
			float				fGain;
};


#endif	// AUDIO_PREVIEW_H
