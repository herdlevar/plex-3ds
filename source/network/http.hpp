#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>

namespace Network {

bool init();
void exit();

struct HttpResponse {
    int statusCode = 0;
    std::string body;
    bool success = false;
};

HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers = {});
HttpResponse post(const std::string& url, const std::string& jsonBody, const std::map<std::string, std::string>& headers = {});

// Stream download helper for images / media buffers
bool downloadFile(const std::string& url, const std::string& destinationPath, const std::map<std::string, std::string>& headers = {});

} // namespace Network
