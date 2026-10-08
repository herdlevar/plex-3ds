#include "plex_api.hpp"
#include "network/http.hpp"
#include "cJSON.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <cctype>

#ifdef __3DS__
#include <3ds.h>
#endif

PlexAPI::PlexAPI(const std::string& clientIdentifier)
    : m_clientIdentifier(clientIdentifier) {}

std::map<std::string, std::string> PlexAPI::getBaseHeaders(const std::string& token) const {
    std::map<std::string, std::string> headers = {
        {"X-Plex-Product", "Plex3DS"},
        {"X-Plex-Version", "0.1.0"},
        {"X-Plex-Client-Identifier", m_clientIdentifier},
        {"X-Plex-Device", "Nintendo 3DS"},
        {"X-Plex-Device-Name", "New Nintendo 3DS"},
        {"X-Plex-Platform", "Chrome"},
        {"Accept", "application/json"}
    };
    if (!token.empty()) {
        headers["X-Plex-Token"] = token;
    }
    return headers;
}

bool PlexAPI::requestPin(std::string& outPinId, std::string& outCode) {
    auto headers = getBaseHeaders();
    auto resp = Network::post("https://plex.tv/api/v2/pins", "", headers);
    if (!resp.success) return false;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    cJSON* idItem = cJSON_GetObjectItem(root, "id");
    cJSON* codeItem = cJSON_GetObjectItem(root, "code");

    if (idItem && codeItem) {
        if (cJSON_IsNumber(idItem)) {
            outPinId = std::to_string(idItem->valueint);
        } else if (cJSON_IsString(idItem)) {
            outPinId = idItem->valuestring;
        }
        outCode = codeItem->valuestring ? codeItem->valuestring : "";
        cJSON_Delete(root);
        return !outPinId.empty() && !outCode.empty();
    }

    cJSON_Delete(root);
    return false;
}

