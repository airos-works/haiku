/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef THROUGHPUT_VIEW_H
#define THROUGHPUT_VIEW_H


#include <deque>

#include <View.h>


/*!	A small graph of the receive and send rates of an interface over the
	last minutes, in the style of AirTop's network panel.
*/
class ThroughputView : public BView {
public:
								ThroughputView();
	virtual						~ThroughputView();

			void				Reset();
			void				AddSample(uint64 receivedBytes,
									uint64 sentBytes);

	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();

private:
			struct Sample {
				double			receive;
				double			send;
			};

			void				_DrawCurve(BRect rect, bool receive,
									double maximum, rgb_color color);
			double				_Maximum() const;

private:
			std::deque<Sample>	fSamples;
			uint64				fLastReceived;
			uint64				fLastSent;
			bigtime_t			fLastTime;
			bool				fHasLast;
			double				fScale;
};


#endif	// THROUGHPUT_VIEW_H
