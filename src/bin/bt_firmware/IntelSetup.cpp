/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Intel controllers, following Linux btintel.c (6.18):
 *   - ROM parts (7260, 7265, 3160, 3165, 3168): optional .bseq patch
 *   - RSA bootloader parts (8260, 8265, 9260, 9560, AX200, AX201)
 *   - TLV bootloader parts (AX210, AX211, AX411, BE200 and later), with
 *     RSA or ECDSA headers and, from Blazar on, an intermediate loader
 */


#include "BluetoothFirmware.h"

#include <string.h>
#include <unistd.h>

#include "IntelBluetoothFirmware.h"


using namespace IntelBluetooth;


static bool
ApplyDdc(Controller& controller, const char* name)
{
	std::vector<uint8_t> data;
	if (!LoadFirmware("intel", name, data, false)) {
		// The controller works without it (Linux ignores a missing file).
		LOG("  no device configuration %s, continuing without it\n", name);
		return true;
	}
	size_t offset = 0, length = 0, records = 0;
	while (NextDdcRecord(data.data(), data.size(), offset, length)) {
		if (!controller.Command(kOpcodeWriteDdc, data.data() + offset, length,
				NULL, NULL, kInitTimeout)) {
			return false;
		}
		offset += length;
		records++;
	}
	if (offset != data.size()) {
		ERROR("%s: malformed record at %zu\n", name, offset);
		return false;
	}
	LOG("  applied %zu device configuration records\n", records);
	return true;
}


/*!	What Linux does when a download fails: an Intel Reset back into the
	bootloader. A bootloader that took part of an image (a download that was
	interrupted, say) refuses the next one until it has been reset; this way
	the controller leaves the bus and comes back ready for another try.
*/
static void
ResetToBootloader(Controller& controller)
{
	// hard reset, patches enabled, DDC reload, current image, address 0
	const uint8_t reset[8] = { 0x01, 0x01, 0x01, 0x00, 0, 0, 0, 0 };
	controller.ClearVendorState();
	if (controller.Post(kOpcodeIntelReset, reset, sizeof(reset))) {
		gControllerReset = true;
		LOG("  reset the controller back into its bootloader\n");
	}
	// it holds the USB reset lines for up to 150 ms
	usleep(150000);
}


static bool
DownloadAndBoot(Controller& controller, const char* name, bool tlv,
	uint8_t hwVariant, uint8_t secureBootEngine)
{
	std::vector<uint8_t> firmware;
	if (!LoadFirmware("intel", name, firmware, true))
		return false;
	SfiLayout layout;
	const char* error;
	if (!InspectSfi(firmware.data(), firmware.size(), tlv, hwVariant,
			secureBootEngine, layout, &error)) {
		ERROR("%s: %s\n", name, error);
		return false;
	}
	LOG("  %s header, build %u-%u.%u, boot address 0x%08x, %zu fragments\n",
		layout.ecdsa ? "ECDSA" : "RSA", layout.buildNumber, layout.buildWeek,
		2000 + layout.buildYear, (unsigned)layout.bootAddress,
		layout.fragments);

	controller.bootloader = true;
	controller.ClearVendorState();
	const uint8_t* data = firmware.data();
	bool ok;
	if (layout.ecdsa) {
		const size_t base = kRsaHeaderLength;
		ok = controller.SecureSend(0x00, data + base, 128)
			&& controller.SecureSend(0x03, data + base + 128, 96)
			&& controller.SecureSend(0x02, data + base + 224, 96);
	} else {
		ok = controller.SecureSend(0x00, data, 128)
			&& controller.SecureSend(0x03, data + 128, 256)
			&& controller.SecureSend(0x02, data + 388, 256);
	}
	if (!ok) {
		ERROR("%s: header rejected\n", name);
		ResetToBootloader(controller);
		return false;
	}

	size_t offset = layout.payloadOffset, length = 0, fragments = 0;
	while (NextSfiFragment(data, firmware.size(), offset, length)) {
		if (!controller.SecureSend(0x01, data + offset - length, length)) {
			ERROR("%s: fragment %zu rejected\n", name, fragments);
			ResetToBootloader(controller);
			return false;
		}
		fragments++;
	}

	uint8_t result;
	if (!controller.WaitDownloadResult(kDownloadTimeout, result)) {
		ERROR("%s: no download completion after %zu fragments\n", name,
			fragments);
		ResetToBootloader(controller);
		return false;
	}
	if (result != 0) {
		ERROR("%s: controller rejected the firmware (result %u)\n", name,
			result);
		ResetToBootloader(controller);
		return false;
	}
	LOG("  sent %zu fragments, firmware accepted\n", fragments);

	const uint8_t reset[8] = { 0x00, 0x01, 0x00, 0x01,
		(uint8_t)layout.bootAddress, (uint8_t)(layout.bootAddress >> 8),
		(uint8_t)(layout.bootAddress >> 16),
		(uint8_t)(layout.bootAddress >> 24) };
	controller.ClearVendorState();
	// Intel Reset is never acknowledged; btusb injects the completion.
	if (!controller.Post(kOpcodeIntelReset, reset, sizeof(reset)))
		return false;
	if (!controller.WaitBootup(kBootTimeout)) {
		ERROR("%s: controller did not report booting\n", name);
		return false;
	}
	controller.bootloader = false;
	LOG("  controller booted the new image\n");
	return true;
}


