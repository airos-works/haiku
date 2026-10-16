/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ParameterForm.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <vector>

#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <GroupLayout.h>
#include <MediaRoster.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <ParameterWeb.h>
#include <PopUpMenu.h>
#include <Slider.h>
#include <SpaceLayoutItem.h>
#include <StringView.h>
#include <TextControl.h>

#include "Cards.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Device controls"


static const uint32 kMsgSliderMoved = 'pfSM';
static const uint32 kMsgSliderDone = 'pfSD';
static const uint32 kMsgCheck = 'pfCK';
static const uint32 kMsgTrailing = 'pfTR';
static const uint32 kMsgChoice = 'pfCH';
static const uint32 kMsgText = 'pfTX';
static const uint32 kMsgPoll = 'pfPL';

// Slider moves are sent to the node at most this often...
static const bigtime_t kApplyInterval = 25000;
// ...and the node's own notifications do not move a slider the user just
// touched.
static const bigtime_t kUserIdleTime = 400000;


enum row_type {
	ROW_SLIDER,
	ROW_BALANCE,
	ROW_CHECK,
	ROW_CHOICE,
	ROW_VALUE,
	ROW_TEXT,
	ROW_STATUS,
	ROW_CAPTION
};


enum trailing_type {
	TRAILING_NONE,
	TRAILING_MUTE,
	TRAILING_AUTO,
	TRAILING_ENABLE
};


struct ParameterForm::RowSpec {
	RowSpec()
		:
		type(ROW_CAPTION),
		parameter(NULL),
		trailing(NULL),
		trailingType(TRAILING_NONE),
		genericLabel(false)
	{
	}

	row_type			type;
	BParameter*			parameter;
	BString				label;
		// the parameter's own label
	BString				rowLabel;
		// shown in the label column instead, when set
	BString				text;
	BParameter*			trailing;
	trailing_type		trailingType;
	bool				genericLabel;
		// the parameter has no name, the label says what kind it is
	BString				toolTip;
};


struct ParameterForm::CardSpec {
	BString				title;
	std::vector<RowSpec> rows;
	std::vector<RowSpec> status;

	int32 CountControlRows() const
	{
		int32 count = 0;
		for (size_t i = 0; i < rows.size(); i++) {
			if (rows[i].type != ROW_CAPTION)
				count++;
		}
		return count;
	}
};


struct ParameterForm::Binding {
	Binding()
		:
		parameter(NULL),
		type(ROW_CAPTION),
		slider(NULL),
		valueView(NULL),
		check(NULL),
		menuField(NULL),
		textControl(NULL),
		textView(NULL),
		trailing(NULL),
		trailingType(TRAILING_NONE),
		trailingCheck(NULL),
		row(NULL),
		inTrailingColumn(false),
		minimum(0),
		maximum(1),
		step(1),
		steps(1),
		dragging(false),
		lastApply(0),
		lastUser(0)
	{
	}

	BParameter*			parameter;
	row_type			type;

	BSlider*			slider;
	BStringView*		valueView;
	BCheckBox*			check;
	BMenuField*			menuField;
	BTextControl*		textControl;
	BStringView*		textView;

	BParameter*			trailing;
	trailing_type		trailingType;
	BCheckBox*			trailingCheck;
	BGroupView*			row;
	bool				inTrailingColumn;
		// a lone Mute, shown where the Mute of a slider is

	float				minimum;
	float				maximum;
	float				step;
	int32				steps;
	std::vector<float>	values;
	std::vector<float>	base;
	bool				dragging;
	bigtime_t			lastApply;
	bigtime_t			lastUser;

	BContinuousParameter* Continuous() const
	{
		return static_cast<BContinuousParameter*>(parameter);
	}

	int32 ToSlider(float value) const
	{
		int32 position = (int32)roundf((value - minimum) / step);
		return std::max((int32)0, std::min(steps, position));
	}

	float FromSlider(int32 position) const
	{
		return std::min(maximum, minimum + position * step);
	}

	float TopValue() const
	{
		float top = values.empty() ? minimum : values[0];
		for (size_t i = 1; i < values.size(); i++)
			top = std::max(top, values[i]);
		return top;
	}
};


// #pragma mark - helpers


static BString
clean_name(const char* name)
{
	BString text(name != NULL ? name : "");
	text.ReplaceAll("<NULL>", "");
	text.Trim();
	while (text.EndsWith(":")) {
		text.Truncate(text.Length() - 1);
		text.Trim();
	}
	return text;
}


static bool
is_kind(BParameter* parameter, const char* kind)
{
	return strcmp(parameter->Kind(), kind) == 0;
}


static bool
is_gain(BParameter* parameter)
{
	return parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER
		&& (is_kind(parameter, B_GAIN) || is_kind(parameter, B_MASTER_GAIN));
}


static bool
is_toggle(BParameter* parameter)
{
	return parameter->Type() == BParameter::B_DISCRETE_PARAMETER
		&& (is_kind(parameter, B_ENABLE) || is_kind(parameter, B_MUTE));
}


//! The same rule the default media theme hides parameters by.
static bool
is_hidden(BParameter* parameter)
{
	if ((parameter->Flags() & B_HIDDEN_PARAMETER) != 0)
		return true;

	if (parameter->Type() != BParameter::B_NULL_PARAMETER
		|| !is_kind(parameter, B_WEB_PHYSICAL_INPUT)) {
		return false;
	}

	for (int32 i = 0; i < parameter->CountOutputs(); i++) {
		if (is_kind(parameter->OutputAt(i), B_INPUT_MUX))
			return true;
	}
	return false;
}


