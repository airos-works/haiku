/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "HardwareWindow.h"

#include <Button.h>
#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <ControlLook.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <OS.h>
#include <SeparatorView.h>
#include <StringView.h>

#include <stdio.h>

#include "HardwareCaps.h"
#include "LavcVideoEncoder.h"
#include "Messages.h"
#include "NvencEncoder.h"
#include "SoftwareEncoders.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Hardware"


static const uint32 kMsgRefresh = 'rfsh';


HardwareWindow::HardwareWindow(BRect frame)
	:
	BWindow(frame, B_TRANSLATE("Video hardware"), B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
			| B_CLOSE_ON_ESCAPE)
{
	fMatrix = new BColumnListView("matrix", 0, B_FANCY_BORDER, false);
	fMatrix->SetSortingEnabled(false);
	float spacing = be_control_look->DefaultLabelSpacing();
	fMatrix->SetExplicitMinSize(BSize(spacing * 110, spacing * 34));

	BView* devices = new BView("devices", 0);
	fDevices = new BGroupLayout(B_VERTICAL, B_USE_SMALL_SPACING);
	devices->SetLayout(fDevices);

	BStringView* intro = new BStringView("intro",
		B_TRANSLATE("Which formats the graphics cards can encode, and what "
			"the CPU does instead."));

	BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
		.SetInsets(B_USE_WINDOW_INSETS)
		.Add(intro)
		.Add(fMatrix, 1)
		.Add(devices)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(new BButton("refresh", B_TRANSLATE("Look again"),
				new BMessage(kMsgRefresh)))
		.End();

	_Fill();
	CenterOnScreen();
}


void
HardwareWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgRefresh:
			HardwareInventory::Default().Probe();
			_Fill();
			break;
		case kMsgHardwareTested:
			_Fill();
			break;
		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
HardwareWindow::_Fill()
{
	HardwareInventory& inventory = HardwareInventory::Default();
	if (!inventory.IsProbed())
		inventory.Probe();

	fMatrix->Clear();
	while (fMatrix->CountColumns() > 0) {
		BColumn* column = fMatrix->ColumnAt(0);
		fMatrix->RemoveColumn(column);
		delete column;
	}
	float spacing = be_control_look->DefaultLabelSpacing();
	fMatrix->AddColumn(new BStringColumn(B_TRANSLATE("Format"), spacing * 18,
		spacing * 10, spacing * 40, B_TRUNCATE_END), 0);
	int32 columns = 1;
	std::vector<int32> devices;
	for (int32 i = 0; i < inventory.CountDevices(); i++) {
		const GpuDevice& device = inventory.DeviceAt(i);
		fMatrix->AddColumn(new BStringColumn(device.name.String(),
			spacing * 30, spacing * 10, spacing * 80, B_TRUNCATE_END),
			columns++);
		devices.push_back(i);
	}
	fMatrix->AddColumn(new BStringColumn(B_TRANSLATE("CPU"), spacing * 26,
		spacing * 10, spacing * 60, B_TRUNCATE_END), columns);

	for (int c = 0; c < kVideoCopy; c++) {
		video_codec codec = (video_codec)c;
		BRow* row = new BRow();
		row->SetField(new BStringField(video_codec_info(codec).name), 0);
		for (size_t d = 0; d < devices.size(); d++) {
			const GpuDevice& device = inventory.DeviceAt(devices[d]);
			const GpuEncodeFormat* format = device.Format(codec);
			BString text;
			if (format == NULL)
				text = "–";
			else {
				// NVIDIA formats count once a test picture has come back
				bool works = false;
				bool tested = device.backend == kBackendNVENC
					&& NvencEncoder::Tested(codec, &works);
				if (device.CanEncode(codec, 16, 16)) {
					text = device.backend == kBackendNVENC && !tested
						? B_TRANSLATE("✓ (not tested yet) ") : "✓ ";
				} else if (!device.ready)
					text = B_TRANSLATE("(no driver) ");
				else if (tested && !works)
					text = B_TRANSLATE("(failed the test) ");
				else
					text = B_TRANSLATE("(not used yet) ");
				if (format->maxWidth > 0) {
					BString size;
					size.SetToFormat(B_TRANSLATE("up to %d×%d"),
						format->maxWidth, format->maxHeight);
					text << size;
				}
				if (format->bFrames)
					text << B_TRANSLATE(", B pictures");
			}
			row->SetField(new BStringField(text.String()), d + 1);
		}
		BString cpu;
		BString missing;
		const VideoCodecInfo& info = video_codec_info(codec);
		if (SoftwareEncoders::IsAvailable(codec, &missing))
			cpu << "✓ " << SoftwareEncoders::Version(info.software);
		else if (LavcVideoEncoder::IsAvailable(codec))
			cpu << "✓ FFmpeg (" << info.lavcEncoder << ")";
		else if (!missing.IsEmpty())
			cpu.SetToFormat(B_TRANSLATE("needs the %s package"),
				missing.String());
		else
			cpu = "–";
		row->SetField(new BStringField(cpu.String()), columns);
		fMatrix->AddRow(row);
	}

	// one line per card about its state
	while (fDevices->CountItems() > 0) {
		BLayoutItem* item = fDevices->ItemAt(0);
		BView* view = item->View();
		fDevices->RemoveItem(item);
		delete item;
		delete view;
	}
	if (inventory.CountDevices() == 0) {
		fDevices->AddView(new BStringView("none",
			B_TRANSLATE("No graphics card with a video engine was found.")));
	}
	for (int32 i = 0; i < inventory.CountDevices(); i++) {
		const GpuDevice& device = inventory.DeviceAt(i);
		BString line;
		line.SetToFormat("%s", device.name.String());
		if (!device.generation.IsEmpty())
			line << " · " << device.generation;
		if (!device.engine.IsEmpty())
			line << " · " << device.engine;
		if (device.vendorID != 0) {
			BString id;
			id.SetToFormat(" · %04x:%04x", device.vendorID, device.deviceID);
			line << id;
		}
		line << " · " << (device.ready ? B_TRANSLATE("ready")
			: device.status.String());
		BStringView* view = new BStringView("device", line.String());
		view->SetToolTip(device.devicePath.String());
		fDevices->AddView(view);
		if (!device.decode.empty()) {
			BString decode = B_TRANSLATE("    Decodes: ");
			for (size_t d = 0; d < device.decode.size(); d++) {
				if (d > 0)
					decode << ", ";
				decode << device.decode[d];
			}
			fDevices->AddView(new BStringView("decode", decode.String()));
		}
	}
	system_info info;
	get_system_info(&info);
	char cpuName[128] = "";
	BString cpu;
	cpu.SetToFormat(B_TRANSLATE("CPU: %" B_PRIu32 " threads for the "
		"software encoders"), info.cpu_count);
	(void)cpuName;
	fDevices->AddView(new BStringView("cpu", cpu.String()));
}
