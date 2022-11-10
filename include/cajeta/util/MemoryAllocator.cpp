//
// Created by James Klappenbach on 11/9/22.
//

#include "MemoryAllocator.h"

namespace cajeta {
    std::mutex MemoryAllocator::mutex;
    MemoryAllocator* MemoryAllocator::theInstance;

    MemoryAllocator* MemoryAllocator::getInstance(CajetaModule* module) {
        mutex.lock();
        if (!theInstance) {
            theInstance = new MemoryAllocator(module);
        }
        mutex.unlock();
        return theInstance;
    }
}
