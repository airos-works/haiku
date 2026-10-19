/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Operating system commands (OSC): titles, colors, links, clipboard,
//!	notifications, pointer shapes and shell integration.


#include "Terminal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "Base64.h"
#include "ColorNames.h"
#include "UnicodeTables.h"


static const size_t kMaxColorStackSize = 10;
static const size_t kMaxPointerStackSize = 16;
static const size_t kMaxHyperLinks = 65536;
static const size_t kMaxNotificationSize = 64 * 1024;
static const size_t kMaxClipboardSize = 64 * 1024 * 1024;

static const char* const kPointerShapes[] = {
	"alias", "cell", "copy", "crosshair", "default", "e-resize", "ew-resize",
	"grab", "grabbing", "help", "move", "n-resize", "ne-resize",
	"nesw-resize", "no-drop", "not-allowed", "ns-resize", "nw-resize",
	"nwse-resize", "pointer", "progress", "s-resize", "se-resize",
	"sw-resize", "text", "vertical-text", "w-resize", "wait", "zoom-in",
	"zoom-out", NULL
};

static const struct {
	const char*	alias;
	const char*	name;
} kPointerAliases[] = {
	{ "left_ptr", "default" }, { "arrow", "default" },
	{ "xterm", "text" }, { "ibeam", "text" }, { "beam", "text" },
	{ "pointing_hand", "pointer" }, { "hand2", "pointer" },
	{ "hand", "pointer" },
	{ "question_arrow", "help" }, { "whats_this", "help" },
	{ "clock", "wait" }, { "watch", "wait" },
	{ "half-busy", "progress" }, { "left_ptr_watch", "progress" },
	{ "tcross", "crosshair" },
	{ "plus", "cell" }, { "cross", "cell" },
	{ "fleur", "move" }, { "pointer-move", "move" },
	{ "right_side", "e-resize" }, { "top_right_corner", "ne-resize" },
	{ "top_left_corner", "nw-resize" }, { "top_side", "n-resize" },
	{ "bottom_right_corner", "se-resize" },
	{ "bottom_left_corner", "sw-resize" }, { "bottom_side", "s-resize" },
	{ "left_side", "w-resize" },
	{ "sb_h_double_arrow", "ew-resize" }, { "split_h", "ew-resize" },
	{ "sb_v_double_arrow", "ns-resize" }, { "split_v", "ns-resize" },
	{ "size_bdiag", "nesw-resize" }, { "size-bdiag", "nesw-resize" },
	{ "size_fdiag", "nwse-resize" }, { "size-fdiag", "nwse-resize" },
	{ "zoom_in", "zoom-in" }, { "zoom_out", "zoom-out" },
	{ "dnd-link", "alias" }, { "dnd-copy", "copy" },
	{ "forbidden", "not-allowed" }, { "crossed_circle", "not-allowed" },
	{ "dnd-no-drop", "no-drop" },
	{ "openhand", "grab" }, { "hand1", "grab" },
	{ "closedhand", "grabbing" }, { "dnd-none", "grabbing" },
	{ NULL, NULL }
};


static const char*
canonical_pointer_shape(const std::string& name)
{
	for (int32 i = 0; kPointerShapes[i] != NULL; i++) {
		if (name == kPointerShapes[i])
			return kPointerShapes[i];
	}
	for (int32 i = 0; kPointerAliases[i].alias != NULL; i++) {
		if (name == kPointerAliases[i].alias)
			return kPointerAliases[i].name;
	}
	return NULL;
}


static void
split(const std::string& string, char separator,
	std::vector<std::string>& parts)
{
	parts.clear();
	size_t start = 0;
	while (true) {
		size_t end = string.find(separator, start);
		if (end == std::string::npos) {
			parts.push_back(string.substr(start));
			return;
		}
		parts.push_back(string.substr(start, end - start));
		start = end + 1;
	}
}


//! Keeps only the characters allowed in identifiers ([a-zA-Z0-9_-+.]).
static std::string
sanitize_identifier(const std::string& identifier)
{
	std::string result;
	for (size_t i = 0; i < identifier.size() && result.size() < 512; i++) {
		char c = identifier[i];
		if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '+'
			|| c == '.') {
			result += c;
		}
	}
	return result;
}


//! Removes control characters (C0, DEL, C1) from UTF-8 text.
static std::string
sanitize_text(const std::string& text, bool keepNewlines)
{
	std::string result;
	for (size_t i = 0; i < text.size(); i++) {
		unsigned char c = text[i];
		if (c < 0x20 && !(keepNewlines && (c == '\n' || c == '\t')))
			continue;
		if (c == 0x7f)
			continue;
		if (c == 0xc2 && i + 1 < text.size()
			&& (unsigned char)text[i + 1] >= 0x80
			&& (unsigned char)text[i + 1] <= 0x9f) {
			i++;	// a C1 control in UTF-8
			continue;
		}
		result += c;
	}
	return result;
}


// #pragma mark - dispatch