bool PlexAPI::checkPin(const std::string& pinId, std::string& outAuthToken) {
    auto headers = getBaseHeaders();
    std::string url = "https://plex.tv/api/v2/pins/" + pinId;
    auto resp = Network::get(url, headers);
    if (!resp.success) return false;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    cJSON* tokenItem = cJSON_GetObjectItem(root, "authToken");
    if (tokenItem && cJSON_IsString(tokenItem) && tokenItem->valuestring) {
        outAuthToken = tokenItem->valuestring;
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    return false;
}

bool PlexAPI::signIn(const std::string& login, const std::string& password, std::string& outAuthToken, std::string& outUsername, std::string& outError) {
    cJSON* userObj = cJSON_CreateObject();
    cJSON_AddStringToObject(userObj, "login", login.c_str());
    cJSON_AddStringToObject(userObj, "password", password.c_str());

    cJSON* reqRoot = cJSON_CreateObject();
    cJSON_AddItemToObject(reqRoot, "user", userObj);

    char* reqBody = cJSON_PrintUnformatted(reqRoot);
    std::string postData = reqBody ? reqBody : "";
    if (reqBody) free(reqBody);
    cJSON_Delete(reqRoot);

    auto headers = getBaseHeaders();
    headers["Content-Type"] = "application/json";

    auto resp = Network::post("https://plex.tv/api/v2/users/signin", postData, headers);
    if (!resp.success || (resp.statusCode != 200 && resp.statusCode != 201)) {
        if (!resp.body.empty()) {
            cJSON* root = cJSON_Parse(resp.body.c_str());
            if (root) {
                cJSON* errors = cJSON_GetObjectItem(root, "errors");
                if (errors && cJSON_IsArray(errors) && cJSON_GetArraySize(errors) > 0) {
                    cJSON* err0 = cJSON_GetArrayItem(errors, 0);
                    cJSON* msg = cJSON_GetObjectItem(err0, "message");
                    if (msg && msg->valuestring) outError = msg->valuestring;
                }
                if (outError.empty()) {
                    cJSON* msg = cJSON_GetObjectItem(root, "message");
                    if (msg && msg->valuestring) outError = msg->valuestring;
                }
                cJSON_Delete(root);
            }
        }
        if (outError.empty()) {
            outError = (resp.statusCode == 401) ? "Invalid email or password" : ("Sign-in failed (HTTP " + std::to_string(resp.statusCode) + ")");
        }
        return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        outError = "Failed to parse sign-in response";
        return false;
    }

    cJSON* tokenItem = cJSON_GetObjectItem(root, "authToken");
    cJSON* usernameItem = cJSON_GetObjectItem(root, "username");
    if (!tokenItem || !tokenItem->valuestring) {
        cJSON* userWrapper = cJSON_GetObjectItem(root, "user");
        if (userWrapper) {
            tokenItem = cJSON_GetObjectItem(userWrapper, "authToken");
            if (!tokenItem) tokenItem = cJSON_GetObjectItem(userWrapper, "authentication_token");
            if (!usernameItem) usernameItem = cJSON_GetObjectItem(userWrapper, "username");
        }
    }

    if (tokenItem && tokenItem->valuestring) {
        outAuthToken = tokenItem->valuestring;
        if (usernameItem && usernameItem->valuestring) {
            outUsername = usernameItem->valuestring;
        } else {
            outUsername = login;
        }
        cJSON_Delete(root);
        return true;
    }

    cJSON_Delete(root);
    outError = "No auth token found in response";
    return false;
}

bool PlexAPI::getUser(const std::string& authToken, std::string& outUsername, std::string& outEmail) {
    if (authToken.empty()) return false;
    auto headers = getBaseHeaders(authToken);
    auto resp = Network::get("https://plex.tv/api/v2/user", headers);
    if (!resp.success || resp.statusCode != 200) {
        resp = Network::get("https://plex.tv/users/account.json", headers);
        if (!resp.success || resp.statusCode != 200) return false;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    cJSON* u = cJSON_GetObjectItem(root, "username");
    cJSON* e = cJSON_GetObjectItem(root, "email");
    if (!u) {
        cJSON* userWrapper = cJSON_GetObjectItem(root, "user");
        if (userWrapper) {
            u = cJSON_GetObjectItem(userWrapper, "username");
            e = cJSON_GetObjectItem(userWrapper, "email");
        }
    }

    if (u && u->valuestring) outUsername = u->valuestring;
    if (e && e->valuestring) outEmail = e->valuestring;

    cJSON_Delete(root);
    return !outUsername.empty();
}

bool PlexAPI::getServers(const std::string& authToken, std::vector<PlexServer>& outServers) {
    auto headers = getBaseHeaders(authToken);
    std::string url = "https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1";
    auto resp = Network::get(url, headers);
    if (!resp.success) return false;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count; i++) {
        cJSON* resItem = cJSON_GetArrayItem(root, i);
        cJSON* provides = cJSON_GetObjectItem(resItem, "provides");
        if (provides && cJSON_IsString(provides) && strstr(provides->valuestring, "server")) {
            PlexServer s;
            cJSON* name = cJSON_GetObjectItem(resItem, "name");
            cJSON* cid = cJSON_GetObjectItem(resItem, "clientIdentifier");
            cJSON* token = cJSON_GetObjectItem(resItem, "accessToken");

            s.name = name && name->valuestring ? name->valuestring : "Unknown Server";
            s.clientIdentifier = cid && cid->valuestring ? cid->valuestring : "";
            s.accessToken = token && token->valuestring ? token->valuestring : authToken;

            cJSON* conns = cJSON_GetObjectItem(resItem, "connections");
            if (conns && cJSON_IsArray(conns)) {
                int cCount = cJSON_GetArraySize(conns);
                for (int j = 0; j < cCount; j++) {
                    cJSON* cItem = cJSON_GetArrayItem(conns, j);
                    cJSON* uri = cJSON_GetObjectItem(cItem, "uri");
                    cJSON* local = cJSON_GetObjectItem(cItem, "local");
                    cJSON* relay = cJSON_GetObjectItem(cItem, "relay");

                    if (uri && uri->valuestring) {
                        PlexConnection conn;
                        conn.uri = uri->valuestring;
                        conn.local = local ? cJSON_IsTrue(local) : false;
                        conn.relay = relay ? cJSON_IsTrue(relay) : false;
                        s.connections.push_back(conn);
                    }
                }
            }
            outServers.push_back(s);
        }
    }

    cJSON_Delete(root);
    return !outServers.empty();
}

bool PlexAPI::selectBestConnection(PlexServer& server) {
    if (!server.selectedUri.empty()) {
        std::string testUrl = server.selectedUri + "/library/sections";
        auto resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
        if (resp.success) {
            return true;
        }
    }

    if (server.connections.empty()) return false;

    // 1. Prioritize local non-relay unencrypted http:// first (fastest for 3DS, no SSL overhead)
    for (const auto& c : server.connections) {
        if (c.local && !c.relay && c.uri.rfind("http://", 0) == 0) {
            std::string testUrl = c.uri + "/library/sections";
            auto resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
            if (resp.success) {
                server.selectedUri = c.uri;
                return true;
            }
        }
    }

    // 2. Prioritize local non-relay connection (e.g. https)
    for (const auto& c : server.connections) {
        if (c.local && !c.relay) {
            std::string testUrl = c.uri + "/library/sections";
            auto resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
            if (resp.success) {
                server.selectedUri = c.uri;
                return true;
            }
        }
    }

    // Fallback to any working connection
    for (const auto& c : server.connections) {
        std::string testUrl = c.uri + "/library/sections";
        auto resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
        if (resp.success) {
            server.selectedUri = c.uri;
            return true;
        }
    }

    return false;
}

bool PlexAPI::testServer(PlexServer& server) {
    if (server.selectedUri.empty()) return false;

    // Normalize URL
    std::string uri = server.selectedUri;
    while (!uri.empty() && (uri.front() == ' ' || uri.front() == '\t')) uri.erase(uri.begin());
    while (!uri.empty() && (uri.back() == ' ' || uri.back() == '\t' || uri.back() == '/')) uri.pop_back();

    if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0) {
        uri = "http://" + uri;
    }

    size_t schemePos = uri.find("://");
    size_t hostStart = (schemePos != std::string::npos) ? (schemePos + 3) : 0;
    size_t colonPos = uri.find(':', hostStart);
    if (colonPos == std::string::npos) {
        size_t slashPos = uri.find('/', hostStart);
        if (slashPos == std::string::npos) {
            uri += ":32400";
        } else {
            uri.insert(slashPos, ":32400");
        }
    }

    server.selectedUri = uri;

    std::string testUrl = uri + "/identity";
    auto resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
    if (!resp.success || (resp.statusCode != 200 && resp.statusCode != 401)) {
        testUrl = uri + "/";
        resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
    }
    if (!resp.success || (resp.statusCode != 200 && resp.statusCode != 401)) {
        testUrl = uri + "/library/sections";
        resp = Network::get(testUrl, getBaseHeaders(server.accessToken));
    }

    if (resp.success && resp.statusCode == 200) {
        cJSON* root = cJSON_Parse(resp.body.c_str());
        if (root) {
            cJSON* mc = cJSON_GetObjectItem(root, "MediaContainer");
            if (mc) {
                cJSON* fname = cJSON_GetObjectItem(mc, "friendlyName");
                cJSON* mid = cJSON_GetObjectItem(mc, "machineIdentifier");
                if (fname && fname->valuestring && (server.name.empty() || server.name == "Custom Server" || server.name == "Local Server")) {
                    server.name = fname->valuestring;
                }
                if (mid && mid->valuestring && server.clientIdentifier.empty()) {
                    server.clientIdentifier = mid->valuestring;
                }
            }
            cJSON_Delete(root);
        }
        if (server.name.empty() || server.name == "Custom Server" || server.name == "Local Server") {
            server.name = uri;
        }

        bool hasConn = false;
        for (const auto& c : server.connections) {
            if (c.uri == uri) { hasConn = true; break; }
        }
        if (!hasConn) {
            PlexConnection conn;
            conn.uri = uri;
            conn.local = true;
            conn.relay = false;
            server.connections.push_back(conn);
        }
        return true;
    }
    return false;
}

