/*
 * airos_mcp - Model Context Protocol server core (JSON-RPC 2.0).
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include "Mcp.h"

#include <stdio.h>
#include <syslog.h>

#include "Util.h"


static const char* kSupportedProtocolVersions[] = {
	"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05", NULL
};
static const char* kDefaultProtocolVersion = "2025-06-18";


ToolResult
ToolResult::Text(const std::string& text)
{
	ToolResult result;
	JsonValue block = JsonValue::Object();
	block.Set("type", "text");
	block.Set("text", text);
	result.content.Push(block);
	return result;
}


ToolResult
ToolResult::Json(const JsonValue& value)
{
	ToolResult result = Text(value.Dump(true));
	if (value.IsObject())
		result.structured = value;
	return result;
}


ToolResult
ToolResult::Error(const std::string& message)
{
	ToolResult result = Text(message);
	result.isError = true;
	return result;
}


ToolResult
ToolResult::Image(const std::string& base64, const std::string& mimeType,
	const std::string& caption)
{
	ToolResult result;
	if (!caption.empty()) {
		JsonValue text = JsonValue::Object();
		text.Set("type", "text");
		text.Set("text", caption);
		result.content.Push(text);
	}
	JsonValue image = JsonValue::Object();
	image.Set("type", "image");
	image.Set("data", base64);
	image.Set("mimeType", mimeType);
	result.content.Push(image);
	return result;
}


McpServer::McpServer(const std::string& name, const std::string& version)
	:
	fName(name),
	fVersion(version)
{
	fLock = create_sem(1, "mcp tools");
}


McpServer::~McpServer()
{
	delete_sem(fLock);
}


void
McpServer::AddTool(const char* name, const char* description,
	const char* inputSchemaJson, ToolHandler handler)
{
	Tool tool;
	tool.name = name;
	tool.description = description;
	std::string error;
	tool.inputSchema = JsonValue::Parse(inputSchemaJson, &error);
	if (!error.empty()) {
		fprintf(stderr, "airos_mcp: bad schema for tool %s: %s\n", name,
			error.c_str());
		tool.inputSchema = JsonValue::Parse("{\"type\":\"object\"}");
	}
	tool.handler = handler;
	fTools.push_back(tool);
}


JsonValue
McpServer::MakeError(const JsonValue& id, int code, const std::string& message)
{
	JsonValue response = JsonValue::Object();
	response.Set("jsonrpc", "2.0");
	response.Set("id", id);
	JsonValue error = JsonValue::Object();
	error.Set("code", code);
	error.Set("message", message);
	response.Set("error", error);
	return response;
}


std::string
McpServer::HandleText(const std::string& text)
{
	std::string parseError;
	JsonValue message = JsonValue::Parse(text, &parseError);
	if (!parseError.empty())
		return MakeError(JsonValue(), -32700, "Parse error: " + parseError).Dump();

	if (message.IsArray()) {
		JsonValue responses = JsonValue::Array();
		for (size_t i = 0; i < message.Size(); i++) {
			JsonValue response = HandleMessage(message.At(i));
			if (!response.IsNull())
				responses.Push(response);
		}
		if (responses.Size() == 0)
			return std::string();
		return responses.Dump();
	}
	JsonValue response = HandleMessage(message);
	if (response.IsNull())
		return std::string();
	return response.Dump();
}


JsonValue
McpServer::HandleMessage(const JsonValue& message)
{
	if (!message.IsObject())
		return MakeError(JsonValue(), -32600, "Invalid Request");

	const JsonValue* idValue = message.Get("id");
	JsonValue id = idValue != NULL ? *idValue : JsonValue();
	bool isNotification = idValue == NULL || idValue->IsNull();
	std::string method = message.GetString("method");
	const JsonValue* paramsValue = message.Get("params");
	JsonValue params = paramsValue != NULL ? *paramsValue : JsonValue::Object();

	if (method.empty()) {
		// A response to one of our requests (we send none) or garbage.
		if (isNotification)
			return JsonValue();
		return MakeError(id, -32600, "Invalid Request: no method");
	}

	if (isNotification) {
		// notifications/initialized, notifications/cancelled, ...
		return JsonValue();
	}

	JsonValue result;
	if (method == "initialize")
		result = _Initialize(params);
	else if (method == "ping")
		result = JsonValue::Object();
	else if (method == "tools/list")
		result = _ListTools();
	else if (method == "tools/call") {
		bool protocolError = false;
		std::string errorText;
		result = _CallTool(params, protocolError, errorText);
		if (protocolError)
			return MakeError(id, -32602, errorText);
	} else if (method == "resources/list") {
		result = JsonValue::Object();
		result.Set("resources", JsonValue::Array());
	} else if (method == "resources/templates/list") {
		result = JsonValue::Object();
		result.Set("resourceTemplates", JsonValue::Array());
	} else if (method == "prompts/list") {
		result = JsonValue::Object();
		result.Set("prompts", JsonValue::Array());
	} else if (method == "logging/setLevel")
		result = JsonValue::Object();
	else
		return MakeError(id, -32601, "Method not found: " + method);

	JsonValue response = JsonValue::Object();
	response.Set("jsonrpc", "2.0");
	response.Set("id", id);
	response.Set("result", result);
	return response;
}


JsonValue
McpServer::_Initialize(const JsonValue& params)
{
	std::string requested = params.GetString("protocolVersion");
	std::string version = kDefaultProtocolVersion;
	for (int i = 0; kSupportedProtocolVersions[i] != NULL; i++) {
		if (requested == kSupportedProtocolVersions[i]) {
			version = requested;
			break;
		}
	}
	JsonValue result = JsonValue::Object();
	result.Set("protocolVersion", version);
	JsonValue capabilities = JsonValue::Object();
	JsonValue tools = JsonValue::Object();
	tools.Set("listChanged", false);
	capabilities.Set("tools", tools);
	result.Set("capabilities", capabilities);
	JsonValue info = JsonValue::Object();
	info.Set("name", fName);
	info.Set("version", fVersion);
	info.Set("title", "air/OS development tools");
	result.Set("serverInfo", info);
	result.Set("instructions",
		"Tools that run on the air/OS (Haiku) machine itself: read the syslog, "
		"run and watch test programs, list and kill teams, take screenshots, "
		"script running apps with hey-style commands, deploy kernel drivers, "
		"inventory the hardware, install packages and check the machine's "
		"health. Paths are paths on the device. The machine may be a lab board: "
		"prefer the tools over guessing from a stale transcript.");
	return result;
}


JsonValue
McpServer::_ListTools()
{
	JsonValue result = JsonValue::Object();
	JsonValue tools = JsonValue::Array();
	for (size_t i = 0; i < fTools.size(); i++) {
		JsonValue tool = JsonValue::Object();
		tool.Set("name", fTools[i].name);
		tool.Set("description", fTools[i].description);
		tool.Set("inputSchema", fTools[i].inputSchema);
		tools.Push(tool);
	}
	result.Set("tools", tools);
	return result;
}


JsonValue
McpServer::_CallTool(const JsonValue& params, bool& isProtocolError,
	std::string& errorText)
{
	std::string name = params.GetString("name");
	const JsonValue* argumentsValue = params.Get("arguments");
	JsonValue arguments = argumentsValue != NULL && argumentsValue->IsObject()
		? *argumentsValue : JsonValue::Object();

	const Tool* tool = NULL;
	for (size_t i = 0; i < fTools.size(); i++) {
		if (fTools[i].name == name) {
			tool = &fTools[i];
			break;
		}
	}
	if (tool == NULL) {
		isProtocolError = true;
		errorText = "Unknown tool: " + name;
		return JsonValue();
	}

	bigtime_t start = system_time();
	ToolResult result;
	try {
		result = tool->handler(arguments);
	} catch (std::exception& e) {
		result = ToolResult::Error(Format("tool %s threw: %s", name.c_str(),
			e.what()));
	}
	syslog(LOG_INFO, "airos_mcp: %s took %.3f s%s", name.c_str(),
		(system_time() - start) / 1000000.0, result.isError ? " (error)" : "");

	JsonValue out = JsonValue::Object();
	out.Set("content", result.content);
	if (result.structured.IsObject())
		out.Set("structuredContent", result.structured);
	out.Set("isError", result.isError);
	return out;
}