void
Terminal::OscDispatch(const char* data, size_t length)
{
	std::string string(data, length);

	size_t separator = string.find(';');
	std::string number = string.substr(0, separator);
	std::string argument = separator == std::string::npos
		? std::string() : string.substr(separator + 1);

	if (number.empty())
		return;
	for (size_t i = 0; i < number.size(); i++) {
		if (!isdigit((unsigned char)number[i]))
			return;
	}
	int32 command = atoi(number.c_str());

	switch (command) {
		case 0:
			_SetTitle(argument.c_str(), true, true);
			break;
		case 1:
			_SetTitle(argument.c_str(), false, true);
			break;
		case 2:
			_SetTitle(argument.c_str(), true, false);
			break;
		case 4:
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 15:
		case 16:
		case 17:
		case 18:
		case 19:
			_SetColors(command, argument.c_str());
			break;
		case 104:
		case 110:
		case 111:
		case 112:
		case 113:
		case 114:
		case 115:
		case 116:
		case 117:
		case 118:
		case 119:
			_ResetColors(command, argument.c_str());
			break;
		case 7:
		{
			// the working directory: file://host/path
			std::string url = argument;
			fWorkingDirectory = url;
			if (fListener != NULL)
				fListener->TerminalWorkingDirectoryChanged(url.c_str());
			break;
		}
		case 8:
		{
			size_t uriStart = argument.find(';');
			if (uriStart == std::string::npos)
				break;
			std::string parameters = argument.substr(0, uriStart);
			std::string uri = argument.substr(uriStart + 1);
			_HyperLink(parameters.c_str(), uri.c_str());
			break;
		}
		case 9:
		{
			// ConEmu/Windows Terminal progress: 9;4;state;percent
			if (argument.size() >= 2 && argument[0] == '4'
				&& argument[1] == ';') {
				std::vector<std::string> parts;
				split(argument, ';', parts);
				int32 state = parts.size() > 1 ? atoi(parts[1].c_str()) : 0;
				int32 percent = parts.size() > 2 ? atoi(parts[2].c_str())
					: 0;
				if (fListener != NULL)
					fListener->TerminalProgressChanged(state, percent);
				break;
			}
			// other ConEmu subcommands start with a number and ';'
			if (!argument.empty() && isdigit((unsigned char)argument[0])) {
				size_t i = 0;
				while (i < argument.size()
					&& isdigit((unsigned char)argument[i])) {
					i++;
				}
				if (i < argument.size() && argument[i] == ';')
					break;
			}
			// iTerm2: a notification with this text
			TermNotification notification;
			notification.title = sanitize_text(argument, false);
			notification.reportActivation = false;
			notification.reportClose = false;
			notification.urgency = 1;
			notification.onlyWhenUnfocused = false;
			if (!notification.title.empty() && fListener != NULL)
				fListener->TerminalNotify(notification);
			break;
		}
		case 21:
			_KittyColors(argument.c_str());
			break;
		case 22:
			_PointerShape(argument.c_str());
			break;
		case 52:
			_Clipboard(argument.c_str());
			break;
		case 66:
			_TextSizing(argument.c_str());
			break;
		case 99:
		{
			size_t payloadStart = argument.find(';');
			if (payloadStart == std::string::npos)
				break;
			_KittyNotification(argument.substr(0, payloadStart).c_str(),
				argument.substr(payloadStart + 1).c_str());
			break;
		}
		case 133:
			_ShellIntegration(argument.c_str());
			break;
		case 777:
		{
			// rxvt-unicode: 777;notify;title;body
			std::vector<std::string> parts;
			split(argument, ';', parts);
			if (parts.size() >= 2 && parts[0] == "notify") {
				TermNotification notification;
				notification.title = sanitize_text(parts[1], false);
				if (parts.size() >= 3) {
					std::string body = parts[2];
					for (size_t i = 3; i < parts.size(); i++)
						body += ";" + parts[i];
					notification.body = sanitize_text(body, false);
				}
				notification.reportActivation = false;
				notification.reportClose = false;
				notification.urgency = 1;
				notification.onlyWhenUnfocused = false;
				if (fListener != NULL)
					fListener->TerminalNotify(notification);
			}
			break;
		}
		case 1337:
			// iTerm2: only the working directory
			if (argument.compare(0, 11, "CurrentDir=") == 0) {
				fWorkingDirectory = "file://" + argument.substr(11);
				if (fListener != NULL) {
					fListener->TerminalWorkingDirectoryChanged(
						fWorkingDirectory.c_str());
				}
			}
			break;
		case 5522:
			_ClipboardProtocol(argument.c_str());
			break;
		case 30001:
			_PushColors(0);
			break;
		case 30101:
			_PopColors(0);
			break;
	}
}


// #pragma mark - titles


void
Terminal::_SetTitle(const char* title, bool window, bool icon)
{
	std::string text = sanitize_text(title, false);
	if (window) {
		fTitle = text;
		if (fListener != NULL)
			fListener->TerminalTitleChanged(fTitle.c_str());
	}
	if (icon) {
		fIconTitle = text;
		if (fListener != NULL)
			fListener->TerminalIconTitleChanged(fIconTitle.c_str());
	}
}


// #pragma mark - colors


