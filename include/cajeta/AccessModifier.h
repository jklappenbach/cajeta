//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

namespace cajeta {
    enum AccessModifier { PACKAGE = 0x01, PUBLIC = 0x02, PRIVATE = 0x04, PROTECTED = 0x08, STATIC = 0x10, FINAL = 0x20, SYNCHRONIZED = 0x40 };
}