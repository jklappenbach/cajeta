//
// Created by James Klappenbach on 2/13/22.
//
#include "cajeta/parse/CajetaParserIRListener.h"
#include "cajeta/type/Modifiable.h"

using namespace cajeta;

//TypeDefinition* TypeDefinition::fromContext(CajetaParser::TypeTypeOrVoidContext* ctxTypeOrVoid, cajeta::ParseContext* ctxParse) {
//    string name = ctxTypeOrVoid->getText();
//    TypeDefinition* result = NativeTypeDefinition::fromName(name, ctxParse);
//    return result;
//}
//
//TypeDefinition* TypeDefinition::fromName(string name, ParseContext* ctxParse) {
//    TypeDefinition* result = NativeTypeDefinition::fromName(name, ctxParse);
//    //TODO: More advanced
//    return result;
//}
//
//TypeDefinition* NativeTypeDefinition::fromName(string name, ParseContext* ctxParse) {
//    TypeDefinition* typeDefinition = NULL;
//    if (name == "void") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getVoidTy());
//    } else if (name == "boolean") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt1Ty());
//    } else if (name == "char") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt8Ty());
//    } else if (name == "int16") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt16Ty());
//    } else if (name == "uint16") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt16Ty());
//    } else if (name == "int32") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt32Ty());
//    } else if (name == "uint32") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt32Ty());
//    } else if (name == "int64") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt64Ty());
//    } else if (name == "uint64") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt64Ty());
//    } else if (name == "int128") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt128Ty());
//    } else if (name == "uint128") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getInt128Ty());
//    } else if (name == "float16") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getHalfTy());
//    } else if (name == "float32") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getFloatTy());
//    } else if (name == "float64") {
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getDoubleTy());
//    } else if (name == "float128") {
//        // TODO: Need to flag an error here, or at least figure out how to customize the output.  Do we need a 128b float?  I think so.
//        typeDefinition = new NativeTypeDefinition(name, ctxParse->module, ctxParse->builder->getDoubleTy());
//    }
//
//    return typeDefinition;
//}


