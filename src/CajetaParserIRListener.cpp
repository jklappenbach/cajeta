//
// Created by James Klappenbach on 2/13/22.
//
#include "cajeta/compile/CajetaParserIRListener.h"
#include "cajeta/type/Modifiable.h"

using namespace cajeta;

//TypeDefinition* TypeDefinition::fromContext(CajetaParser::TypeTypeOrVoidContext* ctxTypeOrVoid, cajeta::ParseContext* ctxParse) {
//    string canonical = ctxTypeOrVoid->getText();
//    TypeDefinition* result = NativeTypeDefinition::fromName(canonical, ctxParse);
//    return result;
//}
//
//TypeDefinition* TypeDefinition::fromName(string canonical, ParseContext* ctxParse) {
//    TypeDefinition* result = NativeTypeDefinition::fromName(canonical, ctxParse);
//    //TODO: More advanced
//    return result;
//}
//
//TypeDefinition* NativeTypeDefinition::fromName(string canonical, ParseContext* ctxParse) {
//    TypeDefinition* typeDefinition = NULL;
//    if (canonical == "void") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getVoidTy());
//    } else if (canonical == "boolean") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt1Ty());
//    } else if (canonical == "char") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt8Ty());
//    } else if (canonical == "int16") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt16Ty());
//    } else if (canonical == "uint16") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt16Ty());
//    } else if (canonical == "int32") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt32Ty());
//    } else if (canonical == "uint32") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt32Ty());
//    } else if (canonical == "int64") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt64Ty());
//    } else if (canonical == "uint64") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt64Ty());
//    } else if (canonical == "int128") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt128Ty());
//    } else if (canonical == "uint128") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getInt128Ty());
//    } else if (canonical == "float16") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getHalfTy());
//    } else if (canonical == "float32") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getFloatTy());
//    } else if (canonical == "float64") {
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getDoubleTy());
//    } else if (canonical == "float128") {
//        // TODO: Need to flag an error here, or at least figure out how to customize the output.  Do we need a 128b float?  I think so.
//        typeDefinition = new NativeTypeDefinition(canonical, ctxParse->module, ctxParse->builder->getDoubleTy());
//    }
//
//    return typeDefinition;
//}


