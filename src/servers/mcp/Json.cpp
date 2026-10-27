/*
 * airos_mcp - JSON value with parser and writer.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include "Json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


namespace {

struct Parser {
	const std::string& text;
	size_t pos;
	std::string error;

	Parser(const std::string& t) : text(t), pos(0) {}

	void SkipSpace()
	{
		while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'
				|| text[pos] == '\n' || text[pos] == '\r'))
			pos++;
	}

	bool Fail(const char* what)
	{
		if (error.empty()) {
			char buffer[128];
			snprintf(buffer, sizeof(buffer), "%s at offset %zu", what, pos);
			error = buffer;
		}
		return false;
	}

	static void AppendUtf8(std::string& out, uint32_t cp)
	{
		if (cp < 0x80)
			out += (char)cp;
		else if (cp < 0x800) {
			out += (char)(0xc0 | (cp >> 6));
			out += (char)(0x80 | (cp & 0x3f));
		} else if (cp < 0x10000) {
			out += (char)(0xe0 | (cp >> 12));
			out += (char)(0x80 | ((cp >> 6) & 0x3f));
			out += (char)(0x80 | (cp & 0x3f));
		} else {
			out += (char)(0xf0 | (cp >> 18));
			out += (char)(0x80 | ((cp >> 12) & 0x3f));
			out += (char)(0x80 | ((cp >> 6) & 0x3f));
			out += (char)(0x80 | (cp & 0x3f));
		}
	}

	bool ParseHex4(uint32_t& value)
	{
		if (pos + 4 > text.size())
			return Fail("truncated \\u escape");
		value = 0;
		for (int i = 0; i < 4; i++) {
			char c = text[pos++];
			value <<= 4;
			if (c >= '0' && c <= '9') value |= c - '0';
			else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
			else return Fail("bad \\u escape");
		}
		return true;
	}

	bool ParseString(std::string& out)
	{
		if (pos >= text.size() || text[pos] != '"')
			return Fail("expected string");
		pos++;
		while (pos < text.size()) {
			char c = text[pos++];
			if (c == '"')
				return true;
			if (c == '\\') {
				if (pos >= text.size())
					return Fail("truncated escape");
				char e = text[pos++];
				switch (e) {
					case '"': out += '"'; break;
					case '\\': out += '\\'; break;
					case '/': out += '/'; break;
					case 'b': out += '\b'; break;
					case 'f': out += '\f'; break;
					case 'n': out += '\n'; break;
					case 'r': out += '\r'; break;
					case 't': out += '\t'; break;
					case 'u': {
						uint32_t cp;
						if (!ParseHex4(cp))
							return false;
						if (cp >= 0xd800 && cp < 0xdc00 && pos + 6 <= text.size()
								&& text[pos] == '\\' && text[pos + 1] == 'u') {
							pos += 2;
							uint32_t low;
							if (!ParseHex4(low))
								return false;
							cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
						}
						AppendUtf8(out, cp);
						break;
					}
					default:
						return Fail("bad escape");
				}
			} else
				out += c;
		}
		return Fail("unterminated string");
	}

	bool ParseValue(JsonValue& out, int depth)
	{
		if (depth > 200)
			return Fail("nesting too deep");
		SkipSpace();
		if (pos >= text.size())
			return Fail("unexpected end of input");
		char c = text[pos];
		if (c == '{') {
			pos++;
			out = JsonValue::Object();
			SkipSpace();
			if (pos < text.size() && text[pos] == '}') {
				pos++;
				return true;
			}
			while (true) {
				SkipSpace();
				std::string key;
				if (!ParseString(key))
					return false;
				SkipSpace();
				if (pos >= text.size() || text[pos] != ':')
					return Fail("expected ':'");
				pos++;
				JsonValue value;
				if (!ParseValue(value, depth + 1))
					return false;
				out.Set(key, value);
				SkipSpace();
				if (pos >= text.size())
					return Fail("unterminated object");
				if (text[pos] == ',') {
					pos++;
					continue;
				}
				if (text[pos] == '}') {
					pos++;
					return true;
				}
				return Fail("expected ',' or '}'");
			}
		}
		if (c == '[') {
			pos++;
			out = JsonValue::Array();
			SkipSpace();
			if (pos < text.size() && text[pos] == ']') {
				pos++;
				return true;
			}
			while (true) {
				JsonValue value;
				if (!ParseValue(value, depth + 1))
					return false;
				out.Push(value);
				SkipSpace();
				if (pos >= text.size())
					return Fail("unterminated array");
				if (text[pos] == ',') {
					pos++;
					continue;
				}
				if (text[pos] == ']') {
					pos++;
					return true;
				}
				return Fail("expected ',' or ']'");
			}
		}
		if (c == '"') {
			std::string s;
			if (!ParseString(s))
				return false;
			out = JsonValue(s);
			return true;
		}
		if (text.compare(pos, 4, "true") == 0) {
			pos += 4;
			out = JsonValue(true);
			return true;
		}
		if (text.compare(pos, 5, "false") == 0) {
			pos += 5;
			out = JsonValue(false);
			return true;
		}
		if (text.compare(pos, 4, "null") == 0) {
			pos += 4;
			out = JsonValue();
			return true;
		}
		if (c == '-' || (c >= '0' && c <= '9')) {
			size_t start = pos;
			if (text[pos] == '-')
				pos++;
			while (pos < text.size() && ((text[pos] >= '0' && text[pos] <= '9')
					|| text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'
					|| text[pos] == '+' || text[pos] == '-'))
				pos++;
			std::string number = text.substr(start, pos - start);
			char* end = NULL;
			double value = strtod(number.c_str(), &end);
			if (end == NULL || *end != 0)
				return Fail("bad number");
			out = JsonValue(value);
			return true;
		}
		return Fail("unexpected character");
	}
};

} // namespace


JsonValue
JsonValue::Parse(const std::string& text, std::string* error)
{
	Parser parser(text);
	JsonValue value;
	if (!parser.ParseValue(value, 0)) {
		if (error != NULL)
			*error = parser.error;
		return JsonValue();
	}
	parser.SkipSpace();
	if (parser.pos != text.size()) {
		if (error != NULL)
			*error = "trailing characters after JSON value";
		return JsonValue();
	}
	if (error != NULL)
		error->clear();
	return value;
}


bool
JsonValue::AsBool(bool fallback) const
{
	if (fType == kBool)
		return fBool;
	if (fType == kNumber)
		return fNumber != 0;
	if (fType == kString)
		return fString == "true" || fString == "1" || fString == "yes";
	return fallback;
}


int64_t
JsonValue::AsInt(int64_t fallback) const
{
	if (fType == kNumber)
		return (int64_t)fNumber;
	if (fType == kString && !fString.empty()) {
		char* end = NULL;
		long long v = strtoll(fString.c_str(), &end, 10);
		if (end != NULL && *end == 0)
			return v;
	}
	if (fType == kBool)
		return fBool ? 1 : 0;
	return fallback;
}


double
JsonValue::AsDouble(double fallback) const
{
	if (fType == kNumber)
		return fNumber;
	if (fType == kString && !fString.empty()) {
		char* end = NULL;
		double v = strtod(fString.c_str(), &end);
		if (end != NULL && *end == 0)
			return v;
	}
	return fallback;
}


std::string
JsonValue::AsString(const std::string& fallback) const
{
	if (fType == kString)
		return fString;
	if (fType == kNumber || fType == kBool)
		return Dump();
	return fallback;
}


const JsonValue*
JsonValue::Get(const std::string& key) const
{
	if (fType != kObject)
		return NULL;
	for (size_t i = 0; i < fObject.size(); i++) {
		if (fObject[i].first == key)
			return &fObject[i].second;
	}
	return NULL;
}


JsonValue&
JsonValue::Set(const std::string& key, const JsonValue& value)
{
	if (fType != kObject) {
		fType = kObject;
		fArray.clear();
	}
	for (size_t i = 0; i < fObject.size(); i++) {
		if (fObject[i].first == key) {
			fObject[i].second = value;
			return fObject[i].second;
		}
	}
	fObject.push_back(std::make_pair(key, value));
	return fObject.back().second;
}


JsonValue&
JsonValue::operator[](const std::string& key)
{
	if (fType != kObject) {
		fType = kObject;
		fArray.clear();
	}
	for (size_t i = 0; i < fObject.size(); i++) {
		if (fObject[i].first == key)
			return fObject[i].second;
	}
	fObject.push_back(std::make_pair(key, JsonValue()));
	return fObject.back().second;
}


const JsonValue&
JsonValue::operator[](const std::string& key) const
{
	static const JsonValue kNullValue;
	const JsonValue* value = Get(key);
	return value != NULL ? *value : kNullValue;
}


std::string
JsonValue::GetString(const std::string& key, const std::string& fallback) const
{
	const JsonValue* value = Get(key);
	return value != NULL && !value->IsNull() ? value->AsString(fallback) : fallback;
}


int64_t
JsonValue::GetInt(const std::string& key, int64_t fallback) const
{
	const JsonValue* value = Get(key);
	return value != NULL ? value->AsInt(fallback) : fallback;
}


double
JsonValue::GetDouble(const std::string& key, double fallback) const
{
	const JsonValue* value = Get(key);
	return value != NULL ? value->AsDouble(fallback) : fallback;
}


bool
JsonValue::GetBool(const std::string& key, bool fallback) const
{
	const JsonValue* value = Get(key);
	return value != NULL ? value->AsBool(fallback) : fallback;
}


JsonValue&
JsonValue::Push(const JsonValue& value)
{
	if (fType != kArray) {
		fType = kArray;
		fObject.clear();
	}
	fArray.push_back(value);
	return fArray.back();
}


const JsonValue&
JsonValue::At(size_t index) const
{
	static const JsonValue kNullValue;
	if (fType != kArray || index >= fArray.size())
		return kNullValue;
	return fArray[index];
}


std::string
JsonValue::Dump(bool pretty) const
{
	std::string out;
	_Dump(out, pretty, 0);
	return out;
}


void
JsonValue::_DumpString(std::string& out, const std::string& s)
{
	out += '"';
	for (size_t i = 0; i < s.size(); i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			default:
				if (c < 0x20) {
					char buffer[8];
					snprintf(buffer, sizeof(buffer), "\\u%04x", c);
					out += buffer;
				} else
					out += (char)c;
		}
	}
	out += '"';
}


void
JsonValue::_Dump(std::string& out, bool pretty, int indent) const
{
	switch (fType) {
		case kNull:
			out += "null";
			break;
		case kBool:
			out += fBool ? "true" : "false";
			break;
		case kNumber: {
			char buffer[64];
			if (isfinite(fNumber) && fNumber == floor(fNumber)
					&& fabs(fNumber) < 9.0e15)
				snprintf(buffer, sizeof(buffer), "%lld", (long long)fNumber);
			else if (isfinite(fNumber))
				snprintf(buffer, sizeof(buffer), "%.17g", fNumber);
			else
				snprintf(buffer, sizeof(buffer), "null");
			out += buffer;
			break;
		}
		case kString:
			_DumpString(out, fString);
			break;
		case kArray: {
			if (fArray.empty()) {
				out += "[]";
				break;
			}
			out += '[';
			for (size_t i = 0; i < fArray.size(); i++) {
				if (i > 0)
					out += ',';
				if (pretty) {
					out += '\n';
					out.append((indent + 1) * 2, ' ');
				}
				fArray[i]._Dump(out, pretty, indent + 1);
			}
			if (pretty) {
				out += '\n';
				out.append(indent * 2, ' ');
			}
			out += ']';
			break;
		}
		case kObject: {
			if (fObject.empty()) {
				out += "{}";
				break;
			}
			out += '{';
			for (size_t i = 0; i < fObject.size(); i++) {
				if (i > 0)
					out += ',';
				if (pretty) {
					out += '\n';
					out.append((indent + 1) * 2, ' ');
				}
				_DumpString(out, fObject[i].first);
				out += pretty ? ": " : ":";
				fObject[i].second._Dump(out, pretty, indent + 1);
			}
			if (pretty) {
				out += '\n';
				out.append(indent * 2, ' ');
			}
			out += '}';
			break;
		}
	}
}
