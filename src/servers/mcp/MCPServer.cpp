/* Copyright 2026 air/OS contributors. Distributed under the MIT License. */
#include <MCPServer.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Messenger.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Path.h>
#include <Server.h>

#include "Mcp.h"
#include "Tools.h"
#include "Transport.h"
#include "Util.h"


static status_t
SettingsPath(BPath& path)
{
	status_t error = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (error == B_OK)
		error = path.Append("mcp_server");
	if (error == B_OK)
		error = create_directory(path.Path(), 0700);
	if (error == B_OK && chmod(path.Path(), 0700) != 0)
		error = errno;
	if (error == B_OK)
		error = path.Append("settings");
	return error;
}


static status_t
NewToken(std::string& token)
{
	unsigned char bytes[32];
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return errno;
	size_t done = 0;
	while (done < sizeof(bytes)) {
		ssize_t length = read(fd, bytes + done, sizeof(bytes) - done);
		if (length < 0 && errno == EINTR)
			continue;
		if (length <= 0) {
			status_t error = length == 0 ? B_IO_ERROR : errno;
			close(fd);
			return error;
		}
		done += length;
	}
	close(fd);
	token.clear();
	const char* hex = "0123456789abcdef";
	for (size_t i = 0; i < sizeof(bytes); i++) {
		token += hex[bytes[i] >> 4];
		token += hex[bytes[i] & 15];
	}
	return B_OK;
}


class MCPService : public BServer {
public:
	MCPService(status_t& error)
		:
		BServer(kMCPServerSignature, false, &error),
		fServer("mcp_server", "1.0.0"),
		fTransport(fServer),
		fEnabled(false),
		fError(B_OK)
	{
		RegisterSyslogTools(fServer);
		RegisterRunTools(fServer);
		RegisterTeamTools(fServer);
		RegisterScreenshotTools(fServer);
		RegisterScriptingTools(fServer);
		RegisterDriverTools(fServer);
		RegisterInventoryTools(fServer);
		RegisterPackageTools(fServer);
		RegisterHealthTools(fServer);
		RegisterFileTools(fServer);
	}

	virtual void ReadyToRun()
	{
		BPath path;
		fError = SettingsPath(path);
		if (fError != B_OK)
			return;
		BFile file(path.Path(), B_READ_ONLY);
		if (file.InitCheck() == B_ENTRY_NOT_FOUND)
			return; // No setting means off; never inherit the old lab service.
		BMessage settings;
		fError = file.InitCheck();
		if (fError == B_OK)
			fError = settings.Unflatten(&file);
		if (fError != B_OK)
			return;
		settings.FindBool("enabled", &fEnabled);
		const char* token;
		if (settings.FindString("token", &token) == B_OK)
			fToken = token;
		if (fToken.size() != 64 || fToken.find_first_not_of("0123456789abcdef")
				!= std::string::npos)
			fToken.clear();
		if (fEnabled) {
			if (fToken.empty()) {
				fError = B_BAD_DATA;
				return;
			}
			fError = _Start();
		}
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what != kMCPGetStatus && message->what != kMCPSetEnabled) {
			BServer::MessageReceived(message);
			return;
		}
		status_t error = fError;
		if (message->what == kMCPSetEnabled) {
			bool enabled;
			error = message->FindBool("enabled", &enabled);
			if (error == B_OK)
				error = _SetEnabled(enabled);
		}
		BMessage reply(B_REPLY);
		reply.AddInt32("error", error);
		reply.AddBool("enabled", fEnabled);
		reply.AddBool("running", fTransport.IsRunning());
		reply.AddString("connection_info", fTransport.IsRunning()
			? _ConnectionInfo().c_str() : "");
		message->SendReply(&reply);
	}

