/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */
#ifndef LAB_SPEAKER_H
#define LAB_SPEAKER_H


#include <BufferConsumer.h>
#include <MediaEventLooper.h>


/*!	A physical audio output that goes nowhere: a sound card for tests of
	what lists and chooses audio devices, on machines without a second one.
*/
class LabSpeaker : public virtual BBufferConsumer, public BMediaEventLooper {
public:
								LabSpeaker(BMediaAddOn* addOn, int32 id);
	virtual						~LabSpeaker();

protected:
	virtual	void				NodeRegistered();
	virtual	BMediaAddOn*		AddOn(int32* _internalID) const;

	virtual	status_t			AcceptFormat(const media_destination& dest,
									media_format* format);
	virtual	status_t			GetNextInput(int32* cookie,
									media_input* _input);
	virtual	void				DisposeInputCookie(int32 cookie);
	virtual	void				BufferReceived(BBuffer* buffer);
	virtual	void				ProducerDataStatus(
									const media_destination& forWhom,
									int32 status, bigtime_t atPerformanceTime);
	virtual	status_t			GetLatencyFor(const media_destination& forWhom,
									bigtime_t* _latency,
									media_node_id* _timesource);
	virtual	status_t			Connected(const media_source& producer,
									const media_destination& where,
									const media_format& withFormat,
									media_input* _input);
	virtual	void				Disconnected(const media_source& producer,
									const media_destination& where);
	virtual	status_t			FormatChanged(const media_source& producer,
									const media_destination& consumer,
									int32 changeTag,
									const media_format& format);

	virtual status_t			HandleMessage(int32 message,
									const void* data, size_t size);
	virtual void				HandleEvent(const media_timed_event* event,
							 		bigtime_t lateness,
							 		bool realTimeEvent = false);

private:
			void				_InitInput();

			BMediaAddOn*		fAddOn;
			int32				fID;
			media_input			fInput;
};


#endif	// LAB_SPEAKER_H
