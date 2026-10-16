/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Pages.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <AutoDeleter.h>
#include <Button.h>
#include <Catalog.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MediaRoster.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <NodeInfo.h>
#include <NodeMonitor.h>
#include <ParameterWeb.h>
#include <Path.h>
#include <PathFinder.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <SoundPlayer.h>
#include <SpaceLayoutItem.h>
#include <StringList.h>
#include <StringView.h>

#include <MidiSettings.h>

#include <map>

#include "Cards.h"
#include "LiveViews.h"
#include "MediaMessages.h"
#include "ParameterForm.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Media pages"


static const uint32 kMsgDeskbarVolume = 'pgDV';
static const uint32 kMsgTestSound = 'pgTS';
static const uint32 kMsgTestSoundDone = 'pgTD';
static const uint32 kMsgSelectSoundFont = 'pgSF';
static const uint32 kMsgOpenSoundFonts = 'pgOF';
static const uint32 kMsgTogglePreview = 'pgTP';


static float
ui_scale()
{
	return std::max(1.0f, be_plain_font->Size() / 12.0f);
}


static BMessage*
set_default_message(uint32 role, const dormant_node_info& info)
{
	BMessage* message = new BMessage(kMsgSetDefault);
	message->AddInt32("role", role);
	message->AddInt32("addon", info.addon);
	message->AddInt32("flavor", info.flavor_id);
	return message;
}


static BString
trimmed(const char* name)
{
	BString text(name);
	text.Trim();
	return text;
}


// #pragma mark - TestSound


/*!	A short two-note chime played through the system mixer, so it comes out
	of the current sound output.
*/
class TestSound {
public:
	static void Play(BMessenger target)
	{
		Stop();

		media_raw_audio_format format = media_raw_audio_format::wildcard;
		format.format = media_raw_audio_format::B_AUDIO_FLOAT;
		format.channel_count = 2;
		format.frame_rate = 48000;
		format.byte_order = B_MEDIA_HOST_ENDIAN;

		sFrame = 0;
		sPlayer = new BSoundPlayer(&format, "Media test sound", &_Fill);
		if (sPlayer->InitCheck() != B_OK) {
			delete sPlayer;
			sPlayer = NULL;
			return;
		}
		sPlayer->Start();
		sPlayer->SetHasData(true);

		BMessage done(kMsgTestSoundDone);
		BMessageRunner::StartSending(target, &done, 1200000, 1);
	}

	static void Stop()
	{
		if (sPlayer == NULL)
			return;
		sPlayer->Stop();
		delete sPlayer;
		sPlayer = NULL;
	}

private:
	static float _Sample(int64 frame, float rate)
	{
		static const float kNotes[2] = { 880.0f, 1318.5f };
		static const float kStart[2] = { 0.0f, 0.16f };
		float time = frame / rate;
		float sample = 0;
		for (int i = 0; i < 2; i++) {
			float local = time - kStart[i];
			if (local < 0 || local > 0.8f)
				continue;
			float attack = std::min(1.0f, local / 0.005f);
			float envelope = attack * expf(-local * 6.0f);
			sample += 0.22f * envelope * sinf(2 * M_PI * kNotes[i] * local);
		}
		return sample;
	}

	static void _Fill(void* cookie, void* buffer, size_t size,
		const media_raw_audio_format& format)
	{
		int32 channels = std::max((uint32)1, format.channel_count);
		float rate = format.frame_rate > 0 ? format.frame_rate : 48000;

		if (format.format == media_raw_audio_format::B_AUDIO_FLOAT) {
			float* samples = (float*)buffer;
			size_t frames = size / sizeof(float) / channels;
			for (size_t i = 0; i < frames; i++) {
				float sample = _Sample(sFrame++, rate);
				for (int32 c = 0; c < channels; c++)
					*samples++ = sample;
			}
		} else if (format.format == media_raw_audio_format::B_AUDIO_SHORT) {
			int16* samples = (int16*)buffer;
			size_t frames = size / sizeof(int16) / channels;
			for (size_t i = 0; i < frames; i++) {
				int16 sample = (int16)(_Sample(sFrame++, rate) * 32767);
				for (int32 c = 0; c < channels; c++)
					*samples++ = sample;
			}
		} else
			memset(buffer, 0, size);
	}

	static BSoundPlayer*	sPlayer;
	static int64			sFrame;
};