bool PlexAPI::getLibraries(const PlexServer& server, std::vector<PlexLibrary>& outLibraries) {
    if (server.selectedUri.empty()) return false;

    std::string url = server.selectedUri + "/library/sections";
    auto resp = Network::get(url, getBaseHeaders(server.accessToken));
    if (!resp.success) return false;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    cJSON* mc = cJSON_GetObjectItem(root, "MediaContainer");
    cJSON* dirs = mc ? cJSON_GetObjectItem(mc, "Directory") : nullptr;
    if (dirs && cJSON_IsArray(dirs)) {
        int count = cJSON_GetArraySize(dirs);
        for (int i = 0; i < count; i++) {
            cJSON* d = cJSON_GetArrayItem(dirs, i);
            cJSON* key = cJSON_GetObjectItem(d, "key");
            cJSON* title = cJSON_GetObjectItem(d, "title");
            cJSON* type = cJSON_GetObjectItem(d, "type");

            if (key && title) {
                PlexLibrary lib;
                lib.key = key->valuestring ? key->valuestring : "";
                lib.title = title->valuestring ? title->valuestring : "";
                lib.type = type && type->valuestring ? type->valuestring : "unknown";
                outLibraries.push_back(lib);
            }
        }
    }

    cJSON_Delete(root);
    return true;
}