/*!	Parses an X11 color specification (rgb:r/g/b, #rgb, rgbi:r/g/b or a
	color name), as xterm and kitty accept them.
*/
bool
Terminal::_ParseColor(const char* spec, uint32& _color) const
{
	std::string string(spec);
	// an alpha suffix (kitty) is ignored
	size_t at = string.find('@');
	if (at != std::string::npos)
		string.erase(at);
	while (!string.empty() && isspace((unsigned char)string.back()))
		string.erase(string.size() - 1);

	if (string.compare(0, 4, "rgb:") == 0) {
		std::vector<std::string> parts;
		split(string.substr(4), '/', parts);
		if (parts.size() == 1 && parts[0].size() == 6) {
			// kitty's lenient rgb:rrggbb
			_color = strtoul(parts[0].c_str(), NULL, 16) & 0xffffff;
			return true;
		}
		if (parts.size() != 3)
			return false;
		uint32 color = 0;
		for (int i = 0; i < 3; i++) {
			const std::string& part = parts[i];
			if (part.empty() || part.size() > 4)
				return false;
			for (size_t j = 0; j < part.size(); j++) {
				if (!isxdigit((unsigned char)part[j]))
					return false;
			}
			uint32 value = strtoul(part.c_str(), NULL, 16);
			uint32 maximum = (1u << (4 * part.size())) - 1;
			uint32 scaled = (value * 255 + maximum / 2) / maximum;
			color = (color << 8) | scaled;
		}
		_color = color;
		return true;
	}

	if (string.compare(0, 5, "rgbi:") == 0) {
		std::vector<std::string> parts;
		split(string.substr(5), '/', parts);
		if (parts.size() != 3)
			return false;
		uint32 color = 0;
		for (int i = 0; i < 3; i++) {
			double value = strtod(parts[i].c_str(), NULL);
			value = std::max(0.0, std::min(1.0, value));
			color = (color << 8) | (uint32)lround(value * 255);
		}
		_color = color;
		return true;
	}

	if (!string.empty() && string[0] == '#') {
		std::string hex = string.substr(1);
		size_t digits = hex.size() / 3;
		if (hex.size() % 3 != 0 || digits < 1 || digits > 4)
			return false;
		for (size_t j = 0; j < hex.size(); j++) {
			if (!isxdigit((unsigned char)hex[j]))
				return false;
		}
		uint32 color = 0;
		for (int i = 0; i < 3; i++) {
			uint32 value = strtoul(hex.substr(i * digits, digits).c_str(),
				NULL, 16);
			// the most significant bits
			if (digits == 1)
				value <<= 4;
			else if (digits == 3)
				value >>= 4;
			else if (digits == 4)
				value >>= 8;
			color = (color << 8) | (value & 0xff);
		}
		_color = color;
		return true;
	}

	return lookup_color_name(string.c_str(), _color);
}


static int32
dynamic_color_index(int32 command)
{
	switch (command) {
		case 10:
			return COLOR_FOREGROUND;
		case 11:
			return COLOR_BACKGROUND;
		case 12:
			return COLOR_CURSOR;
		case 13:
			return COLOR_POINTER_FOREGROUND;
		case 14:
			return COLOR_POINTER_BACKGROUND;
		case 17:
			return COLOR_SELECTION_BACKGROUND;
		case 19:
			return COLOR_SELECTION_FOREGROUND;
	}
	return -1;
}


void
Terminal::_ColorReply(int32 command, int32 index, uint32 color, bool bell)
{
	uint32 red = (color >> 16) & 0xff;
	uint32 green = (color >> 8) & 0xff;
	uint32 blue = color & 0xff;
	const char* terminator = bell ? "\x07" : "\x1b\\";
	if (command == 4) {
		_Reply("\x1b]4;%d;rgb:%04x/%04x/%04x%s", (int)index,
			(unsigned)(red * 0x101), (unsigned)(green * 0x101),
			(unsigned)(blue * 0x101), terminator);
	} else {
		_Reply("\x1b]%d;rgb:%04x/%04x/%04x%s", (int)command,
			(unsigned)(red * 0x101), (unsigned)(green * 0x101),
			(unsigned)(blue * 0x101), terminator);
	}
}


void
Terminal::_SetColors(int32 command, const char* spec)
{
	bool bell = fParser.StringEndedWithBell();
	std::vector<std::string> parts;
	split(spec, ';', parts);

	bool changed = false;
	if (command == 4) {
		// pairs of index and color
		for (size_t i = 0; i + 1 < parts.size(); i += 2) {
			int32 index = atoi(parts[i].c_str());
			if (index < 0 || index > 255)
				continue;
			if (parts[i + 1] == "?") {
				_ColorReply(4, index, Color(index), bell);
				continue;
			}
			uint32 color;
			if (_ParseColor(parts[i + 1].c_str(), color)) {
				fColors[index] = color;
				changed = true;
			}
		}
	} else {
		// OSC 10 with several colors sets 10, 11, 12, ... in turn
		for (size_t i = 0; i < parts.size(); i++, command++) {
			int32 index = dynamic_color_index(command);
			if (parts[i] == "?") {
				uint32 color = index >= 0 ? Color(index) : kColorUnset;
				if (color == kColorUnset) {
					if (index == COLOR_CURSOR || index < 0)
						color = Color(COLOR_FOREGROUND);
					else if (index == COLOR_SELECTION_BACKGROUND)
						color = Color(COLOR_FOREGROUND);
					else
						color = Color(COLOR_BACKGROUND);
				}
				if (color == kColorUnset)
					color = 0;
				_ColorReply(command, -1, color, bell);
				continue;
			}
			if (index < 0)
				continue;
			uint32 color;
			if (_ParseColor(parts[i].c_str(), color)) {
				fColors[index] = color;
				changed = true;
			}
		}
	}

	if (changed) {
		fColorsGeneration++;
		_TouchRange(0, fRows - 1);
		if (fListener != NULL)
			fListener->TerminalColorsChanged();
	}
}