BSoundPlayer* TestSound::sPlayer = NULL;
int64 TestSound::sFrame = 0;


// #pragma mark - Page


Page::Page(const char* key, glyph_kind glyph, rgb_color color,
	const char* title, const char* subtitle, const MediaModel* model)
	:
	BView(key, 0),
	fKey(key),
	fModel(model),
	fBuilt(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	float scale = ui_scale();
	fBadge = new BadgeView(glyph, color, ceilf(36 * scale));

	BFont titleFont(be_bold_font);
	titleFont.SetSize(ceilf(titleFont.Size() * 1.4f));
	fTitle = new TitleView(title, titleFont, false);
	fSubtitle = new TitleView(subtitle, *be_plain_font, true);
	if (subtitle == NULL || subtitle[0] == '\0')
		fSubtitle->Hide();

	fContent = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
	fContent->GroupLayout()->SetInsets(B_USE_WINDOW_SPACING,
		B_USE_DEFAULT_SPACING, B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING);
	BScrollView* scroller = make_page_scroller(fContent, &fViewport);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_SPACING, B_USE_WINDOW_SPACING,
				B_USE_WINDOW_SPACING, B_USE_DEFAULT_SPACING)
			.Add(fBadge)
			.AddGroup(B_VERTICAL, 0)
				.AddGlue()
				.Add(fTitle)
				.Add(fSubtitle)
				.AddGlue()
			.End()
		.End()
		.Add(new BSeparatorView(B_HORIZONTAL, B_PLAIN_BORDER))
		.Add(scroller);
}


void
Page::AttachedToWindow()
{
	BView::AttachedToWindow();
}


void
Page::ModelChanged()
{
	Rebuild();
}


void
Page::Rebuild()
{
	float position = fViewport->ScrollPosition();
	_Clear();
	Build();
	fContent->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
	fBuilt = true;
	fViewport->SetScrollPosition(position);
}


float
Page::ScrollPosition() const
{
	return fViewport->ScrollPosition();
}


void
Page::SetScrollPosition(float position)
{
	fViewport->SetScrollPosition(position);
}


void
Page::_Clear()
{
	BGroupLayout* layout = fContent->GroupLayout();
	while (BView* child = fContent->ChildAt(0)) {
		child->RemoveSelf();
		delete child;
	}
	while (layout->CountItems() > 0)
		delete layout->RemoveItem((int32)0);
}


void
Page::SetTitle(const char* title)
{
	fTitle->SetText(title);
}


void
Page::SetSubtitle(const char* subtitle)
{
	fSubtitle->SetText(subtitle);
}


void
Page::AddTitle(const char* title)
{
	BGroupLayout* layout = fContent->GroupLayout();
	if (layout->CountItems() > 0) {
		layout->AddItem(BSpaceLayoutItem::CreateVerticalStrut(
			ceilf(be_control_look->DefaultItemSpacing() * 0.5f)));
	}
	layout->AddView(make_section_title(title));
}


Card*
Page::AddCard()
{
	Card* card = new Card();
	fContent->GroupLayout()->AddView(card);
	return card;
}


void
Page::AddView(BView* view)
{
	fContent->GroupLayout()->AddView(view);
}


void
Page::AddLink(const char* label, const char* key)
{
	BMessage* message = new BMessage(kMsgShowPage);
	message->AddString("key", key);
	LinkButton* link = new LinkButton(label, message);

	BGroupView* row = new BGroupView(B_HORIZONTAL, 0);
	row->GroupLayout()->AddView(link);
	row->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
	fContent->GroupLayout()->AddView(row);
}


void
Page::AddDeviceChoices(Card* card, uint32 role)
{
	std::vector<const NodeEntry*> nodes;
	fModel->NodesFor(role, nodes);
	if (nodes.empty()) {
		card->AddFullRow(make_caption(B_TRANSLATE("No devices found.")));
		return;
	}

	for (size_t i = 0; i < nodes.size(); i++) {
		const dormant_node_info& info = nodes[i]->info;
		const DeviceEntry* device = fModel->DeviceFor(info);
		BRadioButton* radio = new BRadioButton("choice",
			device != NULL ? device->name : trimmed(info.name),
			set_default_message(role, info));
		radio->SetValue(fModel->IsDefault(info, role)
			? B_CONTROL_ON : B_CONTROL_OFF);
		card->AddFullRow(radio);
	}
}


