#pragma once

#include <string>

class LLMAdapter {
public:
    LLMAdapter();

    // Generate a completion for the given prompt. Returns raw JSON response
    // from the provider (caller may parse or display as needed).
    std::string generate(const std::string &prompt,
                         const std::string &model = "gpt-3.5-turbo");

private:
    std::string apiKey;
};
