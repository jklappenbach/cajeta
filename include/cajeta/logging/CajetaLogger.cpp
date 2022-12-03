//
// Created by James Klappenbach on 11/6/22.
//

#include "CajetaLogger.h"
#include <glog/logging.h>
#include <glog/log_severity.h>
#include <cajeta/compile/CajetaModule.h>


namespace cajeta {
    void logInfo(antlr4::ParserRuleContext* ctx,
        string sourcePath,
        string errorId,
        string message) {
        antlr4::Token* token = ctx->getStart();
        LOG(INFO) << sourcePath << "[" << token->getLine() << ":" << token->getStartIndex() <<
                  "]\nError " << errorId << ": " << message << "\n" << token->getText();
    }

    void logInfo(CajetaModule* cajetaModule,
        AbstractSyntaxNode* node,
        Exception& e) {
        LOG(INFO) << cajetaModule->getSourcePath() << "[" << node->getSourceLine() << "," <<
                  node->getSourceColumn() << "]\nError " << e.getErrorId() << ": " << e.getMessage() << "\n"
                  << node->getSourceText();
    }

    void logWarning(antlr4::ParserRuleContext* ctx,
        string sourcePath,
        string errorId,
        string message) {
        antlr4::Token* token = ctx->getStart();
        LOG(WARNING) << sourcePath << "[" << token->getLine() << ":" << token->getStartIndex() <<
                     "]\nError " << errorId << ": " << message << "\n" << token->getText();
    }

    void logWarning(CajetaModule* cajetaModule,
        AbstractSyntaxNode* node,
        Exception& e) {
        LOG(WARNING) << cajetaModule->getSourcePath() << "[" << node->getSourceLine() << "," <<
                     node->getSourceColumn() << "]\nError " << e.getErrorId() << ": " << e.getMessage() << "\n"
                     << node->getSourceText();
    }

    void logError(antlr4::ParserRuleContext* ctx,
        string sourcePath,
        string errorId,
        string message) {
        antlr4::Token* token = ctx->getStart();
        LOG(ERROR) << sourcePath << "[" << token->getLine() << ":" << token->getStartIndex() <<
                   "]\nError " << errorId << ": " << message << "\n" << token->getText();
    }

    void logError(CajetaModule* cajetaModule,
        AbstractSyntaxNode* node,
        Exception& e) {
        LOG(ERROR) << cajetaModule->getSourcePath() << "[" << node->getSourceLine() << "," <<
                   node->getSourceColumn() << "]\nError " << e.getErrorId() << ": " << e.getMessage() << "\n"
                   << node->getSourceText();
    }

    void logFatal(antlr4::ParserRuleContext* ctx,
        string sourcePath,
        string errorId,
        string message) {
        antlr4::Token* token = ctx->getStart();
        LOG(FATAL) << sourcePath << "[" << token->getLine() << ":" << token->getStartIndex() <<
                   "]\nError " << errorId << ": " << message << "\n" << token->getText();
    }

    void logFatal(CajetaModule* cajetaModule,
        AbstractSyntaxNode* node,
        Exception& e) {
        LOG(FATAL) << cajetaModule->getSourcePath() << "[" << node->getSourceLine() << "," <<
                   node->getSourceColumn() << "]\nError " << e.getErrorId() << ": " << e.getMessage() << "\n"
                   << node->getSourceText();
    }

    void CajetaLogger::log(LoggingLevel level,
        antlr4::ParserRuleContext* ctx,
        string sourcePath,
        string errorId,
        string message) {
        switch (level) {
            case INFO:
                logInfo(ctx, sourcePath, errorId, message);
                break;
            case WARNING:
                logWarning(ctx, sourcePath, errorId, message);
                break;
            case ERROR:
                logError(ctx, sourcePath, errorId, message);
                break;
            case FATAL:
                logFatal(ctx, sourcePath, errorId, message);
                break;
        }
    }

    void CajetaLogger::log(LoggingLevel level,
        CajetaModule* cajetaModule,
        AbstractSyntaxNode* node,
        Exception& e) {
        switch (level) {
            case INFO:
                logInfo(cajetaModule, node, e);
                break;
            case WARNING:
                logWarning(cajetaModule, node, e);
                break;
            case ERROR:
                logError(cajetaModule, node, e);
                break;
            case FATAL:
                logFatal(cajetaModule, node, e);
                break;
        }
    }
} // cajeta