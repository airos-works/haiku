/*
 * airos_mcp - the tool sets, each registered by its own function.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#ifndef AIROS_MCP_TOOLS_H
#define AIROS_MCP_TOOLS_H

class McpServer;

void RegisterSyslogTools(McpServer& server);
void RegisterRunTools(McpServer& server);
void RegisterTeamTools(McpServer& server);
void RegisterScreenshotTools(McpServer& server);
void RegisterScriptingTools(McpServer& server);
void RegisterDriverTools(McpServer& server);
void RegisterInventoryTools(McpServer& server);
void RegisterPackageTools(McpServer& server);
void RegisterHealthTools(McpServer& server);
void RegisterFileTools(McpServer& server);

// The BApplication the GUI-facing tools need, created on first use. Returns
// false when the registrar or app_server does not answer in time.
bool EnsureApplication(const char** reason = 0);

#endif // AIROS_MCP_TOOLS_H