//! "Automatic white balance" -> "white balance"
static BString
automatic_target(const BString& name)
{
	static const char* const kPrefixes[] = { "Automatic ", "Auto " };
	for (size_t i = 0; i < B_COUNT_OF(kPrefixes); i++) {
		size_t length = strlen(kPrefixes[i]);
		if (strncasecmp(name.String(), kPrefixes[i], length) == 0) {
			BString target(name.String() + length);
			target.Trim();
			return target;
		}
	}
	return BString();
}


static bool
read_continuous(BContinuousParameter* parameter, std::vector<float>& values)
{
	int32 count = std::max((int32)1, parameter->CountChannels());
	std::vector<float> buffer(count, 0.0f);
	size_t size = count * sizeof(float);
	bigtime_t when;
	if (parameter->GetValue(buffer.data(), &size, &when) != B_OK
		|| size < sizeof(float)) {
		return false;
	}
	for (size_t i = size / sizeof(float); i < (size_t)count; i++)
		buffer[i] = buffer[0];
	values = buffer;
	return true;
}


static bool
read_discrete(BParameter* parameter, int32& value)
{
	size_t size = sizeof(int32);
	bigtime_t when;
	return parameter->GetValue(&value, &size, &when) == B_OK;
}


static BString
read_text(BParameter* parameter)
{
	BTextParameter* text = static_cast<BTextParameter*>(parameter);
	size_t size = std::max((size_t)1, (size_t)text->MaxBytes()) + 1;
	std::vector<char> buffer(size, '\0');
	bigtime_t when;
	if (parameter->GetValue(buffer.data(), &size, &when) != B_OK)
		return BString();
	buffer[buffer.size() - 1] = '\0';
	return BString(buffer.data());
}


static BString
format_value(const ParameterForm::Binding& binding, float value)
{
	BString text;
	if (binding.type == ROW_BALANCE) {
		float middle = (binding.minimum + binding.maximum) / 2;
		float half = (binding.maximum - binding.minimum) / 2;
		int32 percent = half > 0
			? (int32)roundf(fabsf(value - middle) / half * 100) : 0;
		if (percent == 0)
			return B_TRANSLATE("Center");
		BString number;
		number << percent;
		text = value < middle ? B_TRANSLATE("%percent%% left")
			: B_TRANSLATE("%percent%% right");
		text.ReplaceFirst("%percent%", number);
		return text;
	}

	BContinuousParameter* parameter = binding.Continuous();
	float step = parameter->ValueStep();
	// as many decimals as the step has: 1, 0.5, 0.75
	int decimals = 0;
	if (step > 0) {
		while (decimals < 2) {
			float scaled = step * powf(10, decimals);
			if (fabsf(scaled - roundf(scaled)) < 0.001f)
				break;
			decimals++;
		}
	} else if (binding.maximum - binding.minimum <= 10)
		decimals = 1;
	if (strcmp(parameter->Unit(), "dB") == 0)
		decimals = std::min(decimals, 1);

	// no "-0.0"
	if (fabsf(value) < 0.5f * powf(10, -decimals))
		value = 0;
	text.SetToFormat("%.*f", decimals, value);
	if (parameter->Unit()[0] != '\0')
		text << " " << parameter->Unit();
	return text;
}


static void
refresh_trailing(ParameterForm::Binding* binding)
{
	if (binding->trailingCheck == NULL)
		return;

	int32 value;
	if (read_discrete(binding->trailing, value))
		binding->trailingCheck->SetValue(value != 0);
	if (binding->trailingType == TRAILING_AUTO && binding->slider != NULL)
		binding->slider->SetEnabled(binding->trailingCheck->Value() == 0);
}


static void
refresh_binding(ParameterForm::Binding* binding)
{
	BParameter* parameter = binding->parameter;

	switch (binding->type) {
		case ROW_SLIDER:
		case ROW_BALANCE:
		{
			if (!read_continuous(binding->Continuous(), binding->values))
				break;
			if (!binding->dragging) {
				float top = binding->TopValue();
				binding->slider->SetValue(binding->ToSlider(top));
				binding->valueView->SetText(format_value(*binding, top));
			}
			break;
		}

		case ROW_CHECK:
		{
			if (parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER) {
				if (read_continuous(binding->Continuous(), binding->values)) {
					binding->check->SetValue(binding->values[0]
						> (binding->minimum + binding->maximum) / 2);
				}
			} else {
				int32 value;
				if (read_discrete(parameter, value))
					binding->check->SetValue(value != 0);
			}
			break;
		}

		case ROW_CHOICE:
		{
			int32 value;
			if (!read_discrete(parameter, value))
				break;
			BMenu* menu = binding->menuField->Menu();
			for (int32 i = 0; i < menu->CountItems(); i++) {
				BMenuItem* item = menu->ItemAt(i);
				int32 itemValue;
				if (item->Message() != NULL
					&& item->Message()->FindInt32("value", &itemValue) == B_OK
					&& itemValue == value) {
					item->SetMarked(true);
					break;
				}
			}
			break;
		}

		case ROW_TEXT:
			if (binding->textControl->TextView() == NULL
				|| !binding->textControl->TextView()->IsFocus()) {
				binding->textControl->SetText(read_text(parameter));
			}
			break;

		case ROW_STATUS:
		{
			BString text = read_text(parameter);
			binding->textView->SetText(text);
			if (text.IsEmpty() != binding->textView->IsHidden(
					binding->textView)) {
				if (text.IsEmpty())
					binding->textView->Hide();
				else
					binding->textView->Show();
			}
			break;
		}

		case ROW_VALUE:
		case ROW_CAPTION:
			break;
	}

	refresh_trailing(binding);
}


