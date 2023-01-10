//
// Created by James Klappenbach on 11/6/22.
//

#pragma once

#include <string>

using namespace std;

namespace cajeta {

    class Exception {
    protected:
        string message;
        string errorId;
    public:
        Exception() { }

        Exception(string message, string errorId) {
            this->message = message;
            this->errorId = errorId;
        }

        string getMessage() { return message; }

        string getErrorId() { return errorId; }
    };

} // cajeta