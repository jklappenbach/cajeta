// Shared annotation-instance parsing: builds a typed AnnotationInstance (name +
// captured argument values) from an ANTLR annotation context, so the class-body
// walk and the formal-parameter path capture identical argument shapes.

#pragma once

#include "CajetaParser.h"
#include "../type/Annotatable.h"

namespace cajeta {

    // Builds an AnnotationInstance from `ann`. Walks elementValuePairs, or a single
    // bare elementValue (the unnamed form, stored as name="" and read as "value");
    // array initializers map to *List kinds. Null when `ann` yields no name.
    AnnotationInstancePtr parseAnnotationInstance(CajetaParser::AnnotationContext* ann);

} // namespace cajeta