static void
set_continuous(ParameterForm::Binding* binding,
	const std::vector<float>& values)
{
	binding->parameter->SetValue(values.data(), values.size() * sizeof(float),
		-1);
	binding->values = values;
	binding->lastApply = system_time();
}


static BMessage*
binding_message(uint32 what, int32 index)
{
	BMessage* message = new BMessage(what);
	message->AddInt32("binding", index);
	return message;
}


// #pragma mark - ParameterForm


ParameterForm::ParameterForm(BParameterWeb* web, const char* nodeName,
	uint32 flags, const IconMap* icons)
	:
	BGroupView("parameter form", B_VERTICAL, B_USE_DEFAULT_SPACING),
	fWeb(web),
	fNode(web->Node()),
	fBindings(20),
	fPollRunner(NULL),
	fFlags(flags),
	fIcons(icons),
	fWatching(false),
	fHasControls(false)
{
	if ((flags & kFormMasterVolumeOnly) != 0)
		_BuildMasterVolume();
	else
		_Build(nodeName);

	for (int32 i = 0; i < fBindings.CountItems(); i++) {
		row_type type = fBindings.ItemAt(i)->type;
		if (type != ROW_STATUS && type != ROW_CAPTION && type != ROW_VALUE)
			fHasControls = true;
	}

	// only used while building
	fIcons = NULL;
}


ParameterForm::~ParameterForm()
{
	delete fPollRunner;
	fBindings.MakeEmpty();
	delete fWeb;
}


void
ParameterForm::AttachedToWindow()
{
	BGroupView::AttachedToWindow();

	BMessenger messenger(this);
	for (int32 i = 0; i < fBindings.CountItems(); i++) {
		Binding* binding = fBindings.ItemAt(i);
		if (binding->slider != NULL)
			binding->slider->SetTarget(messenger);
		if (binding->check != NULL)
			binding->check->SetTarget(messenger);
		if (binding->trailingCheck != NULL)
			binding->trailingCheck->SetTarget(messenger);
		if (binding->menuField != NULL)
			binding->menuField->Menu()->SetTargetForItems(messenger);
		if (binding->textControl != NULL)
			binding->textControl->SetTarget(messenger);
	}

	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster != NULL && roster->StartWatching(messenger, fNode,
			B_MEDIA_NEW_PARAMETER_VALUE) == B_OK) {
		fWatching = true;
	}
	_UpdatePolling();
}


void
ParameterForm::DetachedFromWindow()
{
	if (fWatching) {
		BMediaRoster* roster = BMediaRoster::CurrentRoster();
		if (roster != NULL) {
			roster->StopWatching(BMessenger(this), fNode,
				B_MEDIA_NEW_PARAMETER_VALUE);
		}
		fWatching = false;
	}
	delete fPollRunner;
	fPollRunner = NULL;

	BGroupView::DetachedFromWindow();
}


void
ParameterForm::MessageReceived(BMessage* message)
{
	int32 index;
	Binding* binding = NULL;
	if (message->FindInt32("binding", &index) == B_OK)
		binding = fBindings.ItemAt(index);

	switch (message->what) {
		case kMsgSliderMoved:
		case kMsgSliderDone:
		{
			if (binding == NULL || binding->slider == NULL)
				break;

			bool final = message->what == kMsgSliderDone;
			bigtime_t now = system_time();
			binding->lastUser = now;
			if (!binding->dragging) {
				binding->dragging = true;
				binding->base = binding->values;
				if (binding->base.empty())
					binding->base.push_back(binding->minimum);
			}

			float value = binding->FromSlider(binding->slider->Value());
			binding->valueView->SetText(format_value(*binding, value));

			// a single slider moves all channels, keeping their balance
			std::vector<float> values(binding->base);
			float top = values[0];
			for (size_t i = 1; i < values.size(); i++)
				top = std::max(top, values[i]);
			for (size_t i = 0; i < values.size(); i++) {
				values[i] = std::max(binding->minimum,
					std::min(binding->maximum, values[i] + value - top));
			}

			if (final || now - binding->lastApply >= kApplyInterval)
				set_continuous(binding, values);
			if (final)
				binding->dragging = false;
			break;
		}

		case kMsgCheck:
		{
			if (binding == NULL || binding->check == NULL)
				break;
			bool on = binding->check->Value() != 0;
			if (binding->parameter->Type()
					== BParameter::B_CONTINUOUS_PARAMETER) {
				int32 channels = std::max((int32)1,
					binding->Continuous()->CountChannels());
				set_continuous(binding, std::vector<float>(channels,
					on ? binding->maximum : binding->minimum));
			} else {
				int32 value = on ? 1 : 0;
				binding->parameter->SetValue(&value, sizeof(value), -1);
			}
			break;
		}

		case kMsgTrailing:
		{
			if (binding == NULL || binding->trailingCheck == NULL)
				break;
			int32 value = binding->trailingCheck->Value() != 0 ? 1 : 0;
			binding->trailing->SetValue(&value, sizeof(value), -1);
			if (binding->trailingType == TRAILING_AUTO) {
				binding->slider->SetEnabled(value == 0);
				// the device picks the value itself now, or keeps the one it
				// last picked
				binding->lastUser = 0;
				refresh_binding(binding);
				_UpdatePolling();
			}
			break;
		}

		case kMsgChoice:
		{
			int32 value;
			if (binding != NULL && message->FindInt32("value", &value) == B_OK)
				binding->parameter->SetValue(&value, sizeof(value), -1);
			break;
		}

		case kMsgText:
		{
			if (binding == NULL || binding->textControl == NULL)
				break;
			BString text = binding->textControl->Text();
			binding->parameter->SetValue(text.String(), text.Length() + 1, -1);
			break;
		}

		case kMsgPoll:
			for (int32 i = 0; i < fBindings.CountItems(); i++) {
				Binding* item = fBindings.ItemAt(i);
				if (item->trailingType == TRAILING_AUTO
					&& item->trailingCheck->Value() != 0) {
					refresh_binding(item);
				}
			}
			break;

		case B_MEDIA_NEW_PARAMETER_VALUE:
		{
			int32 parameterID;
			if (message->FindInt32("parameter", &parameterID) != B_OK)
				break;
			binding = _BindingFor(parameterID);
			if (binding == NULL)
				break;
			if (binding->dragging
				|| system_time() - binding->lastUser < kUserIdleTime) {
				refresh_trailing(binding);
				break;
			}
			refresh_binding(binding);
			break;
		}

		default:
			BGroupView::MessageReceived(message);
			break;
	}
}


