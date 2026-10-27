/*
 * airos_mcp - Model Context Protocol server core (JSON-RPC 2.0).
 * Copyright 2026 air/OS contributors. MIT license.
 */
#ifndef AIROS_MCP_MCP_H
#define AIROS_MCP_MCP_H

#include <functional>
#include <string>
#include <vector>

#include <OS.h>

#include "Json.h"


struct ToolResult {
	JsonValue content;			// array of content blocks
	JsonValue structured;		// optional structuredContent (object) or null
	bool isError;

	ToolResult() : content(JsonValue::Array()), isError(false) {}

	static ToolResult Text(const std::string& text);
	static ToolResult Json(const JsonValue& value);
		// pretty JSON as text and as structuredContent
	static ToolResult Error(const std::string& message);
	static ToolResult Image(const std::string& base64, const std::string& mimeType,
		const std::string& caption);
};

typedef std::function<ToolResult(const JsonValue& arguments)> ToolHandler;

struct Tool {
	std::string name;
	std::string description;
	JsonValue inputSchema;
	ToolHandler handler;
};


class McpServer {
public:
	McpServer(const std::string& name, const std::string& version);
	~McpServer();

	void AddTool(const char* name, const char* description,
		const char* inputSchemaJson, ToolHandler handler);
	const std::vector<Tool>& Tools() const { return fTools; }

	// Handles one JSON-RPC message or batch. Returns the response text, or
	// an empty string when nothing is to be sent (notifications).
	std::string HandleText(const std::string& text);
	JsonValue HandleMessage(const JsonValue& message);

	static JsonValue MakeError(const JsonValue& id, int code,
		const std::string& message);

private:
	JsonValue _Initialize(const JsonValue& params);
	JsonValue _ListTools();
	JsonValue _CallTool(const JsonValue& params, bool& isProtocolError,
		std::string& errorText);

	std::string fName;
	std::string fVersion;
	std::vector<Tool> fTools;
	sem_id fLock;
};

#endif // AIROS_MCP_MCP_H