void
Terminal::_ResetColors(int32 command, const char* spec)
{
	if (command == 104) {
		if (spec[0] == '\0') {
			for (int32 i = 0; i < 256; i++)
				fColors[i] = fDefaultColors[i];
		} else {
			std::vector<std::string> parts;
			split(spec, ';', parts);
			for (size_t i = 0; i < parts.size(); i++) {
				int32 index = atoi(parts[i].c_str());
				if (index >= 0 && index < 256)
					fColors[index] = fDefaultColors[index];
			}
		}
	} else {
		int32 index = dynamic_color_index(command - 100);
		if (index < 0)
			return;
		fColors[index] = fDefaultColors[index];
	}

	fColorsGeneration++;
	_TouchRange(0, fRows - 1);
	if (fListener != NULL)
		fListener->TerminalColorsChanged();
}


/*!	kitty's OSC 21: key=value pairs to set, query (?) and reset (no =)
	colors.
*/
void
Terminal::_KittyColors(const char* spec)
{
	static const struct {
		const char*	name;
		int32		index;
	} kKeys[] = {
		{ "foreground", COLOR_FOREGROUND },
		{ "background", COLOR_BACKGROUND },
		{ "selection_background", COLOR_SELECTION_BACKGROUND },
		{ "selection_foreground", COLOR_SELECTION_FOREGROUND },
		{ "cursor", COLOR_CURSOR },
		{ "cursor_text", COLOR_CURSOR_TEXT },
		{ NULL, -1 }
	};

	bool bell = fParser.StringEndedWithBell();
	std::vector<std::string> fields;
	split(spec, ';', fields);

	std::string reply;
	bool changed = false;
	for (size_t i = 0; i < fields.size(); i++) {
		const std::string& field = fields[i];
		if (field.empty())
			continue;
		size_t equals = field.find('=');
		std::string key = field.substr(0, equals);

		int32 index = -1;
		bool numeric = !key.empty() && key.size() <= 3;
		for (size_t j = 0; j < key.size() && numeric; j++)
			numeric = isdigit((unsigned char)key[j]) != 0;
		if (numeric) {
			index = atoi(key.c_str());
			if (index > 255)
				index = -1;
		} else {
			for (int32 j = 0; kKeys[j].name != NULL; j++) {
				if (key == kKeys[j].name) {
					index = kKeys[j].index;
					break;
				}
			}
		}

		bool known = index >= 0 || key == "visual_bell"
			|| key.compare(0, 28, "transparent_background_color") == 0;
		if (!known || index < 0) {
			if (!known || (equals != std::string::npos
					&& field.compare(equals + 1, std::string::npos, "?") == 0)) {
				if (!known) {
					std::string encoded;
					base64_encode(key.data(), key.size(), encoded, false);
					reply += ";unknown=" + encoded;
				} else
					reply += ";" + key + "=";
			}
			continue;
		}

		if (equals == std::string::npos) {
			// reset
			fColors[index] = fDefaultColors[index];
			changed = true;
			continue;
		}

		std::string value = field.substr(equals + 1);
		if (value == "?") {
			uint32 color = Color(index);
			if (color == kColorUnset)
				reply += ";" + key + "=";
			else {
				char buffer[32];
				snprintf(buffer, sizeof(buffer), "=rgb:%02x/%02x/%02x",
					(unsigned)((color >> 16) & 0xff),
					(unsigned)((color >> 8) & 0xff),
					(unsigned)(color & 0xff));
				reply += ";" + key + buffer;
			}
		} else if (value.empty()) {
			// dynamic, not allowed for the palette
			if (index >= 256) {
				fColors[index] = kColorUnset;
				changed = true;
			}
		} else {
			uint32 color;
			if (_ParseColor(value.c_str(), color)) {
				fColors[index] = color;
				changed = true;
			}
		}
	}

	if (!reply.empty()) {
		fOutput += "\x1b]21";
		fOutput += reply;
		fOutput += bell ? "\x07" : "\x1b\\";
	}

	if (changed) {
		fColorsGeneration++;
		_TouchRange(0, fRows - 1);
		if (fListener != NULL)
			fListener->TerminalColorsChanged();
	}
}


void
Terminal::_PushColors(int32 index)
{
	std::vector<uint32> colors(fColors, fColors + COLOR_COUNT);
	if (index >= 1 && index <= (int32)kMaxColorStackSize) {
		if ((int32)fColorStack.size() < index)
			fColorStack.resize(index);
		fColorStack[index - 1] = colors;
		return;
	}
	if (fColorStack.size() >= kMaxColorStackSize)
		fColorStack.erase(fColorStack.begin());
	fColorStack.push_back(colors);
}