// #pragma mark - building


/*!	A row that moves into a card of several groups is labelled by the group
	it came from, unless its own name says more.
*/
static void
label_by_group(ParameterForm::RowSpec& row, const BString& group,
	bool keepNamed)
{
	if (row.type == ROW_CAPTION || group.IsEmpty() || group == row.label
		|| row.label.IFindFirst(group) >= 0) {
		// "Input frequency" says enough in a group "Input"
		return;
	}
	if (keepNamed && !row.genericLabel && row.type != ROW_CHECK)
		return;
	row.rowLabel = group;
}


/*!	The sliders of a card get value fields of one width, and room for the
	Auto or Mute switch even where there is none, so they line up.
*/
static void
align_rows(BObjectList<ParameterForm::Binding, true>& bindings, int32 first,
	int32 last)
{
	float valueWidth = 0;
	float trailingWidth = 0;
	for (int32 i = first; i < last; i++) {
		ParameterForm::Binding* binding = bindings.ItemAt(i);
		if (binding->valueView == NULL)
			continue;
		valueWidth = std::max(valueWidth,
			binding->valueView->ExplicitMinSize().width);
		if (binding->trailingCheck != NULL) {
			trailingWidth = std::max(trailingWidth,
				binding->trailingCheck->PreferredSize().width);
		}
	}

	float spacing = ceilf(be_control_look->DefaultItemSpacing() / 2);
	for (int32 i = first; i < last; i++) {
		ParameterForm::Binding* binding = bindings.ItemAt(i);
		if (binding->inTrailingColumn && trailingWidth > 0) {
			binding->check->SetExplicitMinSize(
				BSize(trailingWidth, B_SIZE_UNSET));
		}
		if (binding->valueView == NULL || binding->row == NULL)
			continue;
		binding->valueView->SetExplicitSize(BSize(valueWidth, B_SIZE_UNSET));
		if (binding->trailingCheck != NULL) {
			binding->trailingCheck->SetExplicitMinSize(
				BSize(trailingWidth, B_SIZE_UNSET));
		} else if (trailingWidth > 0) {
			BGroupLayout* layout = binding->row->GroupLayout();
			layout->AddItem(BSpaceLayoutItem::CreateHorizontalStrut(spacing));
			layout->AddItem(
				BSpaceLayoutItem::CreateHorizontalStrut(trailingWidth));
		}
	}
}


