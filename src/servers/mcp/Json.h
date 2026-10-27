/*
 * airos_mcp - JSON value with parser and writer.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#ifndef AIROS_MCP_JSON_H
#define AIROS_MCP_JSON_H

#include <stdint.h>
#include <string>
#include <utility>
#include <vector>


class JsonValue {
public:
	enum Type { kNull, kBool, kNumber, kString, kArray, kObject };

	JsonValue() : fType(kNull), fBool(false), fNumber(0) {}
	JsonValue(bool value) : fType(kBool), fBool(value), fNumber(0) {}
	JsonValue(int value) : fType(kNumber), fBool(false), fNumber(value) {}
	JsonValue(int64_t value) : fType(kNumber), fBool(false), fNumber((double)value) {}
	JsonValue(double value) : fType(kNumber), fBool(false), fNumber(value) {}
	JsonValue(const char* value) : fType(kString), fBool(false), fNumber(0), fString(value) {}
	JsonValue(const std::string& value) : fType(kString), fBool(false), fNumber(0), fString(value) {}

	static JsonValue Array() { JsonValue v; v.fType = kArray; return v; }
	static JsonValue Object() { JsonValue v; v.fType = kObject; return v; }

	// Parsing; on failure the returned value is null and *error describes
	// what went wrong.
	static JsonValue Parse(const std::string& text, std::string* error = NULL);

	std::string Dump(bool pretty = false) const;

	Type GetType() const { return fType; }
	bool IsNull() const { return fType == kNull; }
	bool IsBool() const { return fType == kBool; }
	bool IsNumber() const { return fType == kNumber; }
	bool IsString() const { return fType == kString; }
	bool IsArray() const { return fType == kArray; }
	bool IsObject() const { return fType == kObject; }

	bool AsBool(bool fallback = false) const;
	int64_t AsInt(int64_t fallback = 0) const;
	double AsDouble(double fallback = 0) const;
	std::string AsString(const std::string& fallback = std::string()) const;

	// Object access
	const JsonValue* Get(const std::string& key) const;
	bool Has(const std::string& key) const { return Get(key) != NULL; }
	JsonValue& Set(const std::string& key, const JsonValue& value);
	JsonValue& operator[](const std::string& key);
	const JsonValue& operator[](const std::string& key) const;
	size_t CountMembers() const { return fObject.size(); }
	const std::vector<std::pair<std::string, JsonValue> >& Members() const { return fObject; }

	// Convenience typed getters with defaults for tool arguments
	std::string GetString(const std::string& key, const std::string& fallback = std::string()) const;
	int64_t GetInt(const std::string& key, int64_t fallback = 0) const;
	double GetDouble(const std::string& key, double fallback = 0) const;
	bool GetBool(const std::string& key, bool fallback = false) const;

	// Array access
	JsonValue& Push(const JsonValue& value);
	size_t Size() const { return fArray.size(); }
	const JsonValue& At(size_t index) const;
	const std::vector<JsonValue>& Items() const { return fArray; }

private:
	void _Dump(std::string& out, bool pretty, int indent) const;
	static void _DumpString(std::string& out, const std::string& s);

	Type fType;
	bool fBool;
	double fNumber;
	std::string fString;
	std::vector<JsonValue> fArray;
	std::vector<std::pair<std::string, JsonValue> > fObject;
};

#endif // AIROS_MCP_JSON_H
