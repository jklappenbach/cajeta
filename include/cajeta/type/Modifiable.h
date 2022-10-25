//
// Created by James Klappenbach on 2/19/22.
//

#pragma once
#include <set>
#include <string>

using namespace std;

namespace cajeta {
    enum Modifier { NONE = 0x00, PACKAGE = 0x01, PUBLIC = 0x02, PRIVATE = 0x04, PROTECTED = 0x08, STATIC = 0x10, FINAL = 0x20, SYNCHRONIZED = 0x40 };

    class Modifiable {
    private:
        set<Modifier> modifiers;
    public:
        Modifiable() { }
        Modifiable(set<Modifier>& modifiers) {
            this->modifiers.insert(modifiers.begin(), modifiers.end());
        }
        void addModifier(Modifier modifier) {
            modifiers.insert(modifier);
        }
        void addModifiers(const set<Modifier>& modifiers) {
            this->modifiers.insert(modifiers.begin(), modifiers.end());
        }
        static Modifier toModifier(string value) {
            if (value == "public") {
                return PUBLIC;
            } else if (value == "private") {
                return PRIVATE;
            } else if (value == "protected") {
                return PROTECTED;
            } else if (value == "static") {
                return STATIC;
            } else if (value == "final") {
                return FINAL;
            } else if (value == "synchronized") {
                return SYNCHRONIZED;
            } else if (value == "module") {
                return PACKAGE;
            }

            return NONE;
        }
    };
}
