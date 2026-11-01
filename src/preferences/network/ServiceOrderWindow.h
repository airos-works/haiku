/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SERVICE_ORDER_WINDOW_H
#define SERVICE_ORDER_WINDOW_H


#include <Messenger.h>
#include <StringList.h>
#include <Window.h>


class BButton;
class ServiceOrderListView;


// Sent to the target after the net_server accepted a new order
static const uint32 kMsgServiceOrderChanged = 'SOch';


/*!	Lets the user drag the network interfaces into the order in which they
	should carry the traffic, like "Set Service Order" on macOS.
*/
class ServiceOrderWindow : public BWindow {
public:
								ServiceOrderWindow(BWindow* parent,
									const BMessenger& target);
	virtual						~ServiceOrderWindow();

	virtual	void				MessageReceived(BMessage* message);

	/*!	Returns the interfaces (present ones and remembered absent ones) in
		service order.
	*/
	static	void				GetServiceOrder(BStringList& order);

private:
			void				_Populate();
			void				_UpdateButtons();
			status_t			_Apply();

private:
			BMessenger			fTarget;
			ServiceOrderListView* fListView;
			BButton*			fUpButton;
			BButton*			fDownButton;
			BButton*			fOKButton;
};


#endif	// SERVICE_ORDER_WINDOW_H