// #pragma mark - OutputPage


OutputPage::OutputPage(const MediaModel* model)
	:
	Page("output", GLYPH_SPEAKER, glyph_color(GLYPH_SPEAKER),
		B_TRANSLATE("Sound output"),
		B_TRANSLATE("Choose where sound is played, and how loud."), model),
	fDeskbarCheck(NULL)
{
}


void
OutputPage::AttachedToWindow()
{
	Page::AttachedToWindow();
}


void
OutputPage::Build()
{
	AddTitle(B_TRANSLATE("Play sound through"));
	AddDeviceChoices(AddCard(), ROLE_AUDIO_OUTPUT);

	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	media_node mixer;
	if (roster != NULL && roster->GetAudioMixer(&mixer) == B_OK) {
		BParameterWeb* web = NULL;
		if (roster->GetParameterWebFor(mixer, &web) == B_OK && web != NULL) {
			ParameterForm* form = new ParameterForm(web, NULL,
				kFormMasterVolumeOnly);
			if (form->HasControls()) {
				AddTitle(B_TRANSLATE("Volume"));
				AddView(form);
			} else
				delete form;
		}
		roster->ReleaseNode(mixer);
	}

	AddTitle(B_TRANSLATE("Options"));
	Card* options = AddCard();

	BMenuField* channels = _MakeChannelMenu();
	if (channels != NULL)
		options->AddRow(B_TRANSLATE("Output channel"), channels);

	fDeskbarCheck = new BCheckBox("deskbar",
		B_TRANSLATE("Show volume control in Deskbar"),
		new BMessage(kMsgDeskbarVolume));
	fDeskbarCheck->SetValue(BDeskbar().HasItem("MediaReplicant"));
	fDeskbarCheck->SetTarget(this);
	options->AddFullRow(fDeskbarCheck);

	BButton* test = new BButton("test sound", B_TRANSLATE("Play test sound"),
		new BMessage(kMsgTestSound));
	test->SetTarget(this);
	test->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_VERTICAL_CENTER));
	options->AddFullRow(test);

	const NodeEntry* output = Model()->DefaultFor(ROLE_AUDIO_OUTPUT);
	const DeviceEntry* device = output != NULL
		? Model()->DeviceFor(output->info) : NULL;
	if (device != NULL) {
		BString label(B_TRANSLATE("Settings for %device% ›"));
		label.ReplaceFirst("%device%", device->name);
		AddLink(label, device->Key());
	}
}


BMenuField*
OutputPage::_MakeChannelMenu()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster == NULL)
		return NULL;

	media_node node;
	int32 currentInput = -1;
	if (roster->GetAudioOutput(&node, &currentInput, NULL) != B_OK)
		return NULL;

	int32 count = 0;
	media_input inputs[16];
	status_t status = roster->GetAllInputsFor(node, inputs, 16, &count);
	roster->ReleaseNode(node);
	if (status != B_OK || count < 2)
		return NULL;

	BPopUpMenu* menu = new BPopUpMenu("channel");
	for (int32 i = 0; i < std::min(count, (int32)16); i++) {
		BMessage* message = new BMessage(kMsgSetOutputChannel);
		message->AddData("input", B_RAW_TYPE, &inputs[i], sizeof(media_input));
		BMenuItem* item = new BMenuItem(inputs[i].name, message);
		item->SetMarked(inputs[i].destination.id == currentInput);
		menu->AddItem(item);
	}

	BMenuField* field = new BMenuField("channel", NULL, menu);
	field->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_VERTICAL_CENTER));
	return field;
}


void
OutputPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgDeskbarVolume:
		{
			BDeskbar deskbar;
			if (fDeskbarCheck->Value() == B_CONTROL_ON) {
				BEntry entry("/bin/desklink", true);
				entry_ref ref;
				int32 id;
				if (entry.GetRef(&ref) == B_OK)
					deskbar.AddItem(&ref, &id);
			} else
				deskbar.RemoveItem("MediaReplicant");
			break;
		}

		case kMsgTestSound:
			TestSound::Play(BMessenger(this));
			break;

		case kMsgTestSoundDone:
			TestSound::Stop();
			break;

		default:
			Page::MessageReceived(message);
			break;
	}
}


// #pragma mark - InputPage


