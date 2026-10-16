/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LIVE_VIEWS_H
#define LIVE_VIEWS_H


#include <Locker.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <Messenger.h>
#include <String.h>
#include <View.h>


class BBitmap;
class BButton;
class BMediaRecorder;
class BMessageRunner;


/*!	Shows what a camera sees while it is on screen, so its settings can be
	tried out. It takes the camera's free output; when an application uses
	the camera, it says so instead.
*/
class CameraPreview : public BView {
public:
								CameraPreview(const media_node& node);
	virtual						~CameraPreview();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MessageReceived(BMessage* message);

	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();
	virtual	bool				HasHeightForWidth();
	virtual	void				GetHeightForWidth(float width, float* min,
									float* max, float* preferred);

			void				Start();
			void				Stop();
			bool				IsRunning() const { return fRunning; }

private:
	static	status_t			_StartThread(void* data);
	static	void				_ProcessFrame(void* cookie,
									bigtime_t timestamp, void* data,
									size_t size, const media_format& format);
			void				_Frame(const void* data, size_t size,
									const media_format& format);
			void				_SetStatus(const char* status);
			void				_WaitForStart();

			media_node			fNode;
			BMediaRecorder*		fRecorder;
			thread_id			fStartThread;
			BMessenger			fMessenger;

			BLocker				fLock;
			BBitmap*			fFrame;
			int32				fFramePending;
			BString				fStatus;
			float				fAspect;
			bool				fRunning;
};


/*!	A level meter for the default sound input: shows that the microphone
	hears something.
*/
class LevelMeter : public BView {
public:
								LevelMeter();
	virtual						~LevelMeter();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MessageReceived(BMessage* message);

	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();

private:
	static	status_t			_StartThread(void* data);
	static	void				_ProcessBuffer(void* cookie,
									bigtime_t timestamp, void* data,
									size_t size, const media_format& format);
			void				_Stop();
			void				_SetStatus(const char* status);

			BMediaRecorder*		fRecorder;
			media_node			fInput;
			bool				fHasInput;
			thread_id			fStartThread;
			BMessenger			fMessenger;
			BMessageRunner*		fRunner;

			BLocker				fLock;
			float				fPeak;
				// written by the recorder, taken by the pulse
			float				fLevel;
			float				fHold;
			bigtime_t			fHoldTime;
			BString				fStatus;
};


#endif	// LIVE_VIEWS_H
