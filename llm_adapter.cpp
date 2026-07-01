#include "llm_adapter.h"
#include <curl/curl.h>
#include <cstdlib>
#include <sstream>
#include <iostream>

namespace {

size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    std::string *s = static_cast<std::string *>(userp);
    s->append(static_cast<char *>(contents), total);
    return total;
}

std::string extractJsonStringValue(const std::string &json,
                                   const std::string &key)
{
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + pattern.size());
    if (colonPos == std::string::npos) return "";

    size_t quoteStart = json.find('"', colonPos + 1);
    if (quoteStart == std::string::npos) return "";

    size_t quoteEnd = quoteStart + 1;
    while (quoteEnd < json.size())
    {
        if (json[quoteEnd] == '"' && json[quoteEnd - 1] != '\\')
            break;
        quoteEnd++;
    }
    if (quoteEnd >= json.size()) return "";
    return json.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
}

} // namespace

LLMAdapter::LLMAdapter()
{
    // Initialize with GEMINI_API_KEY environment variable if present
    const char *k = std::getenv("GEMINI_API_KEY");
    if (k)
        apiKey = k;
}

std::string LLMAdapter::generate(const std::string &prompt, const std::string &model)
{
    // 1. Read your Gemini Key from your environment variables if not already set in constructor
    std::string activeKey = apiKey;
    if (activeKey.empty()) {
        const char* apiKeyEnv = std::getenv("GEMINI_API_KEY");
        if (apiKeyEnv) {
            activeKey = apiKeyEnv;
        }
    }
    
    if (activeKey.empty())
    {
        return "{\"error\": \"GEMINI_API_KEY environment variable not set.\"}";
    }

    CURL *curl = curl_easy_init();
    if (!curl) return "{\"error\": \"Failed to initialize cURL\"}";

    std::string response;
    
    // 2. Map standard models over to Google models (e.g., gemini-1.5-flash or gemini-1.5-pro)
    // 2. Map standard models over to Google models (using the stable v1 endpoint)
std::string targetModel = (model == "gpt-3.5-turbo") ? "gemini-2.5-flash" : model;
std::string url = "https://generativelanguage.googleapis.com/v1/models/" 
                + targetModel + ":generateContent?key=" + activeKey;

    // 3. Re-structure raw payload configuration to Gemini's JSON schema safely using std::ostringstream
    std::ostringstream payload;
    payload << "{\"contents\": [{\"parts\":[{\"text\":\"";
    
    // Escape special string bounds for JSON compatibility
    for (char ch : prompt)
    {
        if (ch == '\\')      payload << "\\\\";
        else if (ch == '"')  payload << "\\\"";
        else if (ch == '\n') payload << "\\n";
        else if (ch == '\r') payload << "\\r";
        else if (ch == '\t') payload << "\\t";
        else payload << ch;
    }
    payload << "\"}]}]}";
    std::string data = payload.str();

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "coredb-llm-adapter/1.0");

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
    {
        return "{\"error\": \"curl_easy_perform failed\"}";
    }

    return response;
}