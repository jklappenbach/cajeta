//
// Created by James Klappenbach on 11/6/22.
//

#include "CajetaLogger.h"
#include <glog/logging.h>
#include <glog/log_severity.h>


namespace cajeta {
    namespace {
    void logAt(google::LogSeverity severity, antlr4::ParserRuleContext* ctx,
        const string& sourcePath, const string& errorId, const string& message) {
        antlr4::Token* token = ctx->getStart();
        google::LogMessage(__FILE__, __LINE__, severity).stream()
            << sourcePath << "[" << token->getLine() << ":"
            << token->getStartIndex() << "]\nError " << errorId << ": "
            << message << "\n" << token->getText();
    }
    } // namespace

    void CajetaLogger::log(LoggingLevel level,
        antlr4::ParserRuleContext* ctx,
        string sourcePath,
        string errorId,
        string message) {
        switch (level) {
            case INFO:
                logAt(google::GLOG_INFO, ctx, sourcePath, errorId, message);
                break;
            case WARNING:
                logAt(google::GLOG_WARNING, ctx, sourcePath, errorId, message);
                break;
            case ERROR:
                logAt(google::GLOG_ERROR, ctx, sourcePath, errorId, message);
                break;
            case FATAL:
                logAt(google::GLOG_FATAL, ctx, sourcePath, errorId, message);
                break;
        }
    }
} // code