void
Terminal::_PopColors(int32 index)
{
	std::vector<uint32> colors;
	if (index >= 1 && index <= (int32)fColorStack.size()) {
		colors = fColorStack[index - 1];
	} else if (index == 0 && !fColorStack.empty()) {
		colors = fColorStack.back();
		fColorStack.pop_back();
	}
	if (colors.size() != COLOR_COUNT)
		return;

	for (int32 i = 0; i < COLOR_COUNT; i++)
		fColors[i] = colors[i];
	fColorsGeneration++;
	_TouchRange(0, fRows - 1);
	if (fListener != NULL)
		fListener->TerminalColorsChanged();
}


// #pragma mark - hyperlinks


void
Terminal::_HyperLink(const char* parameters, const char* uri)
{
	if (uri[0] == '\0') {
		fHyperLink = 0;
		return;
	}

	std::string identifier;
	std::vector<std::string> pairs;
	split(parameters, ':', pairs);
	for (size_t i = 0; i < pairs.size(); i++) {
		if (pairs[i].compare(0, 3, "id=") == 0)
			identifier = pairs[i].substr(3);
	}

	// links with the same id and address are the same link; links without
	// id are only the same while they are continued
	std::string key = identifier.empty()
		? std::string() : identifier + '\0' + uri;
	if (!key.empty()) {
		std::unordered_map<std::string, uint32>::iterator found
			= fHyperLinkIndex.find(key);
		if (found != fHyperLinkIndex.end()) {
			fHyperLink = found->second;
			return;
		}
	} else if (fHyperLink != 0 && fHyperLink < fHyperLinks.size()
		&& fHyperLinks[fHyperLink].identifier.empty()
		&& fHyperLinks[fHyperLink].uri == uri) {
		return;
	}

	if (fHyperLinks.size() >= kMaxHyperLinks) {
		// start over; old cells keep pointing to reused slots
		fHyperLinks.resize(1);
		fHyperLinkIndex.clear();
	}

	HyperLink link;
	link.uri = uri;
	link.identifier = identifier;
	fHyperLink = fHyperLinks.size();
	fHyperLinks.push_back(link);
	if (!key.empty())
		fHyperLinkIndex[key] = fHyperLink;
}


// #pragma mark - clipboard


void
Terminal::_Clipboard(const char* parameters)
{
	const char* separator = strchr(parameters, ';');
	if (separator == NULL)
		return;

	std::string selections(parameters, separator - parameters);
	if (selections.empty())
		selections = "c";
	const char* data = separator + 1;

	if (strcmp(data, "?") == 0) {
		if (fListener != NULL)
			fListener->TerminalRequestClipboard(selections.c_str());
		return;
	}

	std::string text;
	if (!base64_decode(data, strlen(data), text))
		return;
	if (fListener != NULL) {
		fListener->TerminalSetClipboard(selections.c_str(), text.data(),
			text.size());
	}
}


void
Terminal::ReplyClipboard(const char* selections, const void* data,
	size_t length)
{
	std::string encoded;
	base64_encode(data, length, encoded);
	fOutput += "\x1b]52;";
	fOutput += selections;
	fOutput += ";";
	fOutput += encoded;
	fOutput += "\x1b\\";
}