void
ParameterForm::_Build(const char* nodeName)
{
	struct Section {
		BString heading;
		BObjectList<CardSpec, true> cards;
	};

	BObjectList<Section, true> sections;
	std::vector<RowSpec> status;
	BString name = clean_name(nodeName);

	for (int32 i = 0; i < fWeb->CountGroups(); i++) {
		BParameterGroup* group = fWeb->GroupAt(i);
		if (group == NULL || (group->Flags() & B_HIDDEN_PARAMETER) != 0)
			continue;

		Section* section = new Section;
		section->heading = clean_name(group->Name());
		if (section->heading == name)
			section->heading = "";
		_CollectCards(group, section->cards);

		BObjectList<CardSpec, true>& cards = section->cards;

		// status lines go to the top of the form
		for (int32 j = cards.CountItems() - 1; j >= 0; j--) {
			CardSpec* card = cards.ItemAt(j);
			status.insert(status.begin(), card->status.begin(),
				card->status.end());
			if (card->rows.empty())
				delete cards.RemoveItemAt(j);
		}

		// single settings of the same name in a row are one card
		for (int32 j = cards.CountItems() - 1; j > 0; j--) {
			CardSpec* card = cards.ItemAt(j);
			CardSpec* previous = cards.ItemAt(j - 1);
			if (card->title != previous->title
				|| card->CountControlRows() > 1
				|| previous->CountControlRows() > 1) {
				continue;
			}
			previous->rows.insert(previous->rows.end(), card->rows.begin(),
				card->rows.end());
			delete cards.RemoveItemAt(j);
		}

		bool allSingle = cards.CountItems() > 1;
		for (int32 j = 0; j < cards.CountItems() && allSingle; j++) {
			if (cards.ItemAt(j)->CountControlRows() != 1)
				allSingle = false;
		}

		if (allSingle) {
			// one card of many single settings, labelled by their groups
			CardSpec* merged = new CardSpec;
			merged->title = section->heading;
			for (int32 j = 0; j < cards.CountItems(); j++) {
				CardSpec* card = cards.ItemAt(j);
				// a group's notes ("48 kHz float") become its tool tip
				BString notes;
				for (size_t k = 0; k < card->rows.size(); k++) {
					if (card->rows[k].type != ROW_CAPTION)
						continue;
					if (!notes.IsEmpty())
						notes << "\n";
					notes << card->rows[k].text;
				}
				for (size_t k = 0; k < card->rows.size(); k++) {
					RowSpec row = card->rows[k];
					if (row.type == ROW_CAPTION)
						continue;
					label_by_group(row, card->title, false);
					row.toolTip = notes;
					merged->rows.push_back(row);
				}
			}
			cards.MakeEmpty();
			cards.AddItem(merged);
			section->heading = "";
		} else if (cards.CountItems() == 1) {
			CardSpec* card = cards.ItemAt(0);
			if (card->title.IsEmpty() || card->title == section->heading) {
				card->title = section->heading;
				section->heading = "";
			}
		}

		// "Line", "Line 2", "Line 3"
		for (int32 j = 1; j < cards.CountItems(); j++) {
			CardSpec* card = cards.ItemAt(j);
			if (card->title.IsEmpty())
				continue;
			int32 count = 1;
			for (int32 k = 0; k < j; k++) {
				BString other = cards.ItemAt(k)->title;
				if (other == card->title
					|| (other.StartsWith(card->title)
						&& other.Length() > card->title.Length()
						&& other[card->title.Length()] == ' ')) {
					count++;
				}
			}
			if (count > 1)
				card->title << " " << count;
		}

		if (cards.IsEmpty())
			delete section;
		else
			sections.AddItem(section);
	}

	// A few settings in many groups are one card.
	int32 cardCount = 0;
	int32 rowCount = 0;
	for (int32 i = 0; i < sections.CountItems(); i++) {
		BObjectList<CardSpec, true>& cards = sections.ItemAt(i)->cards;
		cardCount += cards.CountItems();
		for (int32 j = 0; j < cards.CountItems(); j++)
			rowCount += cards.ItemAt(j)->CountControlRows();
	}
	if (cardCount > 1 && rowCount <= 4) {
		Section* section = new Section;
		CardSpec* merged = new CardSpec;
		for (int32 i = 0; i < sections.CountItems(); i++) {
			BObjectList<CardSpec, true>& cards = sections.ItemAt(i)->cards;
			for (int32 j = 0; j < cards.CountItems(); j++) {
				CardSpec* card = cards.ItemAt(j);
				BString group = card->title.IsEmpty()
					? sections.ItemAt(i)->heading : card->title;
				for (size_t k = 0; k < card->rows.size(); k++) {
					RowSpec row = card->rows[k];
					label_by_group(row, group, true);
					merged->rows.push_back(row);
				}
			}
		}
		sections.MakeEmpty();
		section->cards.AddItem(merged);
		sections.AddItem(section);
	}

	// "Mute", "Mute 2" in one card
	for (int32 i = 0; i < sections.CountItems(); i++) {
		BObjectList<CardSpec, true>& cards = sections.ItemAt(i)->cards;
		for (int32 j = 0; j < cards.CountItems(); j++) {
			std::vector<RowSpec>& rows = cards.ItemAt(j)->rows;
			for (size_t k = 1; k < rows.size(); k++) {
				if (rows[k].type != ROW_CHECK || !rows[k].rowLabel.IsEmpty())
					continue;
				int32 count = 1;
				for (size_t l = 0; l < k; l++) {
					if (rows[l].type == ROW_CHECK
						&& rows[l].rowLabel.IsEmpty()
						&& clean_name(rows[l].parameter->Name())
							== rows[k].label) {
						count++;
					}
				}
				if (count > 1)
					rows[k].label << " " << count;
			}
		}
	}

	BGroupLayout* layout = GroupLayout();
	float spacing = be_control_look->DefaultItemSpacing();
	layout->SetSpacing(spacing);

	for (size_t i = 0; i < status.size(); i++) {
		BView* label = NULL;
		BView* view = _MakeRowView(status[i], &label, BString());
		layout->AddView(view);
	}

	std::vector<Card*> cardViews;
	std::vector<bool> cardHasSliders;
	bool showHeadings = sections.CountItems() > 1;
	BFont headingFont(be_bold_font);
	headingFont.SetSize(ceilf(headingFont.Size() * 1.15f));

	for (int32 i = 0; i < sections.CountItems(); i++) {
		Section* section = sections.ItemAt(i);
		if (showHeadings && !section->heading.IsEmpty()) {
			if (layout->CountItems() > 0) {
				layout->AddItem(BSpaceLayoutItem::CreateVerticalStrut(
					spacing));
			}
			BStringView* heading = make_section_title(section->heading);
			heading->SetFont(&headingFont);
			layout->AddView(heading);
		}

		for (int32 j = 0; j < section->cards.CountItems(); j++) {
			CardSpec* spec = section->cards.ItemAt(j);
			if (!spec->title.IsEmpty())
				layout->AddView(make_section_title(spec->title));

			Card* card = new Card();
			int32 first = fBindings.CountItems();
			for (size_t k = 0; k < spec->rows.size(); k++) {
				RowSpec& row = spec->rows[k];
				BView* label = NULL;
				BView* control = _MakeRowView(row, &label, row.rowLabel);
				card->AddRow(label, control);
			}
			align_rows(fBindings, first, fBindings.CountItems());
			layout->AddView(card);
			cardViews.push_back(card);
			bool sliders = false;
			for (int32 k = first; k < fBindings.CountItems(); k++) {
				if (fBindings.ItemAt(k)->slider != NULL)
					sliders = true;
			}
			cardHasSliders.push_back(sliders);
		}
	}

	// the sliders of all cards line up (a card of menus keeps its own
	// labels, so that long ones there do not shorten the sliders)
	float labelWidth = 0;
	for (size_t i = 0; i < cardViews.size(); i++) {
		if (cardHasSliders[i]) {
			labelWidth = std::max(labelWidth,
				cardViews[i]->NaturalLabelWidth());
		}
	}
	for (size_t i = 0; i < cardViews.size(); i++) {
		if (cardHasSliders[i])
			cardViews[i]->SetLabelWidth(labelWidth);
	}
}


