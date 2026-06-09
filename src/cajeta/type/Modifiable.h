//
// Created by James Klappenbach on 2/19/22.
//

#pragma once

#include <set>
#include <string>

using namespace std;

namespace cajeta {
    enum Modifier {
        NONE = 0x00,
        PACKAGE = 0x01,
        PUBLIC = 0x02,
        PRIVATE = 0x04,
        PROTECTED = 0x08,
        STATIC = 0x10,
        FINAL = 0x20,
        SYNCHRONIZED = 0x40,
        // docs/stdlib/Concurrency.md — method whose body is a suspendable state machine.
        // In the sync-lowering MVP, only the modifier bit is recorded; codegen
        // ignores it. The real scheduler / state-machine lowering keys off this
        // bit to pick the async ABI.
        ASYNC = 0x80,
        // REFL-3.3 — set on a class annotated `@Sealed` (decision D1). Bars
        // reflective access to the class's PRIVATE members: the synthesized
        // invoke/newInstance adapters omit private cases, and the reflect API
        // throws IllegalAccessException. Recorded as a class modifier so it
        // rides into the RTTI header's `modifiers` word for free. Not a source
        // keyword — it is derived from the `@Sealed` annotation in
        // visitClassDeclaration (distinct from Java's `sealed` subclassing).
        // Named REFLECT_SEALED (not SEALED) to avoid colliding with the
        // ReservedIdentifiers::SEALED enumerator (both unscoped, namespace cajeta).
        REFLECT_SEALED = 0x100
    };

    class Modifiable {
    protected:
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

        void addModifier(string str) {
            Modifier modifier = toModifier(str);
            if (modifier != NONE) {
                modifiers.insert(modifier);
            }
        }

        set<Modifier>& getModifiers() { return modifiers; }

        bool isStatic() {
            return modifiers.find(STATIC) != modifiers.end();
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
            } else if (value == "pModule") {
                return PACKAGE;
            } else if (value == "async") {
                return ASYNC;
            }

            return NONE;
        }

        static string toString(Modifier modifier) {
            switch (modifier) {
                case PUBLIC:
                    return "public";
                case PRIVATE:
                    return "private";
                case PROTECTED:
                    return "protected";
                case STATIC:
                    return "static";
                case FINAL:
                    return "final";
                case SYNCHRONIZED:
                    return "synchronized";
                case PACKAGE:
                    return "package";
                case ASYNC:
                    return "async";
                default:
                    return "";
            }
        }

        string toCanonical() {
            string result;
            for (Modifier modifier : modifiers) {
                result.append(toString(modifier)).append(" ");
            }
            return result;
        }
    };
}