InputPage::InputPage(const MediaModel* model)
	:
	Page("input", GLYPH_MICROPHONE, glyph_color(GLYPH_MICROPHONE),
		B_TRANSLATE("Sound input"),
		B_TRANSLATE("Choose the microphone or input applications record "
			"from."), model)
{
}


void
InputPage::Build()
{
	AddTitle(B_TRANSLATE("Record sound from"));
	AddDeviceChoices(AddCard(), ROLE_AUDIO_INPUT);

	if (Model()->DefaultFor(ROLE_AUDIO_INPUT) != NULL) {
		AddTitle(B_TRANSLATE("Input level"));
		Card* card = AddCard();
		card->AddFullRow(new LevelMeter());
		card->AddFullRow(make_caption(B_TRANSLATE("Speak or play something: "
			"the bar shows what the input hears.")));
	}

	const NodeEntry* input = Model()->DefaultFor(ROLE_AUDIO_INPUT);
	const DeviceEntry* device = input != NULL
		? Model()->DeviceFor(input->info) : NULL;
	if (device != NULL) {
		BString label(B_TRANSLATE("Settings for %device% ›"));
		label.ReplaceFirst("%device%", device->name);
		AddLink(label, device->Key());
	}
}


// #pragma mark - CameraPage


CameraPage::CameraPage(const MediaModel* model)
	:
	Page("camera", GLYPH_CAMERA, glyph_color(GLYPH_CAMERA),
		B_TRANSLATE("Camera"),
		B_TRANSLATE("Choose the camera applications use for video."), model)
{
}


void
CameraPage::Build()
{
	AddTitle(B_TRANSLATE("Use camera"));
	AddDeviceChoices(AddCard(), ROLE_VIDEO_INPUT);

	std::vector<const NodeEntry*> outputs;
	Model()->NodesFor(ROLE_VIDEO_OUTPUT, outputs);
	if (!outputs.empty()) {
		AddTitle(B_TRANSLATE("Video output"));
		AddDeviceChoices(AddCard(), ROLE_VIDEO_OUTPUT);
	}

	const NodeEntry* camera = Model()->DefaultFor(ROLE_VIDEO_INPUT);
	const DeviceEntry* device = camera != NULL
		? Model()->DeviceFor(camera->info) : NULL;
	if (device != NULL) {
		BString label(B_TRANSLATE("Preview and settings for %device% ›"));
		label.ReplaceFirst("%device%", device->name);
		AddLink(label, device->Key());
	}
}


// #pragma mark - MixerPage


MixerPage::MixerPage(const MediaModel* model)
	:
	Page("mixer", GLYPH_MIXER, glyph_color(GLYPH_MIXER),
		B_TRANSLATE("Audio mixer"),
		B_TRANSLATE("The volume of each application, and how the mixer "
			"works."), model),
	fHasMixer(false)
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster != NULL && roster->GetAudioMixer(&fMixer) == B_OK)
		fHasMixer = true;
}


MixerPage::~MixerPage()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (fHasMixer && roster != NULL)
		roster->ReleaseNode(fMixer);
}


