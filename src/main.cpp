#include <cstdlib>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdio>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

std::string read_file(const std::string& file_path) {
    std::ifstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + file_path);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void write_file(const std::string& file_path, const std::string& content) {
    std::ofstream file(file_path);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file for writing: " + file_path);
    }
    file << content;
    if (!file.good()) {
        throw std::runtime_error("Error writing to file: " + file_path);
    }
}

std::string execute_bash(const std::string& command) {
    // Redirect stderr to stdout for capturing both
    std::string cmd = command + " 2>&1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        throw std::runtime_error("Failed to execute command: " + command);
    }
    
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    
    int status = pclose(pipe);
    return result;
}

int main(int argc, char* argv[]) {
    if (argc < 3 || std::string(argv[1]) != "-p") {
        std::cerr << "Expected first argument to be '-p'" << std::endl;
        return 1;
    }

    std::string prompt = argv[2];

    if (prompt.empty()) {
        std::cerr << "Prompt must not be empty" << std::endl;
        return 1;
    }

    const char* api_key_env = std::getenv("OPENROUTER_API_KEY");
    const char* base_url_env = std::getenv("OPENROUTER_BASE_URL");

    std::string api_key = api_key_env ? api_key_env : "";
    std::string base_url = base_url_env ? base_url_env : "https://openrouter.ai/api/v1";

    if (api_key.empty()) {
        std::cerr << "OPENROUTER_API_KEY is not set" << std::endl;
        return 1;
    }

    // Initialize conversation history
    json messages = json::array({
        {{"role", "user"}, {"content", prompt}}
    });

    // Tool definition
    json tools = json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "Read"},
                {"description", "Read and return the contents of a file"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"file_path", {
                            {"type", "string"},
                            {"description", "The path to the file to read"}
                        }}
                    }},
                    {"required", json::array({"file_path"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "Write"},
                {"description", "Write content to a file"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"file_path", {
                            {"type", "string"},
                            {"description", "The path of the file to write to"}
                        }},
                        {"content", {
                            {"type", "string"},
                            {"description", "The content to write to the file"}
                        }}
                    }},
                    {"required", json::array({"file_path", "content"})}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "Bash"},
                {"description", "Execute a shell command"},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"command", {
                            {"type", "string"},
                            {"description", "The command to execute"}
                        }}
                    }},
                    {"required", json::array({"command"})}
                }}
            }}
        }
    });

    // Agent loop
    while (true) {
        json request_body = {
            {"model", "anthropic/claude-haiku-4.5"},
            {"messages", messages},
            {"tools", tools}
        };

        cpr::Response response = cpr::Post(
            cpr::Url{base_url + "/chat/completions"},
            cpr::Header{
                {"Authorization", "Bearer " + api_key},
                {"Content-Type", "application/json"}
            },
            cpr::Body{request_body.dump()}
        );

        if (response.status_code != 200) {
            std::cerr << "HTTP error: " << response.status_code << std::endl;
            return 1;
        }

        json result = json::parse(response.text);

        if (!result.contains("choices") || result["choices"].empty()) {
            std::cerr << "No choices in response" << std::endl;
            return 1;
        }

        auto& choice = result["choices"][0];
        auto& message = choice["message"];
        std::string finish_reason = choice["finish_reason"];

        // Add assistant's response to messages
        messages.push_back(message);

        // Check if there are tool calls to process
        if (message.contains("tool_calls") && !message["tool_calls"].empty()) {
            // Process each tool call
            for (auto& tool_call : message["tool_calls"]) {
                std::string tool_call_id = tool_call["id"];
                std::string function_name = tool_call["function"]["name"];

                std::string tool_result;

                if (function_name == "Read") {
                    // Parse the arguments JSON string
                    json arguments = json::parse(tool_call["function"]["arguments"].get<std::string>());
                    std::string file_path = arguments["file_path"];

                    // Read the file contents
                    try {
                        tool_result = read_file(file_path);
                    } catch (const std::exception& e) {
                        tool_result = std::string("Error reading file: ") + e.what();
                        std::cerr << tool_result << std::endl;
                    }
                } else if (function_name == "Write") {
                    // Parse the arguments JSON string
                    json arguments = json::parse(tool_call["function"]["arguments"].get<std::string>());
                    std::string file_path = arguments["file_path"];
                    std::string content = arguments["content"];

                    // Write the content to file
                    try {
                        write_file(file_path, content);
                        tool_result = "File written successfully";
                    } catch (const std::exception& e) {
                        tool_result = std::string("Error writing file: ") + e.what();
                        std::cerr << tool_result << std::endl;
                    }
                } else if (function_name == "Bash") {
                    // Parse the arguments JSON string
                    json arguments = json::parse(tool_call["function"]["arguments"].get<std::string>());
                    std::string command = arguments["command"];

                    // Execute the bash command
                    try {
                        tool_result = execute_bash(command);
                    } catch (const std::exception& e) {
                        tool_result = std::string("Error executing command: ") + e.what();
                        std::cerr << tool_result << std::endl;
                    }
                }

                // Add tool result to messages
                messages.push_back({
                    {"role", "tool"},
                    {"tool_call_id", tool_call_id},
                    {"content", tool_result}
                });
            }
        } else {
            // No tool calls, we're done - print the final response
            if (!message["content"].is_null()) {
                std::cout << message["content"].get<std::string>();
            }
            break;
        }
    }

    return 0;
}