/*!	kitty's clipboard protocol (OSC 5522): reading and writing data of any
	type.
*/
void
Terminal::_ClipboardProtocol(const char* data)
{
	std::string string(data);
	size_t separator = string.find(';');
	std::string metadata = string.substr(0, separator);
	std::string payload = separator == std::string::npos
		? std::string() : string.substr(separator + 1);

	std::string type;
	std::string mimeType;
	std::string identifier;
	std::string name;
	bool primary = false;
	bool hasMime = false;

	std::vector<std::string> pairs;
	split(metadata, ':', pairs);
	for (size_t i = 0; i < pairs.size(); i++) {
		size_t equals = pairs[i].find('=');
		if (equals == std::string::npos)
			return;	// malformed
		std::string key = pairs[i].substr(0, equals);
		std::string value = pairs[i].substr(equals + 1);
		if (key == "type")
			type = value;
		else if (key == "mime") {
			hasMime = true;
			if (!base64_decode(value.data(), value.size(), mimeType))
				return;
		} else if (key == "loc")
			primary = value == "primary";
		else if (key == "id")
			identifier = sanitize_identifier(value);
		else if (key == "name")
			base64_decode(value.data(), value.size(), name);
	}

	ClipboardWrite& write = fClipboardWrite;
	std::string idSuffix = identifier.empty() ? "" : ":id=" + identifier;

	if (type == "read") {
		std::string list;
		if (!base64_decode(payload.data(), payload.size(), list))
			return;	// read requests that are rejected get no answer
		std::vector<std::string> types;
		size_t start = 0;
		while (start < list.size()) {
			size_t end = list.find_first_of(" \t\n\r", start);
			if (end == std::string::npos)
				end = list.size();
			if (end > start)
				types.push_back(list.substr(start, end - start));
			start = end + 1;
		}
		if (fListener != NULL) {
			fListener->TerminalClipboardRead(primary, types,
				identifier.c_str(), name.c_str());
		}
		return;
	}

	if (type == "write") {
		write.active = true;
		write.failed = false;
		write.primary = primary;
		write.identifier = identifier;
		write.mimeType.clear();
		write.data.clear();
		write.aliases.clear();
		write.size = 0;
		fClipboardDecoder.Reset();
		return;
	}

	if (!write.active || write.failed)
		return;

	std::string writeSuffix = write.identifier.empty()
		? "" : ":id=" + write.identifier;

	if (type == "wdata") {
		if (!hasMime) {
			// the end of the data: commit
			if (!fClipboardDecoder.IsComplete()) {
				write.failed = true;
				write.data.clear();
				fOutput += "\x1b]5522;type=write:status=EINVAL" + writeSuffix
					+ "\x1b\\";
				return;
			}
			for (size_t i = 0; i < write.aliases.size(); i++) {
				for (size_t j = 0; j < write.data.size(); j++) {
					if (write.data[j].first == write.aliases[i].second) {
						write.data.push_back(std::make_pair(
							write.aliases[i].first, write.data[j].second));
						break;
					}
				}
			}
			if (fListener != NULL) {
				fListener->TerminalClipboardWrite(write.primary, write.data,
					write.identifier.c_str());
			}
			fOutput += "\x1b]5522;type=write:status=DONE" + writeSuffix
				+ "\x1b\\";
			write.active = false;
			write.data.clear();
			return;
		}

		if (mimeType != write.mimeType || write.data.empty()) {
			if (!fClipboardDecoder.IsComplete()) {
				write.failed = true;
				write.data.clear();
				fOutput += "\x1b]5522;type=write:status=EINVAL" + writeSuffix
					+ "\x1b\\";
				return;
			}
			fClipboardDecoder.Reset();
			write.mimeType = mimeType;
			write.data.push_back(std::make_pair(mimeType, std::string()));
		}

		std::string& target = write.data.back().second;
		size_t before = target.size();
		if (!fClipboardDecoder.Decode(payload.data(), payload.size(),
				target)) {
			write.failed = true;
			write.data.clear();
			fOutput += "\x1b]5522;type=write:status=EINVAL" + writeSuffix
				+ "\x1b\\";
			return;
		}
		write.size += target.size() - before;
		if (write.size > kMaxClipboardSize) {
			write.failed = true;
			write.data.clear();
			fOutput += "\x1b]5522;type=write:status=EFBIG" + writeSuffix
				+ "\x1b\\";
		}
		return;
	}

	if (type == "walias") {
		std::string list;
		if (!hasMime || !base64_decode(payload.data(), payload.size(), list)) {
			write.failed = true;
			write.data.clear();
			fOutput += "\x1b]5522;type=write:status=EINVAL" + writeSuffix
				+ "\x1b\\";
			return;
		}
		std::vector<std::string> aliases;
		split(list, ' ', aliases);
		for (size_t i = 0; i < aliases.size(); i++) {
			if (!aliases[i].empty())
				write.aliases.push_back(std::make_pair(aliases[i], mimeType));
		}
		return;
	}
}


void
Terminal::ReplyClipboardRead(const char* identifier, bool primary,
	const char* status, const ClipboardData* data)
{
	std::string idSuffix = identifier != NULL && identifier[0] != '\0'
		? std::string(":id=") + identifier : std::string();

	if (strcmp(status, "OK") != 0 || data == NULL) {
		fOutput += "\x1b]5522;type=read:status=";
		fOutput += status;
		fOutput += idSuffix + "\x1b\\";
		return;
	}

	fOutput += "\x1b]5522;type=read:status=OK";
	if (primary)
		fOutput += ":loc=primary";
	fOutput += idSuffix + "\x1b\\";

	for (size_t i = 0; i < data->size(); i++) {
		const std::string& type = (*data)[i].first;
		const std::string& bytes = (*data)[i].second;
		std::string encodedType;
		base64_encode(type.data(), type.size(), encodedType);
		for (size_t offset = 0; offset < bytes.size() || offset == 0;
				offset += 4096) {
			size_t chunk = std::min(bytes.size() - offset, (size_t)4096);
			std::string encoded;
			base64_encode(bytes.data() + offset, chunk, encoded);
			fOutput += "\x1b]5522;type=read:status=DATA" + idSuffix
				+ ":mime=" + encodedType + ";" + encoded + "\x1b\\";
			if (bytes.empty())
				break;
		}
	}

	fOutput += "\x1b]5522;type=read:status=DONE" + idSuffix + "\x1b\\";
}


// #pragma mark - pointer shapes


void
Terminal::_PointerShape(const char* spec)
{
	std::vector<std::string>& stack = fScreen->pointerShapes;
	char operation = '=';
	if (spec[0] == '=' || spec[0] == '>' || spec[0] == '<' || spec[0] == '?')
		operation = *spec++;

	std::vector<std::string> names;
	split(spec, ',', names);

	switch (operation) {
		case '=':
			for (size_t i = 0; i < names.size(); i++) {
				if (names[i].empty()) {
					if (!stack.empty())
						stack.back().clear();
					continue;
				}
				const char* shape = canonical_pointer_shape(names[i]);
				if (shape == NULL)
					continue;
				if (stack.empty())
					stack.push_back(shape);
				else
					stack.back() = shape;
			}
			break;
		case '>':
			for (size_t i = 0; i < names.size(); i++) {
				const char* shape = canonical_pointer_shape(names[i]);
				if (shape == NULL)
					continue;
				if (stack.size() >= kMaxPointerStackSize)
					stack.erase(stack.begin());
				stack.push_back(shape);
			}
			break;
		case '<':
			if (!stack.empty())
				stack.pop_back();
			break;
		case '?':
		{
			std::string reply;
			for (size_t i = 0; i < names.size(); i++) {
				if (i > 0)
					reply += ",";
				if (names[i] == "__current__") {
					reply += stack.empty() || stack.back().empty()
						? "0" : stack.back();
				} else if (names[i] == "__default__")
					reply += "text";
				else if (names[i] == "__grabbed__")
					reply += "default";
				else
					reply += canonical_pointer_shape(names[i]) != NULL
						? "1" : "0";
			}
			fOutput += "\x1b]22;" + reply
				+ (fParser.StringEndedWithBell() ? "\x07" : "\x1b\\");
			return;
		}
	}

	if (fListener != NULL)
		fListener->TerminalPointerShapeChanged(PointerShape());
}