void
MixerPage::Build()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	BParameterWeb* web = NULL;
	if (!fHasMixer || roster == NULL
		|| roster->GetParameterWebFor(fMixer, &web) != B_OK || web == NULL) {
		Card* card = AddCard();
		card->AddFullRow(make_caption(
			B_TRANSLATE("The audio mixer is not running.")));
		return;
	}

	// The applications that play show their icons on their rows: the port
	// of a mixer input's producer belongs to the application's team.
	std::map<BString, BBitmap*> icons;
	ParameterForm::IconMap iconMap;
	float iconSize = ceilf(18 * ui_scale());
	media_input inputs[64];
	int32 count = 0;
	if (roster->GetConnectedInputsFor(fMixer, inputs, 64, &count) == B_OK) {
		for (int32 i = 0; i < count; i++) {
			port_info portInfo;
			app_info appInfo;
			if (get_port_info(inputs[i].source.port, &portInfo) != B_OK
				|| be_roster->GetRunningAppInfo(portInfo.team, &appInfo)
					!= B_OK) {
				continue;
			}
			BBitmap* icon = new BBitmap(BRect(0, 0, iconSize - 1,
				iconSize - 1), B_RGBA32);
			if (BNodeInfo::GetTrackerIcon(&appInfo.ref, icon,
					(icon_size)iconSize) != B_OK) {
				delete icon;
				continue;
			}
			BString name(inputs[i].name);
			name.Trim();
			delete icons[name];
			icons[name] = icon;
			iconMap[name] = icon;
		}
	}

	// the master output, and streams of no application, get a badge
	BBitmap* master = make_glyph_bitmap(GLYPH_SPEAKER,
		glyph_color(GLYPH_SPEAKER), iconSize);
	BBitmap* stream = make_glyph_bitmap(GLYPH_SPEAKER, device_glyph_color(),
		iconSize);
	for (int32 i = 0; i < count; i++) {
		BString name(inputs[i].name);
		name.Trim();
		if (iconMap.find(name) == iconMap.end() && stream != NULL)
			iconMap[name] = stream;
	}
	for (int32 i = 0; i < web->CountParameters() && master != NULL; i++) {
		BParameter* parameter = web->ParameterAt(i);
		if (parameter->Type() != BParameter::B_CONTINUOUS_PARAMETER
			|| strcmp(parameter->Kind(), B_MASTER_GAIN) != 0) {
			continue;
		}
		BParameterGroup* group = parameter->Group();
		if (group->CountParameters() > 0
			&& group->ParameterAt(0)->Type() == BParameter::B_NULL_PARAMETER) {
			BString name(group->ParameterAt(0)->Name());
			name.Trim();
			iconMap[name] = master;
		}
		break;
	}

	AddView(new ParameterForm(web, B_TRANSLATE("Audio mixer"), 0,
		&iconMap));
	delete master;
	delete stream;

	for (std::map<BString, BBitmap*>::iterator iterator = icons.begin();
			iterator != icons.end(); iterator++) {
		delete iterator->second;
	}
}


void
MixerPage::ModelChanged()
{
	// the mixer has its own notifications
}


void
MixerPage::AttachedToWindow()
{
	Page::AttachedToWindow();
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (fHasMixer && roster != NULL)
		roster->StartWatching(BMessenger(this), fMixer, B_MEDIA_WEB_CHANGED);
}


void
MixerPage::DetachedFromWindow()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (fHasMixer && roster != NULL)
		roster->StopWatching(BMessenger(this), fMixer, B_MEDIA_WEB_CHANGED);
	Page::DetachedFromWindow();
}


void
MixerPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_MEDIA_WEB_CHANGED:
			Rebuild();
			break;

		default:
			Page::MessageReceived(message);
			break;
	}
}


// #pragma mark - MidiPage


MidiPage::MidiPage(const MediaModel* model)
	:
	Page("midi", GLYPH_MIDI, glyph_color(GLYPH_MIDI), B_TRANSLATE("MIDI"),
		B_TRANSLATE("Choose the SoundFont that plays MIDI music."), model)
{
}


void
MidiPage::_FindSoundFonts(std::vector<BString>& fonts)
{
	BStringList paths;
	if (BPathFinder::FindPaths(B_FIND_PATH_DATA_DIRECTORY, "synth", paths)
			!= B_OK) {
		return;
	}

	for (int32 i = 0; i < paths.CountStrings(); i++) {
		BDirectory directory(paths.StringAt(i).String());
		if (directory.InitCheck() != B_OK)
			continue;

		BEntry entry;
		while (directory.GetNextEntry(&entry) == B_OK) {
			if (entry.IsDirectory())
				continue;
			BPath path;
			if (entry.GetPath(&path) == B_OK)
				fonts.push_back(path.Path());
		}
	}
}


BString
MidiPage::_ActiveSoundFont()
{
	struct BPrivate::midi_settings settings;
	if (BPrivate::read_midi_settings(&settings) == B_OK)
		return settings.soundfont_file;
	return BString();
}


void
MidiPage::_SetActiveSoundFont(const char* path)
{
	struct BPrivate::midi_settings settings;
	strlcpy(settings.soundfont_file, path, sizeof(settings.soundfont_file));
	BPrivate::write_midi_settings(settings);
}