static BParameterGroup*
find_master_group(BParameterGroup* group)
{
	for (int32 i = 0; i < group->CountParameters(); i++) {
		BParameter* parameter = group->ParameterAt(i);
		if (parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER
			&& is_kind(parameter, B_MASTER_GAIN)) {
			return group;
		}
	}
	for (int32 i = 0; i < group->CountGroups(); i++) {
		BParameterGroup* found = find_master_group(group->GroupAt(i));
		if (found != NULL)
			return found;
	}
	return NULL;
}


void
ParameterForm::_BuildMasterVolume()
{
	BParameterGroup* master = NULL;
	for (int32 i = 0; i < fWeb->CountGroups() && master == NULL; i++)
		master = find_master_group(fWeb->GroupAt(i));
	if (master == NULL)
		return;

	CardSpec spec;
	_MakeRows(master, spec);

	Card* card = new Card();
	for (size_t i = 0; i < spec.rows.size(); i++) {
		RowSpec& row = spec.rows[i];
		if (row.type == ROW_CAPTION)
			continue;
		if (row.type == ROW_SLIDER && is_kind(row.parameter, B_MASTER_GAIN))
			row.label = B_TRANSLATE("Volume");

		BView* label = NULL;
		BView* control = _MakeRowView(row, &label, row.rowLabel);
		card->AddRow(label, control);
	}
	align_rows(fBindings, 0, fBindings.CountItems());
	GroupLayout()->AddView(card);
}


void
ParameterForm::_CollectCards(BParameterGroup* group,
	BObjectList<CardSpec, true>& cards)
{
	if ((group->Flags() & B_HIDDEN_PARAMETER) != 0)
		return;

	CardSpec* card = new CardSpec;
	card->title = clean_name(group->Name());
	_MakeRows(group, *card);
	if (!card->rows.empty() || !card->status.empty())
		cards.AddItem(card);
	else
		delete card;

	for (int32 i = 0; i < group->CountGroups(); i++)
		_CollectCards(group->GroupAt(i), cards);
}


