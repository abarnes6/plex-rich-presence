#pragma once
#include <string>
#include <shared_mutex>

// Playback information
struct PlaybackInfo
{
    bool isPlaying = false;
    std::string mediaType = ""; // "movie", "episode", etc.
    std::string title = "";
    std::string subtitle = ""; // show name for episodes, or empty for movies
    std::string thumbnailUrl = "";
    std::string userId = "";
    std::string username = "";
    std::string state = ""; // "playing", "paused", etc.
    int64_t progress = 0;   // in seconds
    int64_t duration = 0;   // in seconds
    time_t startTime = 0;
};