static bool
ReadVersionReply(Controller& controller, bool tlvRequest, uint8_t* reply,
	size_t& length)
{
	const uint8_t parameter = 0xff;
	length = 255;
	return controller.Command(kOpcodeReadVersion, &parameter,
		tlvRequest ? 1 : 0, reply, &length);
}


static bool
ReadLegacyVersion(Controller& controller, LegacyVersion& version)
{
	uint8_t reply[255];
	size_t length;
	return ReadVersionReply(controller, false, reply, length)
		&& ParseLegacyVersion(reply, length, version);
}


static bool
ReadTlvVersion(Controller& controller, TlvVersion& version)
{
	uint8_t reply[255];
	size_t length;
	return ReadVersionReply(controller, true, reply, length)
		&& ParseTlvVersion(reply, length, version);
}


static void
PrintLegacyVersion(const LegacyVersion& version)
{
	LOG("  variant 0x%02x %s, revision %u, firmware variant 0x%02x "
		"revision %u build %u-%u.%u patch %u\n", version.hwVariant,
		VariantName(version.hwVariant), version.hwRevision,
		version.fwVariant, version.fwRevision, version.fwBuildNumber,
		version.fwBuildWeek, 2000 + version.fwBuildYear,
		version.fwPatchNumber);
}


static void
PrintTlvVersion(const TlvVersion& version)
{
	const uint8_t variant = HardwareVariant(version.cnviBt);
	const char* image = version.imageType == kImageBootloader ? "bootloader"
		: version.imageType == kImageIntermediate ? "intermediate loader"
		: version.imageType == kImageOperational ? "operational" : "unknown";
	LOG("  variant 0x%02x %s, CNVi 0x%08x CNVr 0x%08x, image %s, "
		"secure boot engine %s%s%s\n", variant, VariantName(variant),
		(unsigned)version.cnviTop, (unsigned)version.cnvrTop, image,
		version.secureBootEngine == 1 ? "ECDSA" : "RSA",
		version.firmwareId[0] != '\0' ? ", firmware id " : "",
		version.firmwareId);
}


