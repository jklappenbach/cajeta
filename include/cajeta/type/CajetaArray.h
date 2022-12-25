//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaStructure.h"
#include "cajeta/field/Field.h"
#include "cajeta/method/Method.h"
#include "Modifiable.h"
#include <stdio.h>
#include <vector>

namespace cajeta {
    class CajetaArray : public CajetaStructure {
    private:
        int dimension;
        CajetaType* elementType;
        std::vector<llvm::Constant*> dimensions;
    public:
        static string ARRAY_FIELD_NAME;

        CajetaArray(CajetaModule* module, CajetaType* elementType, int dimension);

        int getStructType() override { return STRUCT_TYPE_ARRAY; }

        /**
         * Returns a list of Constants, each element will hold the size of the requested array dimension
         * @return A list of Constant* to dimension sizes
         */
        std::vector<llvm::Constant*>& getDimensions() { return dimensions; }

        /**
         * The underlying type of the element in the array
         * @return
         */
        CajetaType* getElementType() { return elementType; }

        /**
         * The number of dimensions in the array
         * @return An integer
         */
        const int getDimension() { return dimension; }
    };
}