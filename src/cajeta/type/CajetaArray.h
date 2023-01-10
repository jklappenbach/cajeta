//
// Created by James Klappenbach on 10/2/22.
//

#pragma once

#include "CajetaClass.h"
#include "cajeta/field/Field.h"
#include "cajeta/method/Method.h"
#include "Modifiable.h"
#include <stdio.h>
#include <vector>

namespace cajeta {
    class CajetaArray : public CajetaClass {
    private:
        int dimension;
        vector<long> dimensions;
        CajetaTypePtr elementType;
        vector<llvm::Constant*> llvmDimensions;
    public:
        static string ARRAY_FIELD_NAME;

        /**
         * Constructor for an array "reference", which will refer to heap allocated memory
         * @param module
         * @param elementType
         * @param dimension
         */
        CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType, int dimension);

        /**
         * Constructor for a stack array, or one used for structures.  Here, the size of each dimension is provided
         * @param module
         * @param elementType
         * @param dimension
         * @param dimensions
         */
        CajetaArray(CajetaModulePtr module, CajetaTypePtr elementType, vector<long> dimensions);


        CajetaTypeFlags getTypeFlags() override { return ARRAY_TYPE_ID; }

        /**
         * Returns a list of Constants, each element will hold the size of the requested array dimension
         * @return A list of Constant* to dimension sizes
         */
        std::vector<llvm::Constant*>& getDimensions() { return llvmDimensions; }

        /**
         * The underlying type of the element in the array
         * @return
         */
        CajetaTypePtr getElementType() { return elementType; }

        /**
         * The number of llvmDimensions in the array
         * @return An integer
         */
        const int getDimension() { return dimension; }

        llvm::Value* getElement();
    };
    typedef shared_ptr<CajetaArray> CajetaArrayPtr;
}