void
ParameterForm::_MakeRows(BParameterGroup* group, CardSpec& card)
{
	std::vector<BParameter*> parameters;
	for (int32 i = 0; i < group->CountParameters(); i++) {
		BParameter* parameter = group->ParameterAt(i);
		if (parameter != NULL && !is_hidden(parameter))
			parameters.push_back(parameter);
	}

	size_t count = parameters.size();
	size_t first = 0;
	if (count > 0
		&& parameters[0]->Type() == BParameter::B_NULL_PARAMETER) {
		// the first label of a group names it
		BString title = clean_name(parameters[0]->Name());
		if (!title.IsEmpty())
			card.title = title;
		first = 1;
	}

	std::vector<int32> trailingOf(count, -1);
	std::vector<trailing_type> trailingType(count, TRAILING_NONE);
	std::vector<bool> used(count, false);

	// a lone mute belongs to the lone gain
	int32 gain = -1;
	int32 mute = -1;
	int32 gains = 0;
	int32 mutes = 0;
	for (size_t i = first; i < count; i++) {
		if (is_gain(parameters[i])) {
			gain = i;
			gains++;
		} else if (parameters[i]->Type() == BParameter::B_DISCRETE_PARAMETER
			&& is_kind(parameters[i], B_MUTE)) {
			mute = i;
			mutes++;
		}
	}
	if (gains == 1 && mutes == 1) {
		trailingOf[gain] = mute;
		trailingType[gain] = TRAILING_MUTE;
		used[mute] = true;
	}

	// "Automatic exposure" switches "Exposure time"
	for (size_t i = first; i < count; i++) {
		if (used[i] || parameters[i]->Type()
				!= BParameter::B_DISCRETE_PARAMETER
			|| !is_kind(parameters[i], B_ENABLE)) {
			continue;
		}
		BString target = automatic_target(clean_name(parameters[i]->Name()));
		if (target.IsEmpty())
			continue;

		for (size_t j = first; j < count; j++) {
			if (j == i || used[j] || trailingOf[j] >= 0
				|| parameters[j]->Type()
					!= BParameter::B_CONTINUOUS_PARAMETER) {
				continue;
			}
			BString name = clean_name(parameters[j]->Name());
			if (strncasecmp(name.String(), target.String(), target.Length())
					== 0
				|| (name.Length() > 0 && strncasecmp(target.String(),
					name.String(), name.Length()) == 0)) {
				trailingOf[j] = i;
				trailingType[j] = TRAILING_AUTO;
				used[i] = true;
				break;
			}
		}
	}

	// an unnamed switch in front of a slider turns it on
	for (size_t i = first; i + 1 < count; i++) {
		if (used[i] || !is_toggle(parameters[i])
			|| !clean_name(parameters[i]->Name()).IsEmpty()
			|| parameters[i + 1]->Type() != BParameter::B_CONTINUOUS_PARAMETER
			|| trailingOf[i + 1] >= 0) {
			continue;
		}
		trailingOf[i + 1] = i;
		trailingType[i + 1] = TRAILING_ENABLE;
		used[i] = true;
	}

	for (size_t i = first; i < count; i++) {
		if (used[i])
			continue;

		BParameter* parameter = parameters[i];
		RowSpec row;
		row.parameter = parameter;
		row.label = clean_name(parameter->Name());

		switch (parameter->Type()) {
			case BParameter::B_NULL_PARAMETER:
				if (is_kind(parameter, B_WEB_BUFFER_OUTPUT)
					|| is_kind(parameter, B_WEB_BUFFER_INPUT)
					|| row.label.IsEmpty()) {
					continue;
				}
				row.type = ROW_CAPTION;
				row.text = row.label;
				break;

			case BParameter::B_DISCRETE_PARAMETER:
			{
				BDiscreteParameter* discrete
					= static_cast<BDiscreteParameter*>(parameter);
				if (is_toggle(parameter))
					row.type = ROW_CHECK;
				else if (discrete->CountItems() == 0) {
					// a menu without choices
					if (BString(parameter->Name()).EndsWith(":"))
						continue;
					row.type = ROW_CHECK;
				} else if (discrete->CountItems() == 1) {
					row.type = ROW_VALUE;
					row.text = clean_name(discrete->ItemNameAt(0));
				} else
					row.type = ROW_CHOICE;
				break;
			}

			case BParameter::B_CONTINUOUS_PARAMETER:
			{
				BContinuousParameter* continuous
					= static_cast<BContinuousParameter*>(parameter);
				float range = continuous->MaxValue() - continuous->MinValue();
				if (range <= 0)
					continue;
				if (is_kind(parameter, B_BALANCE))
					row.type = ROW_BALANCE;
				else if (continuous->ValueStep() >= 1
					&& range <= continuous->ValueStep()) {
					// 0..1 in steps of 1 is a switch
					row.type = ROW_CHECK;
				} else
					row.type = ROW_SLIDER;
				break;
			}

			case BParameter::B_TEXT_PARAMETER:
				row.type = row.label.IsEmpty() ? ROW_STATUS : ROW_TEXT;
				break;

			default:
				continue;
		}

		if (row.label.IsEmpty()) {
			row.genericLabel = true;
			switch (row.type) {
				case ROW_SLIDER:
					row.label = is_gain(parameter) ? B_TRANSLATE("Level")
						: B_TRANSLATE("Value");
					break;
				case ROW_BALANCE:
					row.label = B_TRANSLATE("Balance");
					break;
				case ROW_CHOICE:
					row.label = is_kind(parameter, B_INPUT_MUX)
						|| is_kind(parameter, B_OUTPUT_MUX)
						? B_TRANSLATE("Source") : B_TRANSLATE("Value");
					break;
				case ROW_CHECK:
					row.label = B_TRANSLATE("Enabled");
					break;
				default:
					break;
			}
		}

		if (trailingOf[i] >= 0) {
			row.trailing = parameters[trailingOf[i]];
			row.trailingType = trailingType[i];
		}

		if (row.type == ROW_STATUS)
			card.status.push_back(row);
		else
			card.rows.push_back(row);
	}
}