static bool
SetupRom(Controller& controller, const LegacyVersion& version, bool infoOnly)
{
	char specific[64], fallback[64];
	RomPatchNames(version, specific, sizeof(specific), fallback,
		sizeof(fallback));
	std::string path;
	const char* name = FindFirmware("intel", specific, path) ? specific
		: FindFirmware("intel", fallback, path) ? fallback : NULL;
	LOG("  ROM firmware; patch %s or %s: %s\n", specific, fallback,
		name != NULL ? name : "none installed");
	if (infoOnly)
		return true;
	if (version.fwPatchNumber != 0) {
		LOG("  already patched (patch %u)\n", version.fwPatchNumber);
		return true;
	}
	// These parts are fully functional from ROM; the patch is optional.
	std::vector<uint8_t> patch;
	if (name == NULL || !LoadFirmware("intel", name, patch, false))
		return true;

	const uint8_t enter[2] = { 0x01, 0x00 };
	if (!controller.Command(kOpcodeManufacturer, enter, sizeof(enter)))
		return false;

	bool activate = false;
	bool failed = false;
	size_t offset = 0, steps = 0;
	PatchStep step;
	while (offset < patch.size()) {
		if (!NextPatchStep(patch.data(), patch.size(), offset, step)) {
			ERROR("%s: malformed at %zu\n", name, offset);
			failed = true;
			break;
		}
		if (step.opcode == kOpcodePatch)
			activate = true;
		std::vector<uint8_t> event;
		if (!controller.CommandExpecting(step.opcode, step.parameters,
				step.parameterLength, step.eventCode, event, kInitTimeout)
			|| event.size() != (size_t)step.eventLength + 2
			|| memcmp(event.data() + 2, step.eventParameters,
				step.eventLength) != 0) {
			ERROR("%s: step %zu (opcode %04x) got an unexpected answer\n",
				name, steps, step.opcode);
			failed = true;
			break;
		}
		steps++;
	}

	// Exit manufacturer mode: 0 = just leave, 1 = reset without patches,
	// 2 = reset with the patches active.
	const uint8_t exit[2] = { 0x00,
		(uint8_t)(failed ? 0x01 : activate ? 0x02 : 0x00) };
	if (!controller.Command(kOpcodeManufacturer, exit, sizeof(exit),
			NULL, NULL, kInitTimeout)) {
		return false;
	}
	if (failed)
		return false;

	LegacyVersion patched;
	if (activate && ReadLegacyVersion(controller, patched))
		LOG("  applied %zu patch commands, patch %u active\n", steps,
			patched.fwPatchNumber);
	else
		LOG("  applied %zu patch commands\n", steps);
	return true;
}


static bool
SetupLegacyBootloader(Controller& controller, const LegacyVersion& version,
	bool infoOnly)
{
	char name[64];
	if (version.fwVariant == kLegacyFirmwareOperational) {
		LOG("  operational firmware already running\n");
		if (infoOnly && LegacyFirmwareName(version, 0, "sfi", name,
				sizeof(name)) && version.hwVariant >= 0x11) {
			std::string path;
			LOG("  firmware %s: %s\n", name,
				FindFirmware("intel", name, path)
					? path.c_str() : "not installed");
		}
		return true;
	}
	if (version.fwVariant != kLegacyFirmwareBootloader) {
		ERROR("%s: unknown firmware variant 0x%02x\n", controller.Path(),
			version.fwVariant);
		return false;
	}

	uint8_t reply[255];
	size_t length = sizeof(reply);
	BootParams params;
	if (!controller.Command(kOpcodeReadBootParams, NULL, 0, reply, &length)
		|| !ParseBootParams(reply, length, params)) {
		ERROR("%s: cannot read boot parameters\n", controller.Path());
		return false;
	}
	LOG("  bootloader: device revision %u, secure boot %s, minimum build "
		"%u-%u.%u\n", params.deviceRevision,
		params.secureBoot ? "on" : "off", params.minimumBuildNumber,
		params.minimumBuildWeek, 2000 + params.minimumBuildYear);
	if (params.limitedCommandComplete != 0) {
		ERROR("%s: unsupported firmware loading method %u\n",
			controller.Path(), params.limitedCommandComplete);
		return false;
	}
	if (!LegacyFirmwareName(version, params.deviceRevision, "sfi", name,
			sizeof(name))) {
		ERROR("%s: no firmware naming for this variant\n", controller.Path());
		return false;
	}
	std::string path;
	LOG("  firmware %s: %s\n", name,
		FindFirmware("intel", name, path) ? path.c_str() : "not installed");
	if (infoOnly)
		return true;

	if (!DownloadAndBoot(controller, name, false, version.hwVariant, 0))
		return false;

	LegacyFirmwareName(version, params.deviceRevision, "ddc", name,
		sizeof(name));
	if (!ApplyDdc(controller, name))
		return false;

	LegacyVersion running;
	if (!ReadLegacyVersion(controller, running)
		|| running.fwVariant != kLegacyFirmwareOperational) {
		ERROR("%s: controller is not operational after booting\n",
			controller.Path());
		return false;
	}
	PrintLegacyVersion(running);
	return true;
}


