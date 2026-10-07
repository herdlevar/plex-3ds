#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <map>

class PlexAPI {
public:
    PlexAPI(const std::string& clientIdentifier);
    void setClientIdentifier(const std::string& id) { m_clientIdentifier = id; }

    // Authentication (Plex PIN & Direct Login)
    bool requestPin(std::string& outPinId, std::string& outCode);
    bool checkPin(const std::string& pinId, std::string& outAuthToken);
    bool signIn(const std::string& login, const std::string& password, std::string& outAuthToken, std::string& outUsername, std::string& outError);
    bool getUser(const std::string& authToken, std::string& outUsername, std::string& outEmail);

    // Server Discovery & Connections
    bool getServers(const std::string& authToken, std::vector<PlexServer>& outServers);
    bool selectBestConnection(PlexServer& server);
    bool testServer(PlexServer& server);

    // Library & Media
    bool getLibraries(const PlexServer& server, std::vector<PlexLibrary>& outLibraries);
    bool getItems(const PlexServer& server, const std::string& keyOrSection, std::vector<PlexMediaItem>& outItems, int start = 0, int size = 100);

    // URL Generators
    std::string buildPosterUrl(const PlexServer& server, const std::string& thumbPath, int width = 128, int height = 192) const;
    std::string buildTranscodeUrl(const PlexServer& server, const PlexMediaItem& item, const AppConfig& config) const;

    // Timeline & Progress
    void reportTimeline(const PlexServer& server, const PlexMediaItem& item, int64_t timeMs, const std::string& state);

private:
    std::string m_clientIdentifier;
    std::map<std::string, std::string> getBaseHeaders(const std::string& token = "") const;
};
