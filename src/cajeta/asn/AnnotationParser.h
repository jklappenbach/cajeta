//
// Shared annotation-instance parsing (REFL-6b).
//
// Builds a typed AnnotationInstance (name + captured argument values) from an
// ANTLR annotation context. Extracted from CajetaLlvmVisitor so the formal-
// parameter parse path (FormalParameter::fromContext) can capture parameter
// annotation ARGUMENTS too — previously parameters recorded annotation names
// only, leaving their reflective #AnnotationDesc rows with argCount 0.
//
// The class-body walk (class/field/method/constructor annotations) and the
// parameter path now share this one implementation, so all owners capture the
// same argument shapes (scalars, class literals, and *List array forms).
//

#pragma once

#include "CajetaParser.h"
#include "../type/Annotatable.h"

namespace cajeta {

    // Build an AnnotationInstance from an ANTLR annotation context. Walks
    // elementValuePairs (`name = value, ...`) or a single bare elementValue
    // (the unnamed-arg form, stored with name="" and looked up as "value").
    // Array initializers map to *List kinds (dominant-kind rule). Returns
    // nullptr when `ann` is null or yields no resolvable name.
    AnnotationInstancePtr parseAnnotationInstance(CajetaParser::AnnotationContext* ann);

} // namespace cajeta
