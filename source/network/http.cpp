#include "http.hpp"
#include "types.hpp"

#ifdef __3DS__
#include <3ds.h>
#endif

#include <curl/curl.h>
#include <malloc.h>

namespace Network {

static uint32_t* socBuffer = nullptr;
static const size_t SOC_BUFFERSIZE = 0x80000; // 512KB linear memory for smooth network streaming
static bool s_networkInitialized = false;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::string* mem = static_cast<std::string*>(userp);
    mem->append(static_cast<char*>(contents), realsize);
    return realsize;
}

static size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, void* stream) {
    size_t written = fwrite(ptr, size, nmemb, (FILE*)stream);
    return written;
}

static int XferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    if (g_appExiting.load()) return 1;
    return 0;
}

bool init() {
    if (s_networkInitialized) return true;
#ifdef __3DS__
    socBuffer = (uint32_t*)memalign(0x1000, SOC_BUFFERSIZE);
    if (!socBuffer) {
        return false;
    }
    Result ret = socInit(socBuffer, SOC_BUFFERSIZE);
    if (R_FAILED(ret)) {
        free(socBuffer);
        socBuffer = nullptr;
        return false;
    }
#endif
    curl_global_init(CURL_GLOBAL_ALL);
    s_networkInitialized = true;
    return true;
}

void exit() {
    if (!s_networkInitialized) return;
    curl_global_cleanup();
#ifdef __3DS__
    if (socBuffer) {
        socExit();
        free(socBuffer);
        socBuffer = nullptr;
    }
#endif
    s_networkInitialized = false;
}

static HttpResponse performCurlRequest(CURL* curl, const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    struct curl_slist* chunk = nullptr;

    for (const auto& pair : headers) {
        std::string headerLine = pair.first + ": " + pair.second;
        chunk = curl_slist_append(chunk, headerLine.c_str());
    }

    if (chunk) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    }

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    // Enable CA certificate verification if bundle is provided on SD, else allow fallback
    static bool s_hasCaBundleChecked = false;
    static bool s_hasCaBundle = false;
    static const char* CA_BUNDLE_PATH = "sdmc:/3ds/plex-3ds/cacert.pem";
    if (!s_hasCaBundleChecked) {
        FILE* caF = fopen(CA_BUNDLE_PATH, "rb");
        if (caF) {
            fclose(caF);
            s_hasCaBundle = true;
        }
        s_hasCaBundleChecked = true;
    }

    if (s_hasCaBundle) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // Fallback for 3DS without root CA bundle
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK) {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        response.statusCode = static_cast<int>(http_code);
        response.success = (response.statusCode >= 200 && response.statusCode < 300);
    } else {
        response.success = false;
        response.statusCode = -1;
    }

    if (chunk) {
        curl_slist_free_all(chunk);
    }

    return response;
}

HttpResponse get(const std::string& url, const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

    response = performCurlRequest(curl, headers);
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse post(const std::string& url, const std::string& jsonBody, const std::map<std::string, std::string>& headers) {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) return response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());

    std::map<std::string, std::string> allHeaders = headers;
    if (allHeaders.find("Content-Type") == allHeaders.end()) {
        allHeaders["Content-Type"] = "application/json";
    }

    response = performCurlRequest(curl, allHeaders);
    curl_easy_cleanup(curl);
    return response;
}

bool downloadFile(const std::string& url, const std::string& destinationPath, const std::map<std::string, std::string>& headers) {
    FILE* fp = fopen(destinationPath.c_str(), "wb");
    if (!fp) return false;

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return false;
    }

    struct curl_slist* chunk = nullptr;
    for (const auto& pair : headers) {
        std::string headerLine = pair.first + ": " + pair.second;
        chunk = curl_slist_append(chunk, headerLine.c_str());
    }
    if (chunk) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    if (chunk) curl_slist_free_all(chunk);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

} // namespace Network