BView*
ParameterForm::_MakeRowView(RowSpec& row, BView** _label,
	const BString& rowLabel)
{
	Binding* binding = new Binding;
	binding->parameter = row.parameter;
	binding->type = row.type;
	int32 index = fBindings.CountItems();
	fBindings.AddItem(binding);

	*_label = NULL;
	BString label = rowLabel.IsEmpty() ? row.label : rowLabel;
	BView* view = NULL;

	switch (row.type) {
		case ROW_SLIDER:
		case ROW_BALANCE:
		{
			BContinuousParameter* continuous
				= static_cast<BContinuousParameter*>(row.parameter);
			binding->minimum = continuous->MinValue();
			binding->maximum = continuous->MaxValue();
			float range = binding->maximum - binding->minimum;
			binding->step = continuous->ValueStep();
			if (binding->step <= 0 || range / binding->step > 20000)
				binding->step = range / 1000;
			binding->steps = std::max((int32)1,
				(int32)roundf(range / binding->step));

			BSlider* slider = new BSlider("slider", NULL,
				binding_message(kMsgSliderDone, index), 0, binding->steps,
				B_HORIZONTAL);
			slider->SetModificationMessage(
				binding_message(kMsgSliderMoved, index));
			slider->SetKeyIncrementValue(std::max((int32)1,
				binding->steps / 50));
			rgb_color fill = ui_color(B_CONTROL_HIGHLIGHT_COLOR);
			slider->UseFillColor(row.type == ROW_SLIDER, &fill);
			if (row.type == ROW_BALANCE) {
				slider->SetHashMarks(B_HASH_MARKS_BOTTOM);
				slider->SetHashMarkCount(3);
			}
			slider->SetExplicitMinSize(BSize(
				ceilf(be_plain_font->Size() * 7), B_SIZE_UNSET));
			binding->slider = slider;

			BStringView* value = new BStringView("value", "");
			float width = std::max(
				value->StringWidth(format_value(*binding, binding->minimum)),
				value->StringWidth(format_value(*binding, binding->maximum)));
			if (row.type == ROW_BALANCE) {
				width = std::max(width, value->StringWidth(
					format_value(*binding, binding->minimum + range * 0.01f)));
			}
			value->SetAlignment(B_ALIGN_RIGHT);
			value->SetExplicitSize(BSize(ceilf(width) + 4, B_SIZE_UNSET));
			binding->valueView = value;

			BGroupView* group = new BGroupView(B_HORIZONTAL,
				B_USE_SMALL_SPACING);
			binding->row = group;
			slider->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
			group->GroupLayout()->AddView(slider, 1.0f);
			group->GroupLayout()->AddView(value, 0.0f);

			if (row.trailing != NULL) {
				const char* text = B_TRANSLATE("On");
				if (row.trailingType == TRAILING_MUTE)
					text = B_TRANSLATE("Mute");
				else if (row.trailingType == TRAILING_AUTO)
					text = B_TRANSLATE("Auto");
				binding->trailing = row.trailing;
				binding->trailingType = row.trailingType;
				binding->trailingCheck = new BCheckBox("trailing", text,
					binding_message(kMsgTrailing, index));
				group->GroupLayout()->AddItem(
					BSpaceLayoutItem::CreateHorizontalStrut(
						be_control_look->DefaultItemSpacing() / 2));
				group->GroupLayout()->AddView(binding->trailingCheck, 0.0f);
			}

			*_label = new BStringView("label", label);
			view = group;
			break;
		}

		case ROW_CHECK:
			binding->check = new BCheckBox("check", row.label,
				binding_message(kMsgCheck, index));
			if (row.parameter->Type() == BParameter::B_CONTINUOUS_PARAMETER) {
				BContinuousParameter* continuous
					= static_cast<BContinuousParameter*>(row.parameter);
				binding->minimum = continuous->MinValue();
				binding->maximum = continuous->MaxValue();
			}
			if (!rowLabel.IsEmpty() && rowLabel != row.label)
				*_label = new BStringView("label", rowLabel);
			view = binding->check;
			if (*_label != NULL && is_kind(row.parameter, B_MUTE)) {
				BGroupView* group = new BGroupView(B_HORIZONTAL, 0);
				group->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
				group->GroupLayout()->AddView(binding->check, 0.0f);
				binding->inTrailingColumn = true;
				view = group;
			}
			break;

		case ROW_CHOICE:
		{
			BDiscreteParameter* discrete
				= static_cast<BDiscreteParameter*>(row.parameter);
			BPopUpMenu* menu = new BPopUpMenu("choice");
			for (int32 i = 0; i < discrete->CountItems(); i++) {
				BMessage* message = binding_message(kMsgChoice, index);
				message->AddInt32("value", discrete->ItemValueAt(i));
				BString name = clean_name(discrete->ItemNameAt(i));
				if (name.IsEmpty())
					name << "#" << i + 1;
				menu->AddItem(new BMenuItem(name, message));
			}
			binding->menuField = new BMenuField("choice", NULL, menu);
			BGroupView* group = new BGroupView(B_HORIZONTAL, 0);
			group->GroupLayout()->AddView(binding->menuField, 0.0f);
			group->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
			*_label = new BStringView("label", label);
			view = group;
			break;
		}

		case ROW_VALUE:
		{
			BStringView* value = new BStringView("value", row.text);
			value->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
			*_label = new BStringView("label", label);
			view = value;
			break;
		}

		case ROW_TEXT:
			binding->textControl = new BTextControl("text", NULL, "",
				binding_message(kMsgText, index));
			*_label = new BStringView("label", label);
			view = binding->textControl;
			break;

		case ROW_STATUS:
			binding->textView = make_caption("");
			view = binding->textView;
			break;

		case ROW_CAPTION:
			view = make_caption(row.text);
			break;
	}

	if (fIcons != NULL && *_label != NULL) {
		BStringView* labelView = dynamic_cast<BStringView*>(*_label);
		IconMap::const_iterator found = labelView != NULL
			? fIcons->find(labelView->Text()) : fIcons->end();
		if (found != fIcons->end()) {
			BView* iconLabel = new IconLabel(found->second, labelView->Text());
			delete labelView;
			*_label = iconLabel;
		}
	}

	if (!row.toolTip.IsEmpty()) {
		if (*_label != NULL)
			(*_label)->SetToolTip(row.toolTip);
		else
			view->SetToolTip(row.toolTip);
	}

	refresh_binding(binding);
	return view;
}


ParameterForm::Binding*
ParameterForm::_BindingFor(int32 parameterID)
{
	for (int32 i = 0; i < fBindings.CountItems(); i++) {
		Binding* binding = fBindings.ItemAt(i);
		if ((binding->parameter != NULL
				&& binding->parameter->ID() == parameterID)
			|| (binding->trailing != NULL
				&& binding->trailing->ID() == parameterID)) {
			return binding;
		}
	}
	return NULL;
}


void
ParameterForm::_UpdatePolling()
{
	bool poll = false;
	for (int32 i = 0; i < fBindings.CountItems(); i++) {
		Binding* binding = fBindings.ItemAt(i);
		if (binding->trailingType == TRAILING_AUTO
			&& binding->trailingCheck->Value() != 0) {
			poll = true;
			break;
		}
	}

	if (poll && Window() != NULL) {
		if (fPollRunner == NULL) {
			BMessage message(kMsgPoll);
			fPollRunner = new BMessageRunner(BMessenger(this), &message,
				1000000);
		}
	} else {
		delete fPollRunner;
		fPollRunner = NULL;
	}
}