void
MidiPage::Build()
{
	std::vector<BString> fonts;
	_FindSoundFonts(fonts);
	BString active = _ActiveSoundFont();

	if (fonts.size() == 1 && active != fonts[0]) {
		// the only one is the one to use
		_SetActiveSoundFont(fonts[0]);
		active = fonts[0];
	}

	AddTitle(B_TRANSLATE("SoundFont"));
	Card* card = AddCard();
	if (fonts.empty()) {
		card->AddFullRow(new WrappingText("none", B_TRANSLATE("There are no "
			"SoundFonts installed, so MIDI music cannot be played. SoundFonts "
			"can be installed with HaikuDepot, or copied into the SoundFonts "
			"folder.")));
	} else {
		bool selected = false;
		for (size_t i = 0; i < fonts.size(); i++) {
			BPath path(fonts[i]);
			BMessage* message = new BMessage(kMsgSelectSoundFont);
			message->AddString("path", fonts[i]);
			BRadioButton* radio = new BRadioButton("font", path.Leaf(),
				message);
			radio->SetToolTip(fonts[i]);
			radio->SetTarget(this);
			if (fonts[i] == active) {
				radio->SetValue(B_CONTROL_ON);
				selected = true;
			}
			card->AddFullRow(radio);
		}
		if (!selected) {
			card->AddFullRow(make_caption(
				B_TRANSLATE("Please choose a SoundFont.")));
		}
	}

	BButton* folder = new BButton("folder",
		B_TRANSLATE("Open SoundFonts folder"),
		new BMessage(kMsgOpenSoundFonts));
	folder->SetTarget(this);
	BGroupView* row = new BGroupView(B_HORIZONTAL, 0);
	row->GroupLayout()->AddView(folder);
	row->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
	AddView(row);
}


void
MidiPage::ModelChanged()
{
	// SoundFonts are not media nodes
}


void
MidiPage::AttachedToWindow()
{
	Page::AttachedToWindow();
	_Watch();
}


void
MidiPage::DetachedFromWindow()
{
	stop_watching(this);
	Page::DetachedFromWindow();
}


void
MidiPage::_Watch()
{
	BStringList paths;
	BPathFinder().FindPaths(B_FIND_PATH_DATA_DIRECTORY, "synth", paths);
	for (int32 i = 0; i < paths.CountStrings(); i++) {
		BEntry entry(paths.StringAt(i));
		node_ref nodeRef;
		if (entry.GetNodeRef(&nodeRef) == B_OK)
			watch_node(&nodeRef, B_WATCH_DIRECTORY, this);
	}
}


void
MidiPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSelectSoundFont:
		{
			const char* path;
			if (message->FindString("path", &path) == B_OK) {
				_SetActiveSoundFont(path);
				Rebuild();
			}
			break;
		}

		case kMsgOpenSoundFonts:
		{
			BPath path;
			if (find_directory(B_USER_NONPACKAGED_DATA_DIRECTORY, &path, true)
					!= B_OK) {
				break;
			}
			path.Append("synth");
			create_directory(path.Path(), 0755);

			entry_ref ref;
			if (get_ref_for_path(path.Path(), &ref) != B_OK)
				break;
			BMessenger tracker("application/x-vnd.Be-TRAK");
			BMessage refs(B_REFS_RECEIVED);
			refs.AddRef("refs", &ref);
			tracker.SendMessage(&refs);
			break;
		}

		case B_NODE_MONITOR:
			Rebuild();
			break;

		default:
			Page::MessageReceived(message);
			break;
	}
}


// #pragma mark - DevicePage


DevicePage::DevicePage(const DeviceEntry& device, const MediaModel* model)
	:
	Page(device.Key(), device.Glyph(), glyph_color(device.Glyph()),
		device.name, device.Description(), model),
	fName(device.name),
	fUseCard(NULL),
	fUseHolder(NULL),
	fWatching(false)
{
	// the camera first, then the sound
	std::vector<NodeEntry> nodes(device.nodes);
	std::stable_sort(nodes.begin(), nodes.end(),
		[](const NodeEntry& a, const NodeEntry& b) {
			return (a.roles & ROLE_VIDEO_INPUT) > (b.roles & ROLE_VIDEO_INPUT);
		});

	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	for (size_t i = 0; i < nodes.size(); i++) {
		Part part;
		part.entry = nodes[i];
		part.acquired = false;
		part.holder = NULL;
		part.preview = NULL;

		if (roster != NULL) {
			const dormant_node_info& info = nodes[i].info;
			media_node_id id;
			int32 count = 1;
			status_t status;
			if (roster->GetInstancesFor(info.addon, info.flavor_id, &id,
					&count) == B_OK && count > 0) {
				status = roster->GetNodeFor(id, &part.node);
			} else {
				status = roster->InstantiateDormantNode(info, &part.node,
					B_FLAVOR_IS_GLOBAL);
			}
			part.acquired = status == B_OK;
		}
		fParts.push_back(part);
	}
}