static bool
SetupTlv(Controller& controller, TlvVersion version, bool infoOnly)
{
	const uint8_t variant = HardwareVariant(version.cnviBt);
	char name[64];
	if (infoOnly || version.imageType == kImageOperational) {
		if (version.imageType == kImageOperational)
			LOG("  operational firmware already running\n");
		TlvFirmwareName(version, "sfi", name, sizeof(name));
		std::string path;
		LOG("  firmware %s: %s\n", name,
			FindFirmware("intel", name, path) ? path.c_str() : "not installed");
		return true;
	}

	// Blazar and later boot an intermediate loader first, which then
	// reports the name of the operational image to load.
	for (int stage = 0; stage < 2
			&& version.imageType != kImageOperational; stage++) {
		if (version.imageType != kImageBootloader
			&& version.imageType != kImageIntermediate) {
			ERROR("%s: unexpected image type %u\n", controller.Path(),
				version.imageType);
			return false;
		}
		TlvFirmwareName(version, "sfi", name, sizeof(name));
		LOG("  loading %s\n", name);
		if (!DownloadAndBoot(controller, name, true, variant,
				version.secureBootEngine)
			|| !ReadTlvVersion(controller, version)) {
			return false;
		}
		PrintTlvVersion(version);
	}
	if (version.imageType != kImageOperational) {
		ERROR("%s: controller is not operational after booting\n",
			controller.Path());
		return false;
	}

	TlvFirmwareName(version, "ddc", name, sizeof(name));
	return ApplyDdc(controller, name);
}


bool
SetupIntel(Controller& controller, const UsbDevice* device, bool infoOnly)
{
	if (device != NULL && device->needsInitialReset) {
		if (!controller.Command(kOpcodeReset, NULL, 0, NULL, NULL,
				kInitTimeout)) {
			return false;
		}
	}

	// Since TyP the command takes a parameter and answers in TLV form;
	// older parts accept the parameter and still answer in the old form.
	uint8_t reply[255];
	size_t length;
	if (!ReadVersionReply(controller, true, reply, length)) {
		ERROR("%s: Intel Read Version failed\n", controller.Path());
		return false;
	}

	if (IsLegacyVersionReply(reply, length)) {
		LegacyVersion version;
		ParseLegacyVersion(reply, length, version);
		PrintLegacyVersion(version);
		switch (LegacyGeneration(version.hwVariant)) {
			case kGenerationLegacyRom:
				return SetupRom(controller, version, infoOnly);
			case kGenerationLegacyBootloader:
				return SetupLegacyBootloader(controller, version, infoOnly);
			default:
				ERROR("%s: unsupported hardware variant 0x%02x\n",
					controller.Path(), version.hwVariant);
				return false;
		}
	}

	TlvVersion version;
	if (!ParseTlvVersion(reply, length, version)) {
		ERROR("%s: malformed version information\n", controller.Path());
		return false;
	}
	if (HardwarePlatform(version.cnviBt) != 0x37) {
		ERROR("%s: unsupported hardware platform 0x%02x\n", controller.Path(),
			HardwarePlatform(version.cnviBt));
		return false;
	}
	const uint8_t variant = HardwareVariant(version.cnviBt);
	switch (TlvGeneration(variant)) {
		case kGenerationLegacyBootloader:
		{
			// The operational firmware of these answers in TLV form, but
			// their file names derive from the legacy version.
			LegacyVersion legacy;
			if (!ReadLegacyVersion(controller, legacy)) {
				ERROR("%s: legacy Read Version failed\n", controller.Path());
				return false;
			}
			PrintLegacyVersion(legacy);
			return SetupLegacyBootloader(controller, legacy, infoOnly);
		}
		case kGenerationTlv:
			PrintTlvVersion(version);
			return SetupTlv(controller, version, infoOnly);
		default:
			ERROR("%s: unsupported hardware variant 0x%02x\n",
				controller.Path(), variant);
			return false;
	}
}


