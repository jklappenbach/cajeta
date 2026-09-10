// Exception for a variable that could not be assigned, carrying the name and cause.

#pragma once

#include "Exception.h"

namespace cajeta {

    class VariableAssignmentException : public Exception {
    private:
        string variableName;
        string variableType;
        string cause;
    public:
        VariableAssignmentException(string variableName, string variableType, string cause, string errorId) {
            this->variableName = variableName;
            this->variableType = variableType;
            this->cause = cause;
            this->errorId = errorId;
            this->message = "Variable '" + variableName + "' could not be assigned: " + cause;
        }
    };

} // code