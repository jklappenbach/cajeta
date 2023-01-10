//
// Created by James Klappenbach on 11/18/23.
//

#pragma once

#include "Exception.h"
#include <string>
#include <filesystem>

using namespace std;

namespace cajeta {

    #define FILE_NOT_FOUND_MESSAGE "File not found. Provided path: %s, PWD: %s"

    class FileNotFoundException : public Exception {
        string path;
        filesystem::path pwd;
    public:
        FileNotFoundException(string& path) {
            char buffer[2048];
            pwd = std::filesystem::current_path();
            this->path = path;
            ::snprintf(buffer, 2048, FILE_NOT_FOUND_MESSAGE, path.c_str(), pwd.c_str());
            this->message = string(buffer);
        }
    };

} // code