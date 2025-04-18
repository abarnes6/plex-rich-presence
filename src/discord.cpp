#include "discord.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h> // For newer socket functions
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#if defined(_WIN32) && !defined(htole32)
#define htole32(x) (x) // little‑endian host
#define le32toh(x) (x)
#endif

using json = nlohmann::json;

// Discord IPC opcodes
enum DiscordOpcodes
{
	OP_HANDSHAKE = 0,
	OP_FRAME = 1,
	OP_CLOSE = 2,
	OP_PING = 3,
	OP_PONG = 4
};

Discord::Discord() : running(false),
					 connected(false),
					 needs_reconnect(false),
					 is_playing(false),
					 start_timestamp(0),
					 reconnect_attempts(0),
					 last_successful_update(0)
{
#ifdef _WIN32
	pipe_handle = INVALID_HANDLE_VALUE;
#else
	pipe_fd = -1;
#endif
}

Discord::~Discord()
{
	stop();
}

bool Discord::init()
{
	running = true;
	conn_thread = std::thread(&Discord::connectionThread, this);
	client_id = Config::getInstance().clientId;
	std::cout << "Initializing Discord with client ID: " << client_id << std::endl;
	return true;
}

void Discord::connectionThread()
{
	while (running)
	{
		if (!connected)
		{
			if (connectToDiscord())
			{
				// After connecting, send handshake
				// Create the correct format that Discord requires
				json payload = {
					{"client_id", std::to_string(client_id)},
					{"v", 1}};

				std::string handshake_str = payload.dump();
				std::cout << "Sending handshake payload: " << handshake_str << std::endl;

				if (!writeFrame(OP_HANDSHAKE, handshake_str))
				{
					std::cerr << "Handshake write failed" << std::endl;
					disconnectFromDiscord();
					calculateBackoffTime();
					continue;
				}

				// Read handshake response - Discord should reply
				int opcode;
				std::string response;
				if (!readFrame(opcode, response) || opcode != OP_FRAME)
				{
					std::cerr << "Failed to read handshake response. Opcode: " << opcode << std::endl;
					if (!response.empty())
					{
						std::cerr << "Response: " << response << std::endl;
					}
					disconnectFromDiscord();
					calculateBackoffTime();
					continue;
				}

				std::cout << "Handshake response: " << response << std::endl;

				// Verify READY was received
				json ready;
				try
				{
					ready = json::parse(response);
					if (ready["evt"] != "READY")
					{
						std::cerr << "Discord did not respond with READY event" << std::endl;
						disconnectFromDiscord();
						calculateBackoffTime();
						continue;
					}
				}
				catch (const std::exception &e)
				{
					std::cerr << "Failed to parse READY response: " << e.what() << std::endl;
					disconnectFromDiscord();
					calculateBackoffTime();
					continue;
				}

				connected = true;
				reconnect_attempts = 0; // Reset attempts after successful connection
				std::cout << "Connected to Discord" << std::endl;

				// If we have a last activity cached, restore it immediately
				if (!last_activity_payload.empty())
				{
					std::cout << "Restoring previous activity state..." << std::endl;
					if (!writeFrame(OP_FRAME, last_activity_payload))
					{
						std::cerr << "Failed to restore activity state" << std::endl;
						needs_reconnect = true;
					}
					else
					{
						// Read response to verify
						readFrame(opcode, response);
					}
				}
			}
			else
			{
				// Wait before retrying with exponential backoff
				std::cerr << "Failed to connect to Discord" << std::endl;
				calculateBackoffTime();
			}
		}
		else
		{
			// Keep connection alive with periodic pings
			static auto last_ping = std::chrono::steady_clock::now();
			if (std::chrono::steady_clock::now() - last_ping > std::chrono::seconds(15))
			{
				keepAlive();
				last_ping = std::chrono::steady_clock::now();
			}

			if (needs_reconnect)
			{
				std::cout << "Reconnecting to Discord..." << std::endl;
				disconnectFromDiscord();
				connected = false;
				needs_reconnect = false;
				continue;
			}

			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
}

void Discord::calculateBackoffTime()
{
	// Exponential backoff: 2^n seconds with a max of 60 seconds
	int backoff_secs = std::min(1 << std::min(reconnect_attempts, 5), 60);
	reconnect_attempts++;
	std::cout << "Waiting " << backoff_secs << " seconds before reconnecting (attempt " << reconnect_attempts << ")" << std::endl;
	std::this_thread::sleep_for(std::chrono::seconds(backoff_secs));
}

bool Discord::connectToDiscord()
{
#ifdef _WIN32
	// Windows implementation using named pipes
	for (int i = 0; i < 10; i++)
	{
		std::string pipeName = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
		std::cout << "Attempting to connect to pipe: " << pipeName << std::endl;

		pipe_handle = CreateFile(
			pipeName.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,
			NULL,
			OPEN_EXISTING,
			0,
			NULL);

		if (pipe_handle != INVALID_HANDLE_VALUE)
		{
			// Try setting pipe to message mode, but don't fail if this doesn't work
			// Some Discord versions may not support message mode
			DWORD mode = PIPE_READMODE_MESSAGE;
			if (!SetNamedPipeHandleState(pipe_handle, &mode, NULL, NULL))
			{
				DWORD error = GetLastError();
				std::cerr << "Warning: Failed to set pipe mode. Using default mode. Error: " << error << std::endl;
				// Continue anyway - don't disconnect
			}

			std::cout << "Successfully connected to Discord pipe: " << pipeName << std::endl;
			return true;
		}

		// Log the specific error for debugging
		DWORD error = GetLastError();
		std::cerr << "Failed to connect to " << pipeName << ": error code " << error << std::endl;
	}
	std::cerr << "Could not connect to any Discord pipe. Is Discord running?" << std::endl;
	return false;
#else
	// Unix implementation using sockets
	for (int i = 0; i < 10; i++)
	{
		std::string socket_path;
		const char *temp = getenv("XDG_RUNTIME_DIR");
		const char *home = getenv("HOME");

		if (temp)
		{
			socket_path = std::string(temp) + "/discord-ipc-" + std::to_string(i);
		}
		else if (home)
		{
			socket_path = std::string(home) + "/.discord-ipc-" + std::to_string(i);
		}
		else
		{
			continue;
		}

		pipe_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (pipe_fd == -1)
		{
			continue;
		}

		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

		if (connect(pipe_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
		{
			return true;
		}

		close(pipe_fd);
		pipe_fd = -1;
	}
	return false;
#endif
}

void Discord::disconnectFromDiscord()
{
#ifdef _WIN32
	if (pipe_handle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(pipe_handle);
		pipe_handle = INVALID_HANDLE_VALUE;
	}
#else
	if (pipe_fd != -1)
	{
		close(pipe_fd);
		pipe_fd = -1;
	}
#endif
	connected = false;
}

bool Discord::writeFrame(int opcode, const std::string &json)
{
	uint32_t len = static_cast<uint32_t>(json.size());
	std::vector<char> buf(8 + len); // Dynamic buffer sized exactly for our payload
	auto *p = reinterpret_cast<uint32_t *>(buf.data());
	p[0] = htole32(opcode); // endian-safe
	p[1] = htole32(len);
	memcpy(buf.data() + 8, json.data(), len);

#ifdef _WIN32
	DWORD written;
	if (!WriteFile(pipe_handle, buf.data(), 8 + len, &written, nullptr) ||
		written != 8 + len)
	{
		needs_reconnect = true;
		return false;
	}
	FlushFileBuffers(pipe_handle);
#else
	ssize_t n = ::write(pipe_fd, buf.data(), 8 + len);
	if (n != 8 + len)
	{
		needs_reconnect = true;
		return false;
	}
#endif
	return true;
}

bool Discord::readFrame(int &opcode, std::string &data)
{
	opcode = -1;
	// Read header (8 bytes)
	char header[8];
	int header_bytes_read = 0;

#ifdef _WIN32
	while (header_bytes_read < 8)
	{
		DWORD bytes_read;
		if (!ReadFile(pipe_handle, header + header_bytes_read, 8 - header_bytes_read, &bytes_read, NULL))
		{
			DWORD error = GetLastError();
			std::cerr << "Failed to read header: error code " << error << std::endl;
			needs_reconnect = true;
			return false;
		}

		if (bytes_read == 0)
		{
			std::cerr << "Read zero bytes from pipe - connection closed" << std::endl;
			needs_reconnect = true;
			return false;
		}

		header_bytes_read += bytes_read;
	}
#else
	while (header_bytes_read < 8)
	{
		ssize_t bytes_read = read(pipe_fd, header + header_bytes_read, 8 - header_bytes_read);
		if (bytes_read <= 0)
		{
			if (bytes_read < 0)
			{
				std::cerr << "Error reading from socket: " << strerror(errno) << std::endl;
			}
			else
			{
				std::cerr << "Socket closed during header read" << std::endl;
			}
			needs_reconnect = true;
			return false;
		}
		header_bytes_read += bytes_read;
	}
#endif

	// Extract opcode and length (with proper endianness handling)
	uint32_t raw0, raw1;
	memcpy(&raw0, header, 4);
	memcpy(&raw1, header + 4, 4);
	opcode = le32toh(raw0);
	uint32_t length = le32toh(raw1);

	std::cout << "Received frame - Opcode: " << opcode << ", Length: " << length << std::endl;

	if (length == 0)
	{
		data.clear();
		return true;
	}

	// Read data
	data.resize(length);
	uint32_t data_bytes_read = 0;

#ifdef _WIN32
	while (data_bytes_read < length)
	{
		DWORD bytes_read;
		if (!ReadFile(pipe_handle, &data[data_bytes_read], length - data_bytes_read, &bytes_read, NULL))
		{
			DWORD error = GetLastError();
			std::cerr << "Failed to read data: error code " << error << std::endl;
			needs_reconnect = true;
			return false;
		}

		if (bytes_read == 0)
		{
			std::cerr << "Read zero bytes from pipe during payload read - connection closed" << std::endl;
			needs_reconnect = true;
			return false;
		}

		data_bytes_read += bytes_read;
	}
#else
	while (data_bytes_read < length)
	{
		ssize_t bytes_read = read(pipe_fd, &data[data_bytes_read], length - data_bytes_read);
		if (bytes_read <= 0)
		{
			if (bytes_read < 0)
			{
				std::cerr << "Error reading from socket: " << strerror(errno) << std::endl;
			}
			else
			{
				std::cerr << "Socket closed during payload read" << std::endl;
			}
			needs_reconnect = true;
			return false;
		}
		data_bytes_read += bytes_read;
	}
#endif

	std::cout << "Successfully read frame with data: " << data << std::endl;
	return true;
}

void Discord::updatePresence(const PlaybackInfo &playbackInfo)
{
	if (!connected)
	{
		std::cerr << "Can't update presence: not connected to Discord" << std::endl;
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);

	// Rate limiting - don't allow more than 5 updates per 20 seconds
	auto now_time = std::chrono::steady_clock::now();
	auto now_seconds = std::chrono::duration_cast<std::chrono::seconds>(now_time.time_since_epoch()).count();
	if (now_seconds - last_successful_update < 4) // 20s ÷ 5 = 4s minimum between updates
	{
		std::cout << "Rate limiting: skipping presence update (too soon)" << std::endl;
		return;
	}

	// Check if we're already showing the same content
	std::string new_details;
	std::string new_state;
	int64_t new_timestamp = 0;

	if (playbackInfo.state == "playing" || playbackInfo.state == "paused")
	{
		is_playing = true;

		// Format details (title)
		new_details = playbackInfo.title;
		if (!playbackInfo.subtitle.empty())
		{
			new_details += " - " + playbackInfo.subtitle;
		}

		// Truncate details if too long (Discord limit is 128 chars)
		if (new_details.length() > 128)
		{
			new_details = new_details.substr(0, 125) + "...";
		}

		// Format state (media type & status)
		new_state = playbackInfo.mediaType;
		if (playbackInfo.state == "paused")
		{
			new_state += " (Paused)";
		}

		// Truncate state if too long (Discord limit is 128 chars)
		if (new_state.length() > 128)
		{
			new_state = new_state.substr(0, 125) + "...";
		}

		// Calculate timestamps
		auto now = std::chrono::system_clock::now();
		int64_t current_time = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

		if (playbackInfo.state == "playing")
		{
			// For playing state, calculate end timestamp (when content will finish)
			if (playbackInfo.duration > 0 && playbackInfo.progress >= 0)
			{
				// Calculate remaining time (in seconds)
				int64_t remaining = static_cast<int64_t>(playbackInfo.duration - playbackInfo.progress);
				new_timestamp = current_time + remaining;
			}
		}

		// If nothing changed, don't update
		if (current_details == new_details && current_state == new_state &&
			((playbackInfo.state == "paused" && start_timestamp == 0) ||
			 (playbackInfo.state != "paused" && start_timestamp == new_timestamp)))
		{
			std::cout << "Skipping presence update - no changes detected" << std::endl;
			return;
		}

		current_details = new_details;
		current_state = new_state;
		start_timestamp = (playbackInfo.state == "paused") ? 0 : new_timestamp;

		// Create rich presence payload
		json presence = {
			{"cmd", "SET_ACTIVITY"},
			{"args", {{"pid", static_cast<int>(
#ifdef _WIN32
								  GetCurrentProcessId()
#else
								  getpid()
#endif
									  )},
					  {"activity", {{"details", current_details}, {"state", current_state}, {"assets", {{"large_image", "plex_logo"}, {"large_text", "Watching on Plex"}}}}}}},
			{"nonce", std::to_string(time(nullptr))}};

		// Add timestamps if playing (not paused)
		if (playbackInfo.state == "playing" && start_timestamp > 0)
		{
			// Use "end" timestamp to show countdown timer
			presence["args"]["activity"]["timestamps"] = {{"end", start_timestamp}};
		}

		std::string presence_str = presence.dump();
		std::cout << "Sending presence update: " << presence_str << std::endl;

		// Send presence update
		if (!writeFrame(OP_FRAME, presence_str))
		{
			std::cerr << "Failed to send presence update" << std::endl;
			needs_reconnect = true;
		}
		else
		{
			// Cache the last successful activity payload for quick restoration after reconnect
			last_activity_payload = presence_str;

			// Read response to verify it worked
			int opcode;
			std::string response;
			if (readFrame(opcode, response))
			{
				std::cout << "Discord response: opcode=" << opcode << ", data=" << response << std::endl;

				// Validate response - check for error
				try
				{
					json response_json = json::parse(response);
					if (response_json.contains("evt") && response_json["evt"] == "ERROR")
					{
						std::cerr << "Discord rejected presence update: " << response << std::endl;
						// If we hit rate limit, don't update timestamp
						if (response_json.contains("data") &&
							response_json["data"].contains("code") &&
							response_json["data"]["code"] == 4000)
						{
							std::cerr << "Rate limit hit, backing off" << std::endl;
							return;
						}
					}
					// Check if assets were invalid
					else if (response_json.contains("data") &&
							 response_json["data"].contains("activity") &&
							 response_json["data"]["activity"].contains("assets"))
					{
						auto assets = response_json["data"]["activity"]["assets"];
						if (assets.is_null() || !assets.contains("large_image"))
						{
							std::cerr << "Warning: large_image asset 'plex_logo' was not found. "
									  << "Make sure it's uploaded in Discord Developer Portal." << std::endl;
						}
					}

					// Update the last update timestamp only if successful
					last_successful_update = now_seconds;
				}
				catch (const std::exception &e)
				{
					std::cerr << "Failed to parse response JSON: " << e.what() << std::endl;
				}
			}
			else
			{
				std::cerr << "Failed to read Discord response" << std::endl;
			}
		}
	}
	else
	{
		// Clear presence if not playing anymore
		if (is_playing)
		{
			clearPresence();
			last_successful_update = now_seconds;
		}
	}
}

void Discord::clearPresence()
{
	if (!connected)
	{
		std::cerr << "Can't clear presence: not connected to Discord" << std::endl;
		return;
	}

	std::lock_guard<std::mutex> lock(mutex);

	// Reset state tracking variables
	current_details.clear();
	current_state.clear();
	start_timestamp = 0;
	is_playing = false;
	last_activity_payload.clear();

	// Create empty presence payload to clear current presence
	json presence = {
		{"cmd", "SET_ACTIVITY"},
		{"args", {{"pid", static_cast<int>(
#ifdef _WIN32
							  GetCurrentProcessId()
#else
							  getpid()
#endif
								  )},
				  {"activity", nullptr}}},
		{"nonce", std::to_string(time(nullptr))}};

	std::cout << "Clearing presence" << std::endl;
	std::string presence_str = presence.dump();

	if (!writeFrame(OP_FRAME, presence_str))
	{
		std::cerr << "Failed to clear presence" << std::endl;
		needs_reconnect = true;
	}
	else
	{
		// Read and log response
		int opcode;
		std::string response;
		if (readFrame(opcode, response))
		{
			std::cout << "Clear presence response: opcode=" << opcode << ", data=" << response << std::endl;
		}
		else
		{
			std::cerr << "Failed to read clear presence response" << std::endl;
		}
	}
}

void Discord::keepAlive()
{
	static const json ping = json::object(); // empty payload
	std::string ping_str = ping.dump();

	if (!writeFrame(OP_PING, ping_str))
	{
		std::cerr << "Failed to send ping" << std::endl;
		needs_reconnect = true;
		return;
	}

	// Read and process PONG response
	int opcode;
	std::string response;
	if (readFrame(opcode, response))
	{
		if (opcode == OP_PONG)
		{
			std::cout << "Received PONG from Discord" << std::endl;
		}
		else
		{
			std::cerr << "Unexpected response to PING. Opcode: " << opcode << std::endl;
		}
	}
	else
	{
		std::cerr << "Failed to read PONG response" << std::endl;
		needs_reconnect = true;
	}
}

// Lifecycle control
void Discord::start()
{
	init();
}

void Discord::stop()
{
	running = false;
	if (conn_thread.joinable())
		conn_thread.join();
	disconnectFromDiscord();
}

bool Discord::isConnected() const
{
	return connected;
}
