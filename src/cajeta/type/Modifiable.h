// Modifier - the declaration-modifier bit set carried by every declaration.

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
        // A method whose body is a suspendable state machine; the sync-lowering
        // MVP records the bit only.
        ASYNC = 0x80,
        // From `@Sealed`, not a keyword: bars reflective access to PRIVATE
        // members. Named REFLECT_SEALED so it cannot collide with the
        // ReservedIdentifiers::SEALED enumerator, which is also unscoped.
        REFLECT_SEALED = 0x100,
        // From `@Retained`, not a keyword: the class must stay in the
        // Class.forName registry even when nothing references it.
        REFLECT_RETAINED = 0x200,
        // A `mut` field accepts in-place writes; a record is otherwise immutable.
        MUT = 0x400
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
            } else if (value == "mut") {
                return MUT;
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