DevicePage::~DevicePage()
{
	_ReleaseNodes();
}


void
DevicePage::_ReleaseNodes()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	for (size_t i = 0; i < fParts.size(); i++) {
		if (fParts[i].acquired && roster != NULL)
			roster->ReleaseNode(fParts[i].node);
		fParts[i].acquired = false;
	}
}


void
DevicePage::Build()
{
	AddTitle(B_TRANSLATE("Used for"));
	fUseHolder = new BGroupView(B_VERTICAL, 0);
	AddView(fUseHolder);
	_FillUseCard();

	float spacing = be_control_look->DefaultItemSpacing();
	BFont headingFont(be_bold_font);
	headingFont.SetSize(ceilf(headingFont.Size() * 1.15f));

	for (size_t i = 0; i < fParts.size(); i++) {
		Part& part = fParts[i];

		if (fParts.size() > 1) {
			const char* heading = B_TRANSLATE("Sound");
			if ((part.entry.roles & ROLE_VIDEO_INPUT) != 0)
				heading = B_TRANSLATE("Camera");
			else if ((part.entry.roles & ROLE_VIDEO_OUTPUT) != 0)
				heading = B_TRANSLATE("Video");
			else if ((part.entry.roles & ROLE_AUDIO_OUTPUT) == 0)
				heading = B_TRANSLATE("Microphone");
			Content()->GroupLayout()->AddItem(
				BSpaceLayoutItem::CreateVerticalStrut(spacing));
			BStringView* title = make_section_title(heading);
			title->SetFont(&headingFont);
			AddView(title);
		}

		if (!part.acquired) {
			Card* card = AddCard();
			card->AddFullRow(make_caption(
				B_TRANSLATE("This device cannot be used right now.")));
			continue;
		}

		if ((part.entry.roles & ROLE_VIDEO_INPUT) != 0) {
			if (fParts.size() == 1)
				AddTitle(B_TRANSLATE("Preview"));
			Card* card = AddCard();
			part.preview = new CameraPreview(part.node);
			card->AddFullRow(part.preview);

			BMessage* message = new BMessage(kMsgTogglePreview);
			message->AddInt32("part", i);
			BButton* toggle = new BButton("toggle",
				B_TRANSLATE("Pause preview"), message);
			toggle->SetTarget(this);
			toggle->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
				B_ALIGN_VERTICAL_CENTER));
			card->AddFullRow(toggle);
		}

		part.holder = new BGroupView(B_VERTICAL, B_USE_DEFAULT_SPACING);
		AddView(part.holder);
		_MakeForm(part);
	}
}


void
DevicePage::_MakeForm(Part& part)
{
	while (BView* child = part.holder->ChildAt(0)) {
		child->RemoveSelf();
		delete child;
	}

	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	BParameterWeb* web = NULL;
	ParameterForm* form = NULL;
	if (roster != NULL && roster->GetParameterWebFor(part.node, &web) == B_OK
		&& web != NULL) {
		form = new ParameterForm(web, fName);
		if (form->IsEmpty()) {
			delete form;
			form = NULL;
		}
	}

	if (form != NULL) {
		part.holder->GroupLayout()->AddView(form);
		return;
	}

	Card* card = new Card();
	card->AddFullRow(make_caption(
		B_TRANSLATE("This device has no settings.")));
	part.holder->GroupLayout()->AddView(card);
}


