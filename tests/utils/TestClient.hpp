#pragma once

#include <string>
#include <map>
#include <vector>
#include <curl/curl.h>

namespace orbit::test {

struct TestResponse {
    long status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

inline size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

inline size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    std::string header(buffer, size * nitems);
    size_t colon_pos = header.find(':');
    if (colon_pos != std::string::npos) {
        std::string key = header.substr(0, colon_pos);
        std::string value = header.substr(colon_pos + 1);
        
        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \r\n\t"));
        value.erase(value.find_last_not_of(" \r\n\t") + 1);
        
        ((std::map<std::string, std::string>*)userdata)->insert({key, value});
    }
    return nitems * size;
}

inline TestResponse send_request(const std::string& method, const std::string& url, const std::string& data = "", const std::vector<std::string>& req_headers = {}) {
    TestResponse response;
    CURL* curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        
        struct curl_slist* headers = NULL;
        for (const auto& h : req_headers) {
            headers = curl_slist_append(headers, h.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        if (!data.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, data.size());
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
        
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
        } else {
            response.body = "CURL Error: " + std::string(curl_easy_strerror(res));
            response.status_code = -1;
        }

        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }
    return response;
}

} // namespace orbit::test