private:
	status_t _Save(bool enabled)
	{
		BPath path;
		status_t error = SettingsPath(path);
		if (error != B_OK)
			return error;
		std::string temporary = std::string(path.Path()) + ".new";
		int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC
			| O_NOFOLLOW | O_CLOEXEC, 0600);
		if (fd < 0)
			return errno;
		BMessage settings;
		settings.AddBool("enabled", enabled);
		settings.AddString("token", fToken.c_str());
		std::vector<char> buffer(settings.FlattenedSize());
		error = settings.Flatten(buffer.data(), buffer.size());
		if (error == B_OK && fchmod(fd, 0600) != 0)
			error = errno;
		size_t done = 0;
		while (error == B_OK && done < buffer.size()) {
			ssize_t length = write(fd, buffer.data() + done, buffer.size() - done);
			if (length < 0 && errno == EINTR)
				continue;
			if (length <= 0)
				error = length == 0 ? B_IO_ERROR : errno;
			else
				done += length;
		}
		if (error == B_OK && fsync(fd) != 0)
			error = errno;
		close(fd);
		if (error == B_OK && rename(temporary.c_str(), path.Path()) != 0)
			error = errno;
		if (error != B_OK)
			unlink(temporary.c_str());
		return error;
	}

	status_t _Start()
	{
		HttpOptions options;
		options.port = kMCPPort;
		options.token = fToken;
		return fTransport.Start(options);
	}

	status_t _SetEnabled(bool enabled)
	{
		if (!enabled) {
			// Close listener and existing connections even if saving fails.
			fTransport.Stop();
			fError = _Save(false);
			if (fError == B_OK)
				fEnabled = false;
			return fError;
		}
		if (fToken.empty()) {
			fError = NewToken(fToken);
			if (fError != B_OK)
				return fError;
		}
		fError = _Start();
		if (fError != B_OK)
			return fError;
		fError = _Save(true);
		if (fError == B_OK)
			fEnabled = true;
		else
			fTransport.Stop();
		return fError;
	}

	std::string _ConnectionInfo()
	{
		std::vector<std::string> urls;
		BNetworkRoster& roster = BNetworkRoster::Default();
		BNetworkInterface interface;
		uint32 cookie = 0;
		while (roster.GetNextInterface(&cookie, interface) == B_OK) {
			if ((interface.Flags() & IFF_UP) == 0
				|| (interface.Flags() & IFF_LOOPBACK) != 0)
				continue;
			for (int32 i = 0; i < interface.CountAddresses(); i++) {
				BNetworkInterfaceAddress address;
				if (interface.GetAddressAt(i, address) != B_OK
					|| address.Address().Family() != AF_INET
					|| address.Address().IsWildcard())
					continue;
				urls.push_back(Format("http://%s:%ld/mcp",
					address.Address().ToString(false).String(), (long)kMCPPort));
			}
		}
		std::string info = "Transport: Streamable HTTP\n";
		if (urls.empty()) {
			info += "No active IPv4 network address. Local connections only.\n";
			urls.push_back(Format("http://127.0.0.1:%ld/mcp", (long)kMCPPort));
		}
		for (size_t i = 0; i < urls.size(); i++)
			info += "URL: " + urls[i] + "\n";
		info += "Authorization: Bearer " + fToken + "\n\n";
		info += "MCP client configuration (choose a reachable URL above):\n";
		JsonValue headers = JsonValue::Object();
		headers.Set("Authorization", "Bearer " + fToken);
		JsonValue config = JsonValue::Object();
		config.Set("url", urls[0]);
		config.Set("headers", headers);
		JsonValue servers = JsonValue::Object();
		servers.Set("airOS", config);
		JsonValue root = JsonValue::Object();
		root.Set("mcpServers", servers);
		return info + root.Dump(true) + "\n";
	}

	McpServer fServer;
	HttpTransport fTransport;
	bool fEnabled;
	status_t fError;
	std::string fToken;
};


int
main(int argc, char** argv)
{
	// Local administration is also useful on machines without a working desktop.
	if (argc == 2) {
		BMessage request(kMCPGetStatus);
		if (strcmp(argv[1], "--enable") == 0 || strcmp(argv[1], "--disable") == 0) {
			request.what = kMCPSetEnabled;
			request.AddBool("enabled", strcmp(argv[1], "--enable") == 0);
		} else if (strcmp(argv[1], "--status") != 0) {
			fprintf(stderr, "usage: mcp_server [--status | --enable | --disable]\n");
			return 1;
		}
		BMessage reply;
		status_t error = BMessenger(kMCPServerSignature).SendMessage(&request,
			&reply, 5000000, 5000000);
		if (error == B_OK)
			reply.FindInt32("error", &error);
		bool enabled = false, running = false;
		reply.FindBool("enabled", &enabled);
		reply.FindBool("running", &running);
		printf("MCP: %s (saved setting: %s)\n", running ? "on" : "off",
			enabled ? "on" : "off");
		const char* info;
		if (reply.FindString("connection_info", &info) == B_OK)
			fputs(info, stdout);
		if (error != B_OK)
			fprintf(stderr, "mcp_server: %s\n", strerror(error));
		return error == B_OK ? 0 : 1;
	}
	if (argc != 1)
		return 1;
	signal(SIGPIPE, SIG_IGN);
	openlog("mcp_server", LOG_PID, LOG_USER);
	status_t error;
	MCPService service(error);
	if (error == B_OK)
		service.Run();
	return error == B_OK ? 0 : 1;
}
