#include "config.h"
#include "logger.h"
#include <fstream>
#include <iostream>
#include <shared_mutex>

// Define a default client ID constant
constexpr long long DEFAULT_CLIENT_ID = 1359742002618564618;

Config::Config() {
	loadConfig();
}

Config &Config::getInstance()
{
    static Config instance;
    return instance;
}

// Get platform-appropriate config directory
std::filesystem::path Config::getConfigDirectory()
{
    std::filesystem::path configDir;
    
    #ifdef _WIN32
        // On Windows, use %APPDATA%\PlexRichPresence
        char* appdata = nullptr;
        size_t requiredSize;
        _dupenv_s(&appdata, &requiredSize, "APPDATA");
        if (appdata) {
            configDir = std::filesystem::path(appdata) / "PlexRichPresence";
            free(appdata); // Free the allocated memory
        } else {
            configDir = std::filesystem::current_path() / "config";
        }
    #elif defined(__APPLE__)
        // On macOS, use ~/Library/Application Support/PlexRichPresence
        const char* home = std::getenv("HOME");
        if (home) {
            configDir = std::filesystem::path(home) / "Library/Application Support/PlexRichPresence";
        } else {
            configDir = std::filesystem::current_path() / "config";
        }
    #else
        // On Linux, use ~/.config/plex-rich-presence
        const char* home = std::getenv("HOME");
        if (home) {
            configDir = std::filesystem::path(home) / ".config/plex-rich-presence";
        } else {
            configDir = std::filesystem::current_path() / "config";
        }
    #endif
    
    // Ensure the directory exists
    if (!std::filesystem::exists(configDir)) {
        try {
            std::filesystem::create_directories(configDir);
        } catch (const std::exception& e) {
            LOG_ERROR_STREAM("Config", "Failed to create config directory: " << e.what());
        }
    }
    
    return configDir;
}

// Get the full path to the config file
std::filesystem::path Config::getConfigFilePath() const
{
    return getConfigDirectory() / "config.toml";
}

bool Config::configExists()
{
    return std::filesystem::exists(getConfigFilePath());
}

bool Config::generateConfig()
{
    try
    {
        std::filesystem::path configPath = getConfigFilePath();
        std::filesystem::path configDir = getConfigDirectory();
        
        // Ensure directory exists
        if (!std::filesystem::exists(configDir)) {
            std::filesystem::create_directories(configDir);
        }
        
        LOG_INFO_STREAM("Config", "Generating default configuration at " << configPath.string());
        
        toml::table config;
        config.insert("plex", toml::table{});
        config["plex"].as_table()->insert("server_ip", "127.0.0.1");
        config["plex"].as_table()->insert("port", 32400); 
        config["plex"].as_table()->insert("force_https", true);
        config["plex"].as_table()->insert("poll_interval", 5);
        config["plex"].as_table()->insert("plex_token", "");

        config.insert("discord", toml::table{});
        config["discord"].as_table()->insert("client_id", DEFAULT_CLIENT_ID);

        config.insert("app", toml::table{});
        config["app"].as_table()->insert("log_level", static_cast<int>(LogLevel::Info));

        std::ofstream configFile(configPath);
        configFile << config;
        configFile.close();

        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR_STREAM("Config", "Error generating configuration file: " << e.what());
        return false;
    }
}

bool Config::loadConfig()
{
    try
    {
        // Check if the configuration file exists
        if (!configExists())
        {
            LOG_INFO("Config", "Configuration file not found. Generating default configuration...");
            if (!generateConfig())
            {
                LOG_ERROR("Config", "Failed to generate configuration file.");
                return false;
            }
        }
        
        std::filesystem::path configPath = getConfigFilePath();
        LOG_INFO_STREAM("Config", "Loading configuration from " << configPath.string());
        
        config = toml::parse_file(configPath.string());
        
        // Load all configuration values with defaults if missing
        serverIp = config["plex"]["server_ip"].value_or("127.0.0.1");
        port = config["plex"]["port"].value_or(32400);
        forceHttps = config["plex"]["force_https"].value_or(false);
        pollInterval = config["plex"]["poll_interval"].value_or(5);
        plexToken = config["plex"]["plex_token"].value_or(std::string{});
        
        clientId = config["discord"]["client_id"].value_or(1359742002618564618);
        
        logLevel = config["app"]["log_level"].value_or(static_cast<int>(LogLevel::Info));
        
        return true;
    }
    catch (const toml::parse_error &err)
    {
        LOG_ERROR_STREAM("Config", "Error parsing configuration file: " << err.what());
        return false;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR_STREAM("Config", "Error loading configuration: " << e.what());
        return false;
    }
}

bool Config::setConfigValue(const std::string &key, const std::string &value)
{
    try {
        std::filesystem::path configPath = getConfigFilePath();
        
        // Parse existing config
        auto parsed_config = toml::parse_file(configPath.string());
        
        // Update values with a simple key format like "plex.auth_token"
        size_t pos = key.find('.');
        if (pos != std::string::npos) {
            std::string section = key.substr(0, pos);
            std::string option = key.substr(pos + 1);
			if (parsed_config[section].is_table()) {
				parsed_config[section].as_table()->insert_or_assign(option, value);
			} else {
				LOG_ERROR_STREAM("Config", "Section " << section << " is not a table.");
				return false;
			}
        }
        
        // Write back to file
        std::ofstream configFile(configPath);
        configFile << parsed_config;
        configFile.close();
		loadConfig(); // Reload config to update in-memory values
        
        return true;
    } catch (const std::exception &e) {
        LOG_ERROR_STREAM("Config", "Error updating configuration: " << e.what());
        return false;
    }
}

std::string Config::getServerIp() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return serverIp;
}

void Config::setServerIp(const std::string &url) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    serverIp = url;
}

int Config::getPort() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return port;
}

void Config::setPort(int p) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    port = p;
}

bool Config::isForceHttps() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return forceHttps;
}

void Config::setForceHttps(bool https) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    forceHttps = https;
}

std::string Config::getPlexToken() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return plexToken;
}

void Config::setPlexToken(const std::string &token) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    plexToken = token;
	setConfigValue("plex.plex_token", token);
}

uint32_t Config::getPollInterval() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return pollInterval;
}

void Config::setPollInterval(const uint32_t &interval) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    pollInterval = interval;
}

long long Config::getClientId() const {
	std::shared_lock<std::shared_mutex> lock(mutex);
	return clientId;
}

void Config::setClientId(long long id) {
	std::unique_lock<std::shared_mutex> lock(mutex);
	clientId = id;
}

int Config::getLogLevel() const {
    std::shared_lock<std::shared_mutex> lock(mutex);
    return logLevel;
}

void Config::setLogLevel(int level) {
    std::unique_lock<std::shared_mutex> lock(mutex);
    logLevel = level;
}