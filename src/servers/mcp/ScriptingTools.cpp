/*
 * airos_mcp - scripting running applications with hey's grammar.
 * The parser follows Haiku's src/bin/hey.cpp (MIT, Attila Mezei and
 * others); the output is JSON instead of text.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <Application.h>
#include <Entry.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <PropertyInfo.h>
#include <Roster.h>
#include <String.h>

#include "Mcp.h"
#include "Tools.h"
#include "Util.h"


namespace {

// #pragma mark - BMessage to JSON


std::string
TypeName(type_code type)
{
	switch (type) {
		case B_STRING_TYPE: return "string";
		case B_INT8_TYPE: return "int8";
		case B_INT16_TYPE: return "int16";
		case B_INT32_TYPE: return "int32";
		case B_INT64_TYPE: return "int64";
		case B_UINT8_TYPE: return "uint8";
		case B_UINT16_TYPE: return "uint16";
		case B_UINT32_TYPE: return "uint32";
		case B_UINT64_TYPE: return "uint64";
		case B_FLOAT_TYPE: return "float";
		case B_DOUBLE_TYPE: return "double";
		case B_BOOL_TYPE: return "bool";
		case B_POINT_TYPE: return "BPoint";
		case B_RECT_TYPE: return "BRect";
		case B_SIZE_TYPE: return "BSize";
		case B_RGB_COLOR_TYPE: return "rgb_color";
		case B_MESSAGE_TYPE: return "BMessage";
		case B_MESSENGER_TYPE: return "BMessenger";
		case B_REF_TYPE: return "entry_ref";
		case B_RAW_TYPE: return "raw";
		case B_PROPERTY_INFO_TYPE: return "property_info";
		case B_MIME_TYPE: return "mime";
	}
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "'%c%c%c%c'", (char)(type >> 24),
		(char)(type >> 16), (char)(type >> 8), (char)type);
	return buffer;
}


std::string
WhatToString(uint32 what)
{
	std::string text;
	bool printable = true;
	for (int i = 3; i >= 0; i--) {
		char c = (char)(what >> (i * 8));
		if (c < 0x20 || c > 0x7e)
			printable = false;
	}
	if (printable) {
		text = "'";
		for (int i = 3; i >= 0; i--)
			text += (char)(what >> (i * 8));
		text += "'";
	} else
		text = Format("0x%x", (unsigned)what);
	return text;
}


JsonValue PropertyInfoToJson(const void* data, ssize_t size);
JsonValue MessageToJson(const BMessage& message, int depth);


JsonValue
FieldToJson(const BMessage& message, const char* name, type_code type, int32 index,
	int depth)
{
	switch (type) {
		case B_STRING_TYPE: {
			const char* s;
			if (message.FindString(name, index, &s) == B_OK)
				return JsonValue(s);
			break;
		}
		case B_INT8_TYPE: { int8 v; if (message.FindInt8(name, index, &v) == B_OK) return JsonValue((int)v); break; }
		case B_INT16_TYPE: { int16 v; if (message.FindInt16(name, index, &v) == B_OK) return JsonValue((int)v); break; }
		case B_INT32_TYPE: { int32 v; if (message.FindInt32(name, index, &v) == B_OK) return JsonValue((int64_t)v); break; }
		case B_INT64_TYPE: { int64 v; if (message.FindInt64(name, index, &v) == B_OK) return JsonValue((int64_t)v); break; }
		case B_UINT8_TYPE: { uint8 v; if (message.FindUInt8(name, index, &v) == B_OK) return JsonValue((int)v); break; }
		case B_UINT16_TYPE: { uint16 v; if (message.FindUInt16(name, index, &v) == B_OK) return JsonValue((int)v); break; }
		case B_UINT32_TYPE: { uint32 v; if (message.FindUInt32(name, index, &v) == B_OK) return JsonValue((int64_t)v); break; }
		case B_UINT64_TYPE: { uint64 v; if (message.FindUInt64(name, index, &v) == B_OK) return JsonValue((double)v); break; }
		case B_FLOAT_TYPE: { float v; if (message.FindFloat(name, index, &v) == B_OK) return JsonValue((double)v); break; }
		case B_DOUBLE_TYPE: { double v; if (message.FindDouble(name, index, &v) == B_OK) return JsonValue(v); break; }
		case B_BOOL_TYPE: { bool v; if (message.FindBool(name, index, &v) == B_OK) return JsonValue(v); break; }
		case B_POINT_TYPE: {
			BPoint p;
			if (message.FindPoint(name, index, &p) == B_OK) {
				JsonValue v = JsonValue::Object();
				v.Set("x", (double)p.x);
				v.Set("y", (double)p.y);
				return v;
			}
			break;
		}
		case B_RECT_TYPE: {
			BRect r;
			if (message.FindRect(name, index, &r) == B_OK) {
				JsonValue v = JsonValue::Object();
				v.Set("left", (double)r.left);
				v.Set("top", (double)r.top);
				v.Set("right", (double)r.right);
				v.Set("bottom", (double)r.bottom);
				v.Set("width", (double)(r.Width() + 1));
				v.Set("height", (double)(r.Height() + 1));
				return v;
			}
			break;
		}
		case B_SIZE_TYPE: {
			BSize s;
			if (message.FindSize(name, index, &s) == B_OK) {
				JsonValue v = JsonValue::Object();
				v.Set("width", (double)s.width);
				v.Set("height", (double)s.height);
				return v;
			}
			break;
		}
		case B_RGB_COLOR_TYPE: {
			const void* data;
			ssize_t size;
			if (message.FindData(name, type, index, &data, &size) == B_OK
					&& size == (ssize_t)sizeof(rgb_color)) {
				const rgb_color* c = (const rgb_color*)data;
				return JsonValue(Format("rgb(%d,%d,%d,%d)", c->red, c->green, c->blue,
					c->alpha));
			}
			break;
		}
		case B_MESSAGE_TYPE: {
			BMessage sub;
			if (message.FindMessage(name, index, &sub) == B_OK && depth < 6)
				return MessageToJson(sub, depth + 1);
			break;
		}
		case B_MESSENGER_TYPE: {
			BMessenger m;
			if (message.FindMessenger(name, index, &m) == B_OK) {
				JsonValue v = JsonValue::Object();
				v.Set("team", (int64_t)m.Team());
				v.Set("valid", m.IsValid());
				return v;
			}
			break;
		}
		case B_REF_TYPE: {
			entry_ref ref;
			if (message.FindRef(name, index, &ref) == B_OK) {
				BPath path(&ref);
				return JsonValue(path.Path() != NULL ? path.Path() : ref.name);
			}
			break;
		}
		case B_PROPERTY_INFO_TYPE: {
			const void* data;
			ssize_t size;
			if (message.FindData(name, type, index, &data, &size) == B_OK)
				return PropertyInfoToJson(data, size);
			break;
		}
		default:
			break;
	}
	const void* data;
	ssize_t size;
	if (message.FindData(name, type, index, &data, &size) == B_OK) {
		JsonValue v = JsonValue::Object();
		v.Set("type", TypeName(type));
		v.Set("size", (int64_t)size);
		if (size <= 256)
			v.Set("base64", Base64Encode(data, size));
		return v;
	}
	return JsonValue();
}


JsonValue
MessageToJson(const BMessage& message, int depth)
{
	JsonValue out = JsonValue::Object();
	out.Set("what", WhatToString(message.what));
	char* name;
	type_code type;
	int32 count;
	for (int32 i = 0; message.GetInfo(B_ANY_TYPE, i, &name, &type, &count) == B_OK;
			i++) {
		if (count == 1)
			out.Set(name, FieldToJson(message, name, type, 0, depth));
		else {
			JsonValue array = JsonValue::Array();
			for (int32 j = 0; j < count; j++)
				array.Push(FieldToJson(message, name, type, j, depth));
			out.Set(name, array);
		}
	}
	return out;
}


const char*
SpecifierName(uint32 spec)
{
	switch (spec) {
		case B_DIRECT_SPECIFIER: return "direct";
		case B_INDEX_SPECIFIER: return "index";
		case B_REVERSE_INDEX_SPECIFIER: return "reverse index";
		case B_RANGE_SPECIFIER: return "range";
		case B_REVERSE_RANGE_SPECIFIER: return "reverse range";
		case B_NAME_SPECIFIER: return "name";
		case B_ID_SPECIFIER: return "id";
	}
	return "other";
}


JsonValue
PropertyInfoToJson(const void* data, ssize_t size)
{
	BPropertyInfo info;
	JsonValue out = JsonValue::Array();
	if (info.Unflatten(B_PROPERTY_INFO_TYPE, data, size) != B_OK)
		return out;
	const property_info* properties = info.Properties();
	for (int32 i = 0; i < info.CountProperties(); i++) {
		JsonValue p = JsonValue::Object();
		p.Set("name", properties[i].name);
		JsonValue commands = JsonValue::Array();
		for (int j = 0; j < 10 && properties[i].commands[j] != 0; j++) {
			switch (properties[i].commands[j]) {
				case B_GET_PROPERTY: commands.Push("get"); break;
				case B_SET_PROPERTY: commands.Push("set"); break;
				case B_CREATE_PROPERTY: commands.Push("create"); break;
				case B_DELETE_PROPERTY: commands.Push("delete"); break;
				case B_COUNT_PROPERTIES: commands.Push("count"); break;
				case B_EXECUTE_PROPERTY: commands.Push("do"); break;
				default: commands.Push(WhatToString(properties[i].commands[j]));
			}
		}
		if (commands.Size() == 0)
			commands.Push("get"), commands.Push("set"), commands.Push("count"),
				commands.Push("create"), commands.Push("delete"), commands.Push("do");
		p.Set("commands", commands);
		JsonValue specifiers = JsonValue::Array();
		for (int j = 0; j < 10 && properties[i].specifiers[j] != 0; j++)
			specifiers.Push(SpecifierName(properties[i].specifiers[j]));
		if (specifiers.Size() == 0)
			specifiers.Push("any");
		p.Set("specifiers", specifiers);
		if (properties[i].usage != NULL && properties[i].usage[0] != 0)
			p.Set("usage", properties[i].usage);
		JsonValue types = JsonValue::Array();
		for (int j = 0; j < 10 && properties[i].types[j] != 0; j++)
			types.Push(TypeName(properties[i].types[j]));
		if (types.Size() > 0)
			p.Set("types", types);
		out.Push(p);
	}
	return out;
}


// #pragma mark - hey grammar


struct HeyParser {
	std::vector<std::string> tokens;
	size_t index;
	std::string error;

	HeyParser(const std::string& text) : index(0) { Tokenize(text); }

	void Tokenize(const std::string& text)
	{
		std::string current;
		bool inQuotes = false;
		bool any = false;
		for (size_t i = 0; i < text.size(); i++) {
			char c = text[i];
			if (c == '"') {
				inQuotes = !inQuotes;
				current += c;	// kept, as hey does, and stripped by the consumers
				any = true;
				continue;
			}
			if (!inQuotes && (c == ' ' || c == '\t' || c == '\n')) {
				if (any) {
					tokens.push_back(current);
					current.clear();
					any = false;
				}
				continue;
			}
			current += c;
			any = true;
		}
		if (any)
			tokens.push_back(current);
	}

	const char* Peek() const { return index < tokens.size() ? tokens[index].c_str() : NULL; }
	const char* Next() { return index < tokens.size() ? tokens[index++].c_str() : NULL; }
	bool AtEnd() const { return index >= tokens.size(); }
	size_t Count() const { return tokens.size(); }

	static std::string Unquote(const char* s)
	{
		std::string value(s);
		if (value.size() >= 2 && value[0] == '"' && value[value.size() - 1] == '"')
			return value.substr(1, value.size() - 2);
		if (!value.empty() && value[0] == '"')
			return value.substr(1);
		return value;
	}

	// returns B_OK if successful, B_ERROR if no more specifiers,
	// B_BAD_SCRIPT_SYNTAX if syntax error
	status_t AddSpecifier(BMessage& message)
	{
		const char* property = Next();
		if (property == NULL)
			return B_ERROR;
		if (strcasecmp(property, "do") == 0 || strcasecmp(property, "to") == 0)
			return B_ERROR;
		if (strcasecmp(property, "with") == 0) {
			index--;
			return B_ERROR;
		}
		if (strcasecmp(property, "of") == 0) {
			property = Next();
			if (property == NULL)
				return B_BAD_SCRIPT_SYNTAX;
		}
		if (strcasecmp(property, "the") == 0) {
			property = Next();
			if (property == NULL)
				return B_BAD_SCRIPT_SYNTAX;
		}
		std::string propertyName = Unquote(property);

		const char* specifier = NULL;
		if (message.what != B_CREATE_PROPERTY)
			specifier = Peek();
		if (specifier == NULL) {
			message.AddSpecifier(propertyName.c_str());
			return B_ERROR;
		}
		index++;
		if (strcasecmp(specifier, "of") == 0) {
			message.AddSpecifier(propertyName.c_str());
			return B_OK;
		}
		if (strcasecmp(specifier, "to") == 0 || strcasecmp(specifier, "with") == 0) {
			message.AddSpecifier(propertyName.c_str());
			index--;
			return B_ERROR;
		}
		if (specifier[0] == '[') {
			char* end;
			if (specifier[1] == '-') {
				int32 ix = strtoul(specifier + 2, &end, 10);
				BMessage reverse(B_REVERSE_INDEX_SPECIFIER);
				reverse.AddString("property", propertyName.c_str());
				reverse.AddInt32("index", ix);
				message.AddSpecifier(&reverse);
				return B_OK;
			}
			int32 ix1 = strtoul(specifier + 1, &end, 10);
			if (end[0] == ']') {
				message.AddSpecifier(propertyName.c_str(), ix1);
				return B_OK;
			}
			const char* next = Next();
			if (next == NULL) {
				message.AddSpecifier(propertyName.c_str(), ix1);
				return B_OK;
			}
			if (strcasecmp(next, "to") == 0) {
				const char* toValue = Next();
				if (toValue == NULL)
					return B_BAD_SCRIPT_SYNTAX;
				int32 ix2 = strtoul(toValue, &end, 10);
				message.AddSpecifier(propertyName.c_str(), ix1,
					ix2 - ix1 > 0 ? ix2 - ix1 : 1);
				return B_OK;
			}
			return B_BAD_SCRIPT_SYNTAX;
		}
		// name specifier, or an index when it is all digits
		bool reverse = specifier[0] == '-';
		bool isIndex = strlen(specifier) > (reverse ? 1u : 0u);
		for (size_t i = reverse ? 1 : 0; specifier[i] != 0; i++) {
			if (specifier[i] < '0' || specifier[i] > '9') {
				isIndex = false;
				break;
			}
		}
		if (isIndex) {
			if (reverse) {
				BMessage reverseSpec(B_REVERSE_INDEX_SPECIFIER);
				reverseSpec.AddString("property", propertyName.c_str());
				reverseSpec.AddInt32("index", atol(specifier + 1));
				message.AddSpecifier(&reverseSpec);
			} else
				message.AddSpecifier(propertyName.c_str(), atol(specifier));
		} else
			message.AddSpecifier(propertyName.c_str(), Unquote(specifier).c_str());
		return B_OK;
	}

	status_t AddData(BMessage& message)
	{
		const char* raw = Next();
		if (raw == NULL)
			return B_ERROR;
		std::string valueString(raw);

		bool onlyDigits = !valueString.empty();
		bool floating = false;
		for (size_t i = 0; i < valueString.size(); i++) {
			char c = valueString[i];
			if (i == 0 && c == '-')
				continue;
			if (c == '.')
				floating = true;
			else if (c < '0' || c > '9') {
				onlyDigits = false;
				break;
			}
		}
		if (onlyDigits && valueString != "-") {
			if (floating)
				message.AddFloat("data", atof(valueString.c_str()));
			else
				message.AddInt32("data", atol(valueString.c_str()));
			return B_OK;
		}
		if (strcasecmp(raw, "true") == 0) {
			message.AddBool("data", true);
			return B_OK;
		}
		if (strcasecmp(raw, "false") == 0) {
			message.AddBool("data", false);
			return B_OK;
		}

		std::string name = "data";
		size_t eq = valueString.find('=');
		if (eq != std::string::npos && eq > 0 && valueString[0] != '"') {
			name = valueString.substr(0, eq);
			valueString = valueString.substr(eq + 1);
		}
		const char* value = valueString.c_str();
		#define TYPED(prefix) (strncasecmp(value, prefix, strlen(prefix)) == 0)
		if (TYPED("int8("))
			message.AddInt8(name.c_str(), atol(value + 5));
		else if (TYPED("int16("))
			message.AddInt16(name.c_str(), atol(value + 6));
		else if (TYPED("int32("))
			message.AddInt32(name.c_str(), atol(value + 6));
		else if (TYPED("int64("))
			message.AddInt64(name.c_str(), atoll(value + 6));
		else if (TYPED("bool(")) {
			const char* b = value + 5;
			if (strncasecmp(b, "true", 4) == 0)
				message.AddBool(name.c_str(), true);
			else if (strncasecmp(b, "false", 5) == 0)
				message.AddBool(name.c_str(), false);
			else
				message.AddBool(name.c_str(), atol(b) != 0);
		} else if (TYPED("float("))
			message.AddFloat(name.c_str(), atof(value + 6));
		else if (TYPED("double("))
			message.AddDouble(name.c_str(), atof(value + 7));
		else if (TYPED("string(")) {
			std::string s(value + 7);
			if (!s.empty() && s[s.size() - 1] == ')')
				s.erase(s.size() - 1);
			message.AddString(name.c_str(), Unquote(s.c_str()).c_str());
		} else if (TYPED("BPoint(")) {
			float x = atof(value + 7), y = 0;
			const char* comma = strchr(value, ',');
			if (comma == NULL)
				comma = strchr(value, ' ');
			if (comma != NULL)
				y = atof(comma + 1);
			message.AddPoint(name.c_str(), BPoint(x, y));
		} else if (TYPED("BRect(")) {
			float v[4] = {0, 0, 0, 0};
			const char* p = value + 6;
			for (int i = 0; i < 4 && p != NULL; i++) {
				v[i] = atof(p);
				p = strchr(p, ',');
				if (p != NULL)
					p++;
			}
			message.AddRect(name.c_str(), BRect(v[0], v[1], v[2], v[3]));
		} else if (TYPED("rgb_color(")) {
			int v[4] = {0, 0, 0, 255};
			const char* p = value + 10;
			for (int i = 0; i < 4 && p != NULL; i++) {
				v[i] = atol(p);
				p = strchr(p, ',');
				if (p != NULL)
					p++;
			}
			rgb_color color = {(uint8)v[0], (uint8)v[1], (uint8)v[2], (uint8)v[3]};
			message.AddData(name.c_str(), B_RGB_COLOR_TYPE, &color, sizeof(color));
		} else if (TYPED("file(")) {
			std::string path(value + 5);
			if (!path.empty() && (path[path.size() - 1] == ')' || path[path.size() - 1] == ']'))
				path.erase(path.size() - 1);
			path = Unquote(path.c_str());
			entry_ref ref;
			if (get_ref_for_path(path.c_str(), &ref) != B_OK)
				return B_ENTRY_NOT_FOUND;
			BEntry entry(&ref);
			if (entry.InitCheck() != B_OK)
				return B_ENTRY_NOT_FOUND;
			message.AddRef("refs", &ref);
			message.AddRef(name.c_str(), &ref);
		} else
			message.AddString(name.c_str(), Unquote(raw).c_str());
		#undef TYPED
		return B_OK;
	}

	status_t AddWith(BMessage& message)
	{
		const char* next = Peek();
		if (next == NULL)
			return B_OK;
		if (strcasecmp(next, "with") != 0)
			return B_OK;
		index++;
		while (true) {
			status_t result = AddData(message);
			if (result != B_OK)
				return result == B_ERROR ? B_BAD_SCRIPT_SYNTAX : result;
			const char* more = Peek();
			if (more != NULL && strcasecmp(more, "and") == 0) {
				index++;
				continue;
			}
			break;
		}
		return B_OK;
	}

	// Builds the message; on failure sets error.
	bool Build(BMessenger* target, BMessage& message)
	{
		const char* verb = Next();
		if (verb == NULL) {
			error = "empty command";
			return false;
		}
		if (strcasecmp(verb, "let") == 0) {
			// hey App let Specifier do Verb
			BMessage getTarget(B_GET_PROPERTY);
			getTarget.AddSpecifier("Messenger");
			status_t result;
			while ((result = AddSpecifier(getTarget)) == B_OK)
				;
			if (result != B_ERROR) {
				error = "bad specifier syntax after let";
				return false;
			}
			BMessage reply;
			if (target->SendMessage(&getTarget, &reply, 5000000, 5000000) != B_OK
					|| reply.FindMessenger("result", target) != B_OK) {
				error = "could not get the Messenger of the let target";
				return false;
			}
			verb = Next();
			if (verb == NULL || strcasecmp(verb, "do") != 0) {
				error = "expected 'do' after the let specifier";
				return false;
			}
			verb = Next();
			if (verb == NULL) {
				error = "no verb after 'do'";
				return false;
			}
		}
		bool direct = false;
		if (strcasecmp(verb, "do") == 0) message.what = B_EXECUTE_PROPERTY;
		else if (strcasecmp(verb, "get") == 0) message.what = B_GET_PROPERTY;
		else if (strcasecmp(verb, "set") == 0) message.what = B_SET_PROPERTY;
		else if (strcasecmp(verb, "create") == 0) message.what = B_CREATE_PROPERTY;
		else if (strcasecmp(verb, "delete") == 0) message.what = B_DELETE_PROPERTY;
		else if (strcasecmp(verb, "quit") == 0) message.what = B_QUIT_REQUESTED;
		else if (strcasecmp(verb, "save") == 0) message.what = B_SAVE_REQUESTED;
		else if (strcasecmp(verb, "load") == 0) message.what = B_REFS_RECEIVED;
		else if (strcasecmp(verb, "count") == 0) message.what = B_COUNT_PROPERTIES;
		else if (strcasecmp(verb, "getsuites") == 0) message.what = B_GET_SUPPORTED_SUITES;
		else {
			size_t length = strlen(verb);
			if (length >= 1 && length <= 4) {
				uint32 what = 0;
				for (size_t i = 0; i < length; i++)
					what = (what << 8) | (uint8)verb[i];
				message.what = what;
			} else if (strncasecmp(verb, "0x", 2) == 0)
				message.what = strtoul(verb, NULL, 16);
			else {
				// a command named in the target's supported suites
				bool found = false;
				BMessage suites;
				BMessage suitesRequest(B_GET_SUPPORTED_SUITES);
				if (target->SendMessage(&suitesRequest, &suites, 5000000,
						5000000) == B_OK) {
					const void* data;
					ssize_t size;
					for (int32 j = 0; !found && suites.FindData("messages",
							B_PROPERTY_INFO_TYPE, j, &data, &size) == B_OK; j++) {
						BPropertyInfo info;
						if (info.Unflatten(B_PROPERTY_INFO_TYPE, data, size) != B_OK)
							continue;
						const value_info* values = info.Values();
						for (int32 k = 0; k < info.CountValues(); k++) {
							if (strcmp(values[k].name, verb) == 0) {
								message.what = values[k].value;
								found = true;
								break;
							}
						}
					}
				}
				if (!found) {
					error = Format("bad verb \"%s\" (use get, set, count, create, "
						"delete, do, quit, save, load, getsuites, a 4-char code or "
						"0x code)", verb);
					return false;
				}
			}
			direct = true;
		}

		if (direct && index == Count() - 1) {
			// one data item at the end of the line
			AddData(message);
		} else if (message.what != B_REFS_RECEIVED) {
			status_t result;
			while ((result = AddSpecifier(message)) == B_OK)
				;
			if (result != B_ERROR) {
				error = "bad specifier syntax";
				return false;
			}
		}
		if ((message.what == B_SET_PROPERTY || message.what == B_REFS_RECEIVED)
				&& Peek() != NULL) {
			if (strcasecmp(Peek(), "to") == 0)
				index++;
			status_t result = AddData(message);
			if (result != B_OK) {
				error = result == B_ENTRY_NOT_FOUND ? "file not found"
					: "invalid 'to ...' value";
				return false;
			}
		}
		status_t result = AddWith(message);
		if (result != B_OK) {
			error = result == B_ENTRY_NOT_FOUND ? "file not found"
				: "invalid 'with ...' value";
			return false;
		}
		return true;
	}
};


// #pragma mark - targets


bool
ResolveTarget(const std::string& target, BMessenger& messenger, std::string& error,
	JsonValue& resolved)
{
	// A team id?
	char* end = NULL;
	long team = strtol(target.c_str(), &end, 10);
	if (end != NULL && *end == 0 && !target.empty()) {
		messenger = BMessenger(NULL, (team_id)team, NULL);
		resolved.Set("team", (int64_t)team);
		if (!messenger.IsValid()) {
			error = Format("team %ld is not a running application", team);
			return false;
		}
		return true;
	}
	// A signature?
	if (target.find('/') != std::string::npos) {
		messenger = BMessenger(target.c_str());
		resolved.Set("signature", target);
		if (!messenger.IsValid()) {
			error = "no running application with signature " + target;
			return false;
		}
		resolved.Set("team", (int64_t)messenger.Team());
		return true;
	}
	// A name: match against the running applications' file names and
	// signatures, as hey does.
	BList teams;
	be_roster->GetAppList(&teams);
	std::vector<std::pair<team_id, std::string> > candidates;
	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id id = (team_id)(addr_t)teams.ItemAt(i);
		app_info info;
		if (be_roster->GetRunningAppInfo(id, &info) != B_OK)
			continue;
		std::string name = info.ref.name != NULL ? info.ref.name : "";
		if (strcasecmp(name.c_str(), target.c_str()) == 0
				|| strcasestr(info.signature, target.c_str()) != NULL)
			candidates.push_back(std::make_pair(id, std::string(info.signature)));
	}
	// exact file name first
	for (size_t i = 0; i < candidates.size(); i++) {
		app_info info;
		be_roster->GetRunningAppInfo(candidates[i].first, &info);
		if (strcasecmp(info.ref.name, target.c_str()) == 0) {
			messenger = BMessenger(NULL, candidates[i].first);
			resolved.Set("team", (int64_t)candidates[i].first);
			resolved.Set("signature", candidates[i].second);
			return messenger.IsValid();
		}
	}
	if (!candidates.empty()) {
		messenger = BMessenger(NULL, candidates[0].first);
		resolved.Set("team", (int64_t)candidates[0].first);
		resolved.Set("signature", candidates[0].second);
		if (candidates.size() > 1)
			resolved.Set("note", Format("%zu applications matched; using the first",
				candidates.size()));
		return messenger.IsValid();
	}
	error = "no running application named " + target
		+ " (app_list shows what runs; a signature like application/x-vnd.Be-TRAK "
		"or a team id also works)";
	return false;
}


ToolResult
Hey(const JsonValue& args)
{
	const char* reason = NULL;
	if (!EnsureApplication(&reason))
		return ToolResult::Error(std::string("scripting needs the registrar: ") + reason);
	std::string target = args.GetString("target");
	std::string command = args.GetString("command");
	bigtime_t timeout = SecondsToMicro(args.GetDouble("timeout_s", 5));
	if (target.empty() || command.empty())
		return ToolResult::Error("target and command are required");

	BMessenger messenger;
	std::string error;
	JsonValue result = JsonValue::Object();
	JsonValue resolved = JsonValue::Object();
	if (!ResolveTarget(target, messenger, error, resolved))
		return ToolResult::Error(error);
	result.Set("target", resolved);

	HeyParser parser(command);
	BMessage message;
	if (!parser.Build(&messenger, message))
		return ToolResult::Error("cannot parse command: " + parser.error);
	result.Set("sent", MessageToJson(message, 0));

	BMessage reply;
	status_t status = messenger.SendMessage(&message, &reply, timeout, timeout);
	if (status != B_OK) {
		result.Set("error", "SendMessage failed: " + StrError(status) + (status
			== B_TIMED_OUT ? " (the application's looper is busy or hung; see "
			"team_threads)" : ""));
		ToolResult out = ToolResult::Json(result);
		out.isError = true;
		return out;
	}
	result.Set("reply", MessageToJson(reply, 0));
	int32 replyError;
	if (reply.FindInt32("error", &replyError) == B_OK && replyError != B_OK) {
		const char* text;
		std::string explanation = StrError(replyError);
		if (reply.FindString("message", &text) == B_OK)
			explanation += std::string(": ") + text;
		result.Set("error", explanation);
		if (replyError == B_BAD_SCRIPT_SYNTAX)
			result.Set("hint", "the property or specifier is not supported; try "
				"\"getsuites\" (optionally \"getsuites of Window 0\") to list them");
		ToolResult out = ToolResult::Json(result);
		out.isError = true;
		return out;
	}
	if (reply.what == B_MESSAGE_NOT_UNDERSTOOD)
		result.Set("hint", "B_MESSAGE_NOT_UNDERSTOOD: the target has no such property "
			"or verb");
	return ToolResult::Json(result);
}


ToolResult
AppList(const JsonValue& args)
{
	const char* reason = NULL;
	if (!EnsureApplication(&reason))
		return ToolResult::Error(std::string("needs the registrar: ") + reason);
	std::string filter = args.GetString("filter");
	BList teams;
	be_roster->GetAppList(&teams);
	JsonValue apps = JsonValue::Array();
	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id id = (team_id)(addr_t)teams.ItemAt(i);
		app_info info;
		if (be_roster->GetRunningAppInfo(id, &info) != B_OK)
			continue;
		BPath path(&info.ref);
		if (!filter.empty() && strcasestr(info.signature, filter.c_str()) == NULL
				&& (path.Path() == NULL || strcasestr(path.Path(), filter.c_str()) == NULL))
			continue;
		JsonValue app = JsonValue::Object();
		app.Set("team", (int64_t)id);
		app.Set("signature", info.signature);
		app.Set("path", path.Path() != NULL ? path.Path() : "");
		app.Set("name", info.ref.name);
		JsonValue flags = JsonValue::Array();
		if ((info.flags & B_BACKGROUND_APP) != 0) flags.Push("background");
		if ((info.flags & B_ARGV_ONLY) != 0) flags.Push("argv only");
		switch (info.flags & B_LAUNCH_MASK) {
			case B_SINGLE_LAUNCH: flags.Push("single launch"); break;
			case B_MULTIPLE_LAUNCH: flags.Push("multiple launch"); break;
			case B_EXCLUSIVE_LAUNCH: flags.Push("exclusive launch"); break;
		}
		app.Set("flags", flags);
		apps.Push(app);
	}
	JsonValue result = JsonValue::Object();
	result.Set("apps", apps);
	result.Set("count", (int64_t)apps.Size());
	return ToolResult::Json(result);
}


ToolResult
AppLaunch(const JsonValue& args)
{
	const char* reason = NULL;
	if (!EnsureApplication(&reason))
		return ToolResult::Error(std::string("needs the registrar: ") + reason);
	std::string what = args.GetString("app");
	if (what.empty())
		return ToolResult::Error("app (a signature or a path) is required");
	std::vector<std::string> argStrings;
	const JsonValue* argv = args.Get("args");
	if (argv != NULL && argv->IsArray()) {
		for (size_t i = 0; i < argv->Size(); i++)
			argStrings.push_back(argv->At(i).AsString());
	}
	std::vector<const char*> cArgs;
	for (size_t i = 0; i < argStrings.size(); i++)
		cArgs.push_back(argStrings[i].c_str());

	team_id team = -1;
	status_t status;
	if (what.find('/') != std::string::npos && what[0] == '/') {
		entry_ref ref;
		status = get_ref_for_path(ExpandPath(what).c_str(), &ref);
		if (status == B_OK)
			status = be_roster->Launch(&ref, (int)cArgs.size(),
				cArgs.empty() ? NULL : (const char* const*)cArgs.data(), &team);
	} else
		status = be_roster->Launch(what.c_str(), (int)cArgs.size(),
			cArgs.empty() ? NULL : (char**)cArgs.data(), &team);
	JsonValue result = JsonValue::Object();
	result.Set("status", StrError(status));
	result.Set("team", (int64_t)team);
	if (status == B_ALREADY_RUNNING)
		result.Set("note", "already running (single launch); the team is the "
			"existing one and received the arguments");
	if (status != B_OK && status != B_ALREADY_RUNNING) {
		ToolResult out = ToolResult::Json(result);
		out.isError = true;
		return out;
	}
	return ToolResult::Json(result);
}

} // namespace


void
RegisterScriptingTools(McpServer& server)
{
	server.AddTool("hey",
		"Send a scripting message to a running application using hey's grammar "
		"and return the reply as JSON. target is an application file name "
		"(airTime, Tracker), a signature (application/x-vnd.Be-TRAK) or a team "
		"id. Commands: \"get Frame of Window 0\", \"set Position of Window 0 to "
		"1.5\", \"count Window\", \"get Title of Window [-1]\", \"getsuites\", "
		"\"getsuites of Window 0\", \"do Save of Window 0\", \"quit\", "
		"\"create Entry with path=string(/boot)\", or a 4-char message code "
		"such as \"_PLY\". Values: ints, floats, true/false, \"quoted strings\", "
		"int32(5), float(1.5), bool(true), BPoint(10,20), BRect(0,0,100,100), "
		"rgb_color(255,0,0), file(/path). The reply's 'result' field holds the "
		"value; an 'error' with B_BAD_SCRIPT_SYNTAX means the property or "
		"specifier is not supported.",
		"{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\"},"
		"\"command\":{\"type\":\"string\"},\"timeout_s\":{\"type\":\"number\","
		"\"description\":\"reply timeout (default 5)\"}},"
		"\"required\":[\"target\",\"command\"]}", Hey);
	server.AddTool("app_list",
		"Running applications known to the registrar: team, signature, path, "
		"launch flags. Use the signature or name as hey's target.",
		"{\"type\":\"object\",\"properties\":{\"filter\":{\"type\":\"string\"}}}",
		AppList);
	server.AddTool("app_launch",
		"Launch an application by signature or absolute path through the roster, "
		"as the Deskbar would (no shell environment), with optional arguments. "
		"Returns the team.",
		"{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\"},"
		"\"args\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}},"
		"\"required\":[\"app\"]}", AppLaunch);
}