static std::string urlEncode(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() * 3);
    static const char hexChars[] = "0123456789ABCDEF";
    for (char c : value) {
        if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped += c;
        } else {
            escaped += '%';
            escaped += hexChars[((unsigned char)c >> 4) & 0x0F];
            escaped += hexChars[(unsigned char)c & 0x0F];
        }
    }
    return escaped;
}

bool PlexAPI::getItems(const PlexServer& server, const std::string& keyOrSection, std::vector<PlexMediaItem>& outItems, int start, int size) {
    if (server.selectedUri.empty()) return false;

    std::string url;
    if (!keyOrSection.empty() && keyOrSection[0] == '/') {
        url = server.selectedUri + keyOrSection;
        if (keyOrSection.find('?') != std::string::npos) {
            url += "&X-Plex-Container-Start=" + std::to_string(start) + "&X-Plex-Container-Size=" + std::to_string(size);
        } else {
            url += "?X-Plex-Container-Start=" + std::to_string(start) + "&X-Plex-Container-Size=" + std::to_string(size);
        }
    } else {
        url = server.selectedUri + "/library/sections/" + keyOrSection + "/all"
            + "?X-Plex-Container-Start=" + std::to_string(start)
            + "&X-Plex-Container-Size=" + std::to_string(size);
    }

    auto resp = Network::get(url, getBaseHeaders(server.accessToken));
    if (!resp.success) return false;

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return false;

    cJSON* mc = cJSON_GetObjectItem(root, "MediaContainer");
    if (!mc) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* list = cJSON_GetObjectItem(mc, "Metadata");
    if (!list || !cJSON_IsArray(list)) {
        list = cJSON_GetObjectItem(mc, "Directory");
    }

    if (list && cJSON_IsArray(list)) {
        int count = cJSON_GetArraySize(list);
        for (int i = 0; i < count; i++) {
            cJSON* m = cJSON_GetArrayItem(list, i);
            PlexMediaItem item;

            cJSON* rk = cJSON_GetObjectItem(m, "ratingKey");
            cJSON* k = cJSON_GetObjectItem(m, "key");
            cJSON* title = cJSON_GetObjectItem(m, "title");
            cJSON* summary = cJSON_GetObjectItem(m, "summary");
            cJSON* thumb = cJSON_GetObjectItem(m, "thumb");
            cJSON* year = cJSON_GetObjectItem(m, "year");
            cJSON* index = cJSON_GetObjectItem(m, "index");
            cJSON* type = cJSON_GetObjectItem(m, "type");
            cJSON* parentTitle = cJSON_GetObjectItem(m, "parentTitle");
            cJSON* gpTitle = cJSON_GetObjectItem(m, "grandparentTitle");
            cJSON* viewOffset = cJSON_GetObjectItem(m, "viewOffset");

            if (rk) {
                if (rk->valuestring) item.ratingKey = rk->valuestring;
                else if (cJSON_IsNumber(rk)) item.ratingKey = std::to_string((int64_t)rk->valuedouble);
            }
            if (k) item.key = k->valuestring ? k->valuestring : "";
            if (title) item.title = title->valuestring ? title->valuestring : "Untitled";
            if (summary) item.summary = summary->valuestring ? summary->valuestring : "";
            if (thumb) item.thumbUrl = thumb->valuestring ? thumb->valuestring : "";
            if (year && cJSON_IsNumber(year)) item.year = year->valueint;
            if (index && cJSON_IsNumber(index)) item.index = index->valueint;
            if (parentTitle && parentTitle->valuestring) item.parentTitle = parentTitle->valuestring;
            if (gpTitle && gpTitle->valuestring) item.grandparentTitle = gpTitle->valuestring;
            if (viewOffset) {
                if (cJSON_IsNumber(viewOffset)) item.viewOffsetMs = (int64_t)viewOffset->valuedouble;
                else if (viewOffset->valuestring) item.viewOffsetMs = std::atoll(viewOffset->valuestring);
            }

            std::string typeStr = type && type->valuestring ? type->valuestring : "";
            if (typeStr == "movie") item.type = MediaType::MOVIE;
            else if (typeStr == "show") item.type = MediaType::SHOW;
            else if (typeStr == "season") item.type = MediaType::SEASON;
            else if (typeStr == "episode") item.type = MediaType::EPISODE;
            else if (typeStr == "artist") item.type = MediaType::ARTIST;
            else if (typeStr == "album") item.type = MediaType::ALBUM;
            else if (typeStr == "track") item.type = MediaType::TRACK;

            cJSON* mediaArr = cJSON_GetObjectItem(m, "Media");
            cJSON* dur = cJSON_GetObjectItem(m, "duration");
            if (mediaArr && cJSON_IsArray(mediaArr) && cJSON_GetArraySize(mediaArr) > 0) {
                cJSON* med = cJSON_GetArrayItem(mediaArr, 0);
                if (!dur) dur = cJSON_GetObjectItem(med, "duration");
                cJSON* partArr = cJSON_GetObjectItem(med, "Part");
                if (partArr && cJSON_IsArray(partArr) && cJSON_GetArraySize(partArr) > 0) {
                    cJSON* part = cJSON_GetArrayItem(partArr, 0);
                    if (!dur) dur = cJSON_GetObjectItem(part, "duration");
                    cJSON* pk = cJSON_GetObjectItem(part, "key");
                    cJSON* cont = cJSON_GetObjectItem(part, "container");
                    if (!cont) cont = cJSON_GetObjectItem(med, "container");
                    cJSON* aCodec = cJSON_GetObjectItem(med, "audioCodec");
                    if (pk && pk->valuestring) item.partKey = pk->valuestring;
                    if (cont && cont->valuestring) item.container = cont->valuestring;
                    else if (aCodec && aCodec->valuestring) item.container = aCodec->valuestring;

                    cJSON* streamArr = cJSON_GetObjectItem(part, "Stream");
                    if (streamArr && cJSON_IsArray(streamArr)) {
                        int sCount = cJSON_GetArraySize(streamArr);
                        for (int si = 0; si < sCount; si++) {
                            cJSON* st = cJSON_GetArrayItem(streamArr, si);
                            cJSON* sType = cJSON_GetObjectItem(st, "streamType");
                            if (sType && sType->valueint == 3) { // 3 = Subtitle stream
                                PlexSubtitleTrack sub;
                                cJSON* sId = cJSON_GetObjectItem(st, "id");
                                cJSON* sLang = cJSON_GetObjectItem(st, "language");
                                cJSON* sLangCode = cJSON_GetObjectItem(st, "languageCode");
                                cJSON* sTitle = cJSON_GetObjectItem(st, "displayTitle");
                                if (!sTitle) sTitle = cJSON_GetObjectItem(st, "title");
                                cJSON* sCodec = cJSON_GetObjectItem(st, "codec");
                                cJSON* sSel = cJSON_GetObjectItem(st, "selected");
                                cJSON* sForced = cJSON_GetObjectItem(st, "forced");

                                if (sId && cJSON_IsNumber(sId)) sub.id = sId->valueint;
                                if (sLang && sLang->valuestring) sub.language = sLang->valuestring;
                                if (sLangCode && sLangCode->valuestring) sub.languageCode = sLangCode->valuestring;
                                if (sTitle && sTitle->valuestring) sub.title = sTitle->valuestring;
                                else if (!sub.language.empty()) sub.title = sub.language;
                                else sub.title = "Subtitle " + std::to_string(item.subtitleTracks.size() + 1);
                                if (sCodec && sCodec->valuestring) sub.format = sCodec->valuestring;
                                if (sSel && (sSel->valueint == 1 || (sSel->valuestring && std::string(sSel->valuestring) == "1"))) sub.isSelected = true;
                                if (sForced && (sForced->valueint == 1 || (sForced->valuestring && std::string(sForced->valuestring) == "1"))) sub.isForced = true;

                                item.subtitleTracks.push_back(sub);
                            }
                        }
                    }
                }
            }

            if (dur) {
                if (cJSON_IsNumber(dur)) item.durationMs = (int64_t)dur->valuedouble;
                else if (dur->valuestring) item.durationMs = atoll(dur->valuestring);
                item.duration = std::to_string(item.durationMs);
            }

            outItems.push_back(item);
        }
    }

    cJSON_Delete(root);
    return true;
}

