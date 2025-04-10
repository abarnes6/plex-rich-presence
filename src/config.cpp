#include "config.h"

const std::string filename = "config.toml";

Config& Config::getInstance() {
	static Config instance;
	return instance;
}

Config::Config() {
	// Load the configuration
	if (!loadConfig()) {
		std::cerr << "Failed to load configuration." << std::endl;
	}
	else {
		std::cout << "Configuration loaded successfully." << std::endl;
	}
}

bool Config::loadConfig() {
	try {
		// Check if the configuration file exists
		if (!configExists()) {
			std::cerr << "Configuration file not found. Generating default configuration..." << std::endl;
			if (!generateConfig()) {
				std::cerr << "Failed to generate configuration file." << std::endl;
				return false;
			}
		}
		config = toml::parse_file(filename);
		serverUrl = config["plex"]["server_url"].value_or("http://localhost:32400");
		authToken = config["plex"]["auth_token"].value_or("");
		pollInterval = config["plex"]["poll_interval"].value_or(5);
		clientId = config["discord"]["client_id"].value_or(1359742002618564618);
		return true;
	}
	catch (const toml::parse_error& err) {
		std::cerr << "Error parsing configuration file: " << err.what() << std::endl;
		return false;
	}
	catch (const std::exception& e) {
		std::cerr << "Error loading configuration: " << e.what() << std::endl;
		return false;
	}
}

bool Config::configExists() {
	try {
		return std::filesystem::exists(std::filesystem::path{ filename });
	}
	catch (const std::exception& e) {
		std::cerr << "Error checking configuration file: " << e.what() << std::endl;
		return false;
	}
}

bool Config::generateConfig() {
	try {
		std::ofstream configFile;
		configFile.open(filename);
		if (!configFile.is_open()) {
			std::cerr << "Error creating configuration file: " << filename << std::endl;
			return false;
		}
		configFile << "[plex]\n";
		configFile << "server_url = \"http://localhost:32400\"\n";
		configFile << "auth_token = \"\"\n";
		configFile << "poll_interval = 5\n\n";
		configFile << "[discord]\n";
		configFile << "client_id = 1359742002618564618\n";
		configFile.close();
		std::cout << "Configuration file created: " << filename << std::endl;
		return true;
	}
	catch (const std::exception& e) {
		std::cerr << "Error generating configuration file: " << e.what() << std::endl;
		return false;
	}
}