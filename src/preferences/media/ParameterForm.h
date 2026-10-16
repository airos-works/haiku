/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PARAMETER_FORM_H
#define PARAMETER_FORM_H


#include <GroupView.h>
#include <MediaNode.h>
#include <ObjectList.h>
#include <String.h>


class BMessageRunner;
class BParameter;
class BParameterGroup;
class BParameterWeb;
class BStringView;


enum {
	// only the master gain of a mixer, as one "Volume" row
	kFormMasterVolumeOnly	= 0x01
};


/*!	Shows the controls of a BParameterWeb as cards of rows: a label, then a
	horizontal slider with its value, a menu, or a check box. Groups become
	cards; the Auto switch of a camera setting and the Mute of a gain sit on
	the row they belong to. It keeps the controls and the node in sync both
	ways.
*/
class ParameterForm : public BGroupView {
public:
								ParameterForm(BParameterWeb* web,
									const char* nodeName, uint32 flags = 0);
	virtual						~ParameterForm();

			bool				HasControls() const { return fHasControls; }
			bool				IsEmpty() const
									{ return fBindings.IsEmpty(); }

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

	struct Binding;
	struct RowSpec;
	struct CardSpec;
		// (public for the helpers in the implementation)

private:
			void				_Build(const char* nodeName);
			void				_BuildMasterVolume();
			void				_CollectCards(BParameterGroup* group,
									BObjectList<CardSpec, true>& cards);
			void				_MakeRows(BParameterGroup* group,
									CardSpec& card);
			BView*				_MakeRowView(RowSpec& row, BView** _label,
									const BString& rowLabel);
			Binding*			_BindingFor(int32 parameterID);
			void				_UpdatePolling();

			BParameterWeb*		fWeb;
			media_node			fNode;
			BObjectList<Binding, true> fBindings;
			BMessageRunner*		fPollRunner;
			uint32				fFlags;
			bool				fWatching;
			bool				fHasControls;
};


#endif	// PARAMETER_FORM_H
