/*
 * Copyright 2005, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */
#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H


#include <Locker.h>
#include <Region.h>
#include <ObjectList.h>


class EventStream;

class InputManager : public BLocker {
	public:
		InputManager();
		virtual ~InputManager();

		void UpdateScreenBounds(BRect bounds, const BRegion* displays = NULL,
			BRect touchScreen = BRect(), int32 touchRotation = 0);

		bool AddStream(EventStream* stream);
		void RemoveStream(EventStream* stream);

		EventStream* GetStream();
		void PutStream(EventStream* stream);

	private:
		BObjectList<EventStream, true> fFreeStreams;
		BObjectList<EventStream, true> fUsedStreams;
};

extern InputManager* gInputManager;

#endif	/* INPUT_MANAGER_H */