void
DevicePage::_FillUseCard()
{
	if (fUseHolder == NULL)
		return;

	while (BView* child = fUseHolder->ChildAt(0)) {
		child->RemoveSelf();
		delete child;
	}

	const DeviceEntry* device = Model()->DeviceFor(Key());
	fUseCard = new Card();
	if (device == NULL) {
		fUseCard->AddFullRow(make_caption(
			B_TRANSLATE("This device is no longer connected.")));
		fUseHolder->GroupLayout()->AddView(fUseCard);
		return;
	}

	static const uint32 kRoles[] = { ROLE_AUDIO_OUTPUT, ROLE_AUDIO_INPUT,
		ROLE_VIDEO_INPUT, ROLE_VIDEO_OUTPUT };
	for (size_t i = 0; i < B_COUNT_OF(kRoles); i++) {
		uint32 role = kRoles[i];
		const NodeEntry* node = device->NodeFor(role);
		if (node == NULL)
			continue;

		BView* control;
		if (Model()->IsDefault(node->info, role)) {
			control = new BStringView("in use", B_TRANSLATE("✓ In use"));
		} else {
			BGroupView* group = new BGroupView(B_HORIZONTAL,
				B_USE_DEFAULT_SPACING);
			BButton* button = new BButton("use",
				B_TRANSLATE("Use this device"),
				set_default_message(role, node->info));
			group->GroupLayout()->AddView(button);
			BString current = Model()->DefaultName(role);
			if (!current.IsEmpty()) {
				BString text(B_TRANSLATE("Now: %device%"));
				text.ReplaceFirst("%device%", current);
				group->GroupLayout()->AddView(make_caption(text));
			}
			group->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue());
			control = group;
		}
		fUseCard->AddRow(MediaModel::RoleName(role), control);
	}
	fUseHolder->GroupLayout()->AddView(fUseCard);
}


void
DevicePage::ModelChanged()
{
	// The controls and the preview stay; only what the device is used for
	// changes.
	_FillUseCard();
}


void
DevicePage::AttachedToWindow()
{
	Page::AttachedToWindow();

	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster == NULL)
		return;
	for (size_t i = 0; i < fParts.size(); i++) {
		if (fParts[i].acquired) {
			roster->StartWatching(BMessenger(this), fParts[i].node,
				B_MEDIA_WEB_CHANGED);
		}
	}
	fWatching = true;
}


void
DevicePage::DetachedFromWindow()
{
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (fWatching && roster != NULL) {
		for (size_t i = 0; i < fParts.size(); i++) {
			if (fParts[i].acquired) {
				roster->StopWatching(BMessenger(this), fParts[i].node,
					B_MEDIA_WEB_CHANGED);
			}
		}
	}
	fWatching = false;
	Page::DetachedFromWindow();
}


void
DevicePage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_MEDIA_WEB_CHANGED:
		{
			const media_node* node = NULL;
			ssize_t size;
			if (message->FindData("node", B_RAW_TYPE, (const void**)&node,
					&size) != B_OK) {
				node = NULL;
			}
			for (size_t i = 0; i < fParts.size(); i++) {
				if (fParts[i].holder != NULL && fParts[i].acquired
					&& (node == NULL || node->node == fParts[i].node.node)) {
					_MakeForm(fParts[i]);
				}
			}
			break;
		}

		case kMsgTogglePreview:
		{
			int32 index;
			BButton* button = NULL;
			if (message->FindInt32("part", &index) != B_OK
				|| index < 0 || index >= (int32)fParts.size()
				|| fParts[index].preview == NULL) {
				break;
			}
			message->FindPointer("source", (void**)&button);
			CameraPreview* preview = fParts[index].preview;
			if (preview->IsRunning()) {
				preview->Stop();
				if (button != NULL)
					button->SetLabel(B_TRANSLATE("Show preview"));
			} else {
				preview->Start();
				if (button != NULL)
					button->SetLabel(B_TRANSLATE("Pause preview"));
			}
			break;
		}

		default:
			Page::MessageReceived(message);
			break;
	}
}


// #pragma mark - MessagePage


MessagePage::MessagePage(const char* key, glyph_kind glyph, const char* title,
	const char* text, const char* buttonLabel, BMessage* buttonMessage,
	const MediaModel* model)
	:
	Page(key, glyph, glyph_color(glyph), title, "", model),
	fText(text),
	fButtonLabel(buttonLabel != NULL ? buttonLabel : ""),
	fButtonMessage(buttonMessage),
	fTextView(NULL)
{
}


MessagePage::~MessagePage()
{
	delete fButtonMessage;
}


void
MessagePage::Build()
{
	Card* card = AddCard();
	fTextView = new WrappingText("text", fText);
	card->AddFullRow(fTextView);

	if (fButtonMessage != NULL) {
		BButton* button = new BButton("action", fButtonLabel,
			new BMessage(*fButtonMessage));
		button->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
			B_ALIGN_VERTICAL_CENTER));
		card->AddFullRow(button);
	}
}


void
MessagePage::SetText(const char* text)
{
	fText = text;
	if (fTextView != NULL)
		fTextView->SetText(text);
}