std::string PlexAPI::buildPosterUrl(const PlexServer& server, const std::string& thumbPath, int width, int height) const {
    if (thumbPath.empty() || server.selectedUri.empty()) return "";
    return server.selectedUri + "/photo/:/transcode?width=" + std::to_string(width)
        + "&height=" + std::to_string(height)
        + "&minSize=1&upscale=0&url=" + thumbPath
        + "&X-Plex-Token=" + server.accessToken;
}

std::string PlexAPI::buildTranscodeUrl(const PlexServer& server, const PlexMediaItem& item, const AppConfig& config) const {
    if (server.selectedUri.empty()) return "";

    std::string pathKey = item.key.empty() ? ("/library/metadata/" + item.ratingKey) : item.key;
    std::string encodedKey = urlEncode(pathKey);
    std::string clientId = m_clientIdentifier.empty() ? "Plex3DS-Client-001" : m_clientIdentifier;

    if (item.type == MediaType::TRACK) {
        std::string contLower = item.container;
        std::transform(contLower.begin(), contLower.end(), contLower.begin(), ::tolower);
        std::string pkLower = item.partKey;
        std::transform(pkLower.begin(), pkLower.end(), pkLower.begin(), ::tolower);
        bool isMp3 = (contLower == "mp3" || contLower == "audio/mp3" || contLower == "mpeg" ||
                     (pkLower.length() >= 4 && pkLower.substr(pkLower.length() - 4) == ".mp3"));
        if (!item.partKey.empty() && isMp3) {
            return server.selectedUri + item.partKey + "?X-Plex-Token=" + server.accessToken;
        }

        uint64_t sessionTime = 0;
#ifdef __3DS__
        sessionTime = osGetTime();
#else
        sessionTime = (uint64_t)time(nullptr);
#endif
        std::string sessionId = "3ds-audio-" + std::to_string(sessionTime) + "-" + std::to_string(rand() % 10000);
        std::string profileExtra = "add-transcode-target(type=musicProfile&context=streaming&protocol=http&container=mp3&audioCodec=mp3)";
        std::string profileExtraEncoded = "add-transcode-target(type%3DmusicProfile%26context%3Dstreaming%26protocol%3Dhttp%26container%3Dmp3%26audioCodec%3Dmp3)";

        std::string dec = server.selectedUri + "/music/:/transcode/universal/decision"
            + "?path=" + encodedKey
            + "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0&audioQuality=60"
            + "&location=lan&session=" + sessionId
            + "&X-Plex-Token=" + server.accessToken
            + "&X-Plex-Client-Identifier=" + clientId
            + "&X-Plex-Client-Profile-Name=Generic"
            + "&X-Plex-Client-Profile-Extra=" + profileExtraEncoded;

        auto decHeaders = getBaseHeaders(server.accessToken);
        decHeaders["X-Plex-Client-Profile-Name"] = "Generic";
        decHeaders["X-Plex-Client-Profile-Extra"] = profileExtra;
        Network::HttpResponse decRes = Network::get(dec, decHeaders);
        if (!decRes.success || decRes.statusCode >= 400) {
            return "";
        }

        std::string streamUrl = server.selectedUri + "/music/:/transcode/universal/start.mp3"
            + "?path=" + encodedKey
            + "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0&audioQuality=60"
            + "&location=lan&session=" + sessionId
            + "&X-Plex-Token=" + server.accessToken
            + "&X-Plex-Client-Identifier=" + clientId
            + "&X-Plex-Client-Profile-Name=Generic"
            + "&X-Plex-Client-Profile-Extra=" + profileExtraEncoded;
        return streamUrl;
    }

    uint64_t sessionTime = 0;
#ifdef __3DS__
    sessionTime = osGetTime();
#else
    sessionTime = (uint64_t)time(nullptr);
#endif
    std::string sessionId = "3ds-video-" + std::to_string(sessionTime) + "-" + std::to_string(rand() % 10000);

    std::string subParam = "subtitles=none";
    if (config.subtitlesEnabled) {
        subParam = "subtitles=burn";
        if (item.selectedSubtitleIdx >= 0 && item.selectedSubtitleIdx < (int)item.subtitleTracks.size()) {
            subParam += "&subtitleStreamID=" + std::to_string(item.subtitleTracks[item.selectedSubtitleIdx].id);
        }
    }

    // Video Transcode Decision
    std::string dec = server.selectedUri + "/video/:/transcode/universal/decision"
        + "?path=" + encodedKey
        + "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0"
        + "&videoQuality=60"
        + "&videoBitrate=1000"
        + "&videoResolution=400x240"
        + "&videoCodec=h264&audioCodec=aac"
        + "&location=lan"
        + "&session=" + sessionId
        + (!subParam.empty() ? ("&" + subParam) : "")
        + "&X-Plex-Token=" + server.accessToken
        + "&X-Plex-Client-Identifier=" + clientId
        + "&X-Plex-Client-Profile-Name=Generic"
        + "&X-Plex-Client-Profile-Extra=add-transcode-target(type%3DvideoProfile%26context%3Dstreaming%26protocol%3Dhttp%26container%3Dmkv%26videoCodec%3Dh264%26audioCodec%3Daac)";

    auto decHeaders = getBaseHeaders(server.accessToken);
    decHeaders["X-Plex-Client-Profile-Name"] = "Generic";
    decHeaders["X-Plex-Client-Profile-Extra"] = "add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mkv&videoCodec=h264&audioCodec=aac)";
    Network::HttpResponse decRes = Network::get(dec, decHeaders);
    if (!decRes.success || decRes.statusCode >= 400) {
        return "";
    }

    // Return the Matroska MKV stream URL with full profile augmentation
    std::string streamUrl = server.selectedUri + "/video/:/transcode/universal/start.mkv"
        + "?path=" + encodedKey
        + "&mediaIndex=0&partIndex=0&protocol=http&fastSeek=1&directPlay=0&directStream=0"
        + "&videoQuality=60"
        + "&videoBitrate=1000"
        + "&videoResolution=400x240"
        + "&videoCodec=h264&audioCodec=aac"
        + "&location=lan"
        + "&session=" + sessionId
        + (!subParam.empty() ? ("&" + subParam) : "")
        + "&X-Plex-Token=" + server.accessToken
        + "&X-Plex-Client-Identifier=" + clientId
        + "&X-Plex-Client-Profile-Name=Generic"
        + "&X-Plex-Client-Profile-Extra=add-transcode-target(type%3DvideoProfile%26context%3Dstreaming%26protocol%3Dhttp%26container%3Dmkv%26videoCodec%3Dh264%26audioCodec%3Daac)";

    return streamUrl;
}

void PlexAPI::reportTimeline(const PlexServer& server, const PlexMediaItem& item, int64_t timeMs, const std::string& state) {
    if (server.selectedUri.empty() || item.ratingKey.empty() || item.isOffline) return;

    std::string url = server.selectedUri + "/:/timeline?ratingKey=" + item.ratingKey
        + "&key=" + urlEncode(item.key.empty() ? ("/library/metadata/" + item.ratingKey) : item.key)
        + "&state=" + state
        + "&time=" + std::to_string(timeMs)
        + "&duration=" + std::to_string(item.durationMs)
        + "&X-Plex-Token=" + server.accessToken
        + "&X-Plex-Client-Identifier=" + m_clientIdentifier;

    auto headers = getBaseHeaders(server.accessToken);
    Network::get(url, headers);
}