// #pragma mark - shell integration


void
Terminal::_ShellIntegration(const char* spec)
{
	if (fCursorY < 0 || fCursorY >= fRows)
		return;

	std::vector<std::string> parts;
	split(spec, ';', parts);
	if (parts.empty() || parts[0].empty())
		return;

	TerminalLine& line = _Line(fCursorY);
	switch (parts[0][0]) {
		case 'A':
		{
			bool secondary = false;
			for (size_t i = 1; i < parts.size(); i++) {
				if (parts[i] == "k=s")
					secondary = true;
			}
			line.flags &= ~(LINE_PROMPT | LINE_SECONDARY_PROMPT
				| LINE_OUTPUT);
			line.flags |= secondary ? LINE_SECONDARY_PROMPT : LINE_PROMPT;
			break;
		}
		case 'C':
			line.flags &= ~(LINE_PROMPT | LINE_SECONDARY_PROMPT);
			line.flags |= LINE_OUTPUT;
			break;
		default:
			// B, D and kitty's private k markers carry nothing we use
			break;
	}
}


// #pragma mark - notifications


/*!	kitty's desktop notifications (OSC 99): metadata of key=value pairs,
	then the payload; may come in pieces with the same id.
*/
void
Terminal::_KittyNotification(const char* metadata, const char* payload)
{
	std::string identifier;
	std::string payloadType = "title";
	bool done = true;
	bool encoded = false;
	int32 actions = -1;		// bit 0 focus, bit 1 report; -1 = default
	bool reportClose = false;
	bool onlyUnfocused = false;
	int32 urgency = -1;
	std::vector<std::string> iconNames;
	std::string application;

	std::vector<std::string> pairs;
	split(metadata, ':', pairs);
	for (size_t i = 0; i < pairs.size(); i++) {
		const std::string& pair = pairs[i];
		if (pair.size() < 2 || pair[1] != '=')
			continue;
		char key = pair[0];
		std::string value = pair.substr(2);
		switch (key) {
			case 'i':
				identifier = sanitize_identifier(value);
				break;
			case 'p':
				payloadType = value;
				break;
			case 'd':
				done = value != "0";
				break;
			case 'e':
				encoded = value == "1";
				break;
			case 'a':
			{
				int32 set = 1;
				std::vector<std::string> list;
				split(value, ',', list);
				for (size_t j = 0; j < list.size(); j++) {
					bool remove = !list[j].empty() && list[j][0] == '-';
					std::string action = remove ? list[j].substr(1) : list[j];
					int32 bit = action == "focus" ? 1
						: action == "report" ? 2 : 0;
					if (remove)
						set &= ~bit;
					else
						set |= bit;
				}
				actions = set;
				break;
			}
			case 'c':
				reportClose = value == "1";
				break;
			case 'o':
				onlyUnfocused = value == "unfocused" || value == "invisible";
				break;
			case 'u':
				urgency = atoi(value.c_str());
				break;
			case 'n':
			{
				std::string name;
				if (base64_decode(value.data(), value.size(), name))
					iconNames.push_back(name);
				break;
			}
			case 'f':
				base64_decode(value.data(), value.size(), application);
				break;
		}
	}

	std::string replyId = identifier.empty() ? "0" : identifier;

	if (payloadType == "?") {
		fOutput += "\x1b]99;i=" + replyId + ":p=?;a=focus,report:c=1"
			":o=always,unfocused,invisible:p=title,body,close,?,alive"
			":s=system,silent:u=0,1,2\x1b\\";
		return;
	}

	if (payloadType == "alive") {
		std::string list;
		for (std::map<std::string, int32>::iterator it
				= fNotificationFlags.begin();
				it != fNotificationFlags.end(); it++) {
			if (!list.empty())
				list += ",";
			list += it->first;
		}
		fOutput += "\x1b]99;i=" + replyId + ":p=alive;" + list + "\x1b\\";
		return;
	}

	if (payloadType == "close") {
		if (!identifier.empty() && fListener != NULL)
			fListener->TerminalCloseNotification(identifier.c_str());
		return;
	}

	std::string text;
	if (encoded) {
		if (!base64_decode(payload, strlen(payload), text))
			return;
	} else
		text = payload;

	// without an id each one is a notification of its own
	std::string key = identifier.empty() ? std::string("\x01") : identifier;
	TermNotification& notification = fPendingNotifications[key];
	if (notification.identifier.empty() && !identifier.empty()) {
		notification.identifier = identifier;
		notification.reportActivation = false;
		notification.reportClose = false;
		notification.urgency = 1;
		notification.onlyWhenUnfocused = false;
	} else if (identifier.empty()) {
		notification.reportActivation = false;
		notification.reportClose = false;
		notification.urgency = 1;
		notification.onlyWhenUnfocused = false;
	}

	if (actions >= 0)
		notification.reportActivation = (actions & 2) != 0;
	if (reportClose)
		notification.reportClose = true;
	if (onlyUnfocused)
		notification.onlyWhenUnfocused = true;
	if (urgency >= 0)
		notification.urgency = std::min(urgency, (int32)2);
	if (!iconNames.empty() && notification.iconName.empty())
		notification.iconName = iconNames[0];

	if (notification.title.size() + notification.body.size()
			< kMaxNotificationSize) {
		if (payloadType == "title")
			notification.title += sanitize_text(text, false);
		else if (payloadType == "body")
			notification.body += sanitize_text(text, true);
		else if (payloadType == "buttons") {
			// separated by U+2028
			std::string buttons = sanitize_text(text, false);
			size_t start = 0;
			while (start <= buttons.size()) {
				size_t end = buttons.find("\xe2\x80\xa8", start);
				if (end == std::string::npos)
					end = buttons.size();
				if (end > start)
					notification.buttons.push_back(
						buttons.substr(start, end - start));
				start = end + 3;
			}
		}
	}

	if (!done)
		return;

	TermNotification complete = notification;
	fPendingNotifications.erase(key);
	if (complete.title.empty()) {
		complete.title = complete.body;
		complete.body.clear();
	}
	if (complete.title.empty())
		return;
	if (complete.onlyWhenUnfocused && fFocused)
		return;

	if (!complete.identifier.empty()) {
		fNotificationFlags[complete.identifier]
			= (complete.reportActivation ? 1 : 0)
				| (complete.reportClose ? 2 : 0);
	}
	if (fListener != NULL)
		fListener->TerminalNotify(complete);
}


void
Terminal::ReportNotificationActivated(const char* identifier, int32 button)
{
	std::string id = identifier != NULL && identifier[0] != '\0'
		? identifier : "0";
	std::map<std::string, int32>::iterator found
		= fNotificationFlags.find(id);
	if (found == fNotificationFlags.end() || (found->second & 1) == 0)
		return;

	if (button > 0)
		_Reply("\x1b]99;i=%s;%d\x1b\\", id.c_str(), (int)button);
	else
		_Reply("\x1b]99;i=%s;\x1b\\", id.c_str());
}


void
Terminal::ReportNotificationClosed(const char* identifier)
{
	std::string id = identifier != NULL ? identifier : "";
	std::map<std::string, int32>::iterator found
		= fNotificationFlags.find(id);
	if (found == fNotificationFlags.end())
		return;
	if ((found->second & 2) != 0)
		_Reply("\x1b]99;i=%s:p=close;\x1b\\", id.empty() ? "0" : id.c_str());
	fNotificationFlags.erase(found);
}


// #pragma mark - text sizing


/*!	kitty's text sizing protocol (OSC 66). Text in other sizes is not
	supported: the text is printed in the normal size, taking the cells it
	asks for (w), so that what follows it still lines up.
*/
void
Terminal::_TextSizing(const char* data)
{
	const char* separator = strchr(data, ';');
	if (separator == NULL)
		return;

	int32 width = 0;
	int32 scale = 1;
	std::vector<std::string> pairs;
	split(std::string(data, separator - data), ':', pairs);
	for (size_t i = 0; i < pairs.size(); i++) {
		if (pairs[i].compare(0, 2, "w=") == 0)
			width = atoi(pairs[i].c_str() + 2);
		else if (pairs[i].compare(0, 2, "s=") == 0)
			scale = std::max(1, std::min(7, atoi(pairs[i].c_str() + 2)));
	}

	// decode the UTF-8 text and print it
	const uint8* text = (const uint8*)separator + 1;
	std::vector<uint32> codePoints;
	while (*text != '\0') {
		uint32 c = *text++;
		int32 extra = 0;
		if (c >= 0xf0) {
			c &= 0x07;
			extra = 3;
		} else if (c >= 0xe0) {
			c &= 0x0f;
			extra = 2;
		} else if (c >= 0xc0) {
			c &= 0x1f;
			extra = 1;
		} else if (c >= 0x80)
			continue;
		while (extra-- > 0 && (*text & 0xc0) == 0x80)
			c = (c << 6) | (*text++ & 0x3f);
		if (c >= 0x20)
			codePoints.push_back(c);
	}

	int32 startX = fCursorX;
	int32 startY = fCursorY;
	for (size_t i = 0; i < codePoints.size(); i++)
		_PrintCodePoint(codePoints[i]);

	// take as many cells as the program expects: w (or the text's own
	// width) times the scale
	if (fCursorY == startY && !fPendingWrap) {
		int32 printed = fCursorX - startX;
		int32 wanted = startX + (width > 0 ? width : printed) * scale;
		while (fCursorX < wanted && fCursorX < fColumns - 1)
			_PutCharacter(' ', 1);
	}
}
