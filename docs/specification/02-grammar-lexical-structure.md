# 2 — Grammar & Lexical Structure

This chapter defines the notation grammar productions use throughout the specification, and the lexical structure of Cajeta source: encoding, tokens, identifiers, keywords, literals, operators, and comments.

## 2.1 Grammar Notation

The grammar's source of truth is the ANTLR grammar pair `antlr4/CajetaLexer.g4` and `antlr4/CajetaParser.g4`. Productions in this specification are taken from it and presented in its notation:

- A production names a rule, a colon, and one or more alternatives separated by `|`, ending with `;`.
- Lexical rules are written in upper case (`IDENTIFIER`, `DECIMAL_LITERAL`), and syntactic rules in camel case (`classDeclaration`).
- `'…'` quotes a literal terminal. `x?` is optional, `x*` zero or more, `x+` one or more. `~[…]` matches any character not in the set. `fragment` rules exist only to build other lexical rules and produce no tokens.

The complete grammar, collected, is Complete Grammar §21.

## 2.2 Source Text

Source text is Unicode, and the reference implementation reads source files as UTF-8. The language is case-sensitive. Whitespace — space, tab, carriage return, line feed, and form feed — separates tokens and has no other meaning.

## 2.3 Tokenization

The lexer is greedy: at each position it produces the longest token the lexical grammar admits, and among equal-length matches, the rule declared first. In particular, `#=` is one indivisible token, declared before `#` so that a title store cannot be read as `#` followed by `=`:

```text
SHARP_ASSIGN:       '#=';
REFERENCE:          '#';
```

## 2.4 Identifiers

```text
IDENTIFIER:         Letter LetterOrDigit*;
```

A letter is `a`–`z`, `A`–`Z`, `$`, `_`, or any Unicode character above U+007F that is not a surrogate (supplementary characters are accepted as surrogate pairs). Digits are `0`–`9`. An identifier is any such sequence that is not a keyword.

**Example 2.4-1.** Identifiers may use non-ASCII letters.

```cajeta
int32 café = 1;
System.stdout.println(café);
```

## 2.5 Keywords

The following words are keywords and cannot be used as identifiers:

```text
abstract   annotation  assert     async      await      boolean    break
case       catch       char       class      const      continue   default
detach     do          else       enum       extends    final      finally
for        goto        heap       if         implements import     instanceof
interface  mut         native     non-sealed operator   package    permits
private    protected   public     record     return     scope      sealed
shared     spawn       stack      static     strictfp   structure  super
switch     this        throw      throws     transient  try        var
view       void        volatile   while      yield
```

The primitive type names are also keywords: `int8` `uint8` `int16` `uint16` `int32` `uint32` `int64` `uint64` `int128` `uint128`, `float16` `float32` `float64` `float128`, the reduced-precision floats `float4e2m1` `float6e2m3` `float6e3m2` `float8e4m3` `float8e5m2` `float8e4m3fnuz` `float8e5m2fnuz`, plus `boolean` and `char` above. The literal words `true`, `false`, and `null` are literals, not keywords, but are equally unavailable as identifiers.

Three groups carry qualifications:

- **Reserved, no grammar role today:** `goto` and `structure` appear in no parser rule. `var` is reserved for local type inference.
- **Soft keyword:** `mut` is also accepted where an identifier is expected, so existing names continue to parse. Its keyword role is per-field mutation opt-in on records.
- **Module words:** `open`, `requires`, `exports`, `opens`, `to`, `uses`, `provides`, `with`, `transitive` (and the module-declaration token itself) are reserved for future module syntax but are consumed by no rule today, and remain freely usable as ordinary identifiers so that names like `open` and `requires` parse.

## 2.6 Literals

### 2.6.1 Integer literals

```text
DECIMAL_LITERAL:    ('0' | [1-9] (Digits? | '_'+ Digits)) [lL]?;
HEX_LITERAL:        '0' [xX] [0-9a-fA-F] ([0-9a-fA-F_]* [0-9a-fA-F])? [lL]?;
OCT_LITERAL:        '0' '_'* [0-7] ([0-7_]* [0-7])? [lL]?;
BINARY_LITERAL:     '0' [bB] [01] ([01_]* [01])? [lL]?;
```

Decimal, hexadecimal (`0x`), octal (leading `0`), and binary (`0b`) forms. Underscores may separate digits for readability, and they carry no value. The `l`/`L` suffix marks a 64-bit literal.

### 2.6.2 Floating-point literals

```text
FLOAT_LITERAL:      (Digits '.' Digits? | '.' Digits) ExponentPart? [fFdD]?
             |       Digits (ExponentPart [fFdD]? | [fFdD]);
HEX_FLOAT_LITERAL:  '0' [xX] (HexDigits '.'? | HexDigits? '.' HexDigits) [pP] [+-]? Digits [fFdD]?;
```

Decimal forms with optional exponent (`1.5e3`), and hexadecimal forms with a binary exponent (`0x1.8p3`). The `f`/`F` suffix marks `float32`, and `d`/`D` marks `float64`.

### 2.6.3 Boolean, character, string, and null literals

```text
BOOL_LITERAL:       'true' | 'false';
CHAR_LITERAL:       '\'' (~['\\\r\n] | EscapeSequence) '\'';
STRING_LITERAL:     '"' (~["\\\r\n] | EscapeSequence)* '"';
NULL_LITERAL:       'null';
```

The escape sequences are `\b` `\t` `\n` `\f` `\r` `\"` `\'` `\\`, octal escapes of up to three digits, and Unicode escapes `\u` followed by four hexadecimal digits.

**Example 2.6-1.** Literal forms.

```cajeta
int32 million = 1_000_000;
int64 mask = 0xFF_FFL;
int32 bits = 0b1010_1010;
float64 f = 1.5e3d;
char nl = '\n';
String s = "a\tb";
System.stdout.println("" + million + " " + mask + " " + bits + " " + f + " " + s);
```

> *Discussion.* The lexer also defines a text-block token — `"""` through `"""`, spanning lines. Delimiter and indentation processing for text blocks is not finished (as of 0.27.0 the delimiters' inner quotes reach the string value verbatim), so text blocks are not yet specified here.

## 2.7 Separators and Operators

Separators: `(` `)` `{` `}` `[` `]` `;` `,` `.` `@` `...` `::`.

Ownership tokens: `#=` (title store, one token — §2.3) and `#` (move expression prefix, and the must-own marker in `#T` parameter and return types). Their semantics are Ownership §5.

Operators:

```text
=   >   <   !   ~   ?   :   ==  <=  >=  !=  &&  ||  ++  --
+   -   *   /   &   |   ^   %   ->
+=  -=  *=  /=  &=  |=  ^=  %=  <<=  >>=  >>>=
```

`->` introduces a lambda body (Expressions §14), and `::` forms a method reference. Operator semantics and precedence are Expressions §14. Overloadable operators are Classes §8.

## 2.8 Comments

```text
COMMENT:            '/*' (COMMENT | .)*? '*/';
LINE_COMMENT:       '//' ~[\r\n]*;
```

A line comment runs to end of line. Block comments **nest**: `/*` inside a block comment opens an inner comment that must close before the outer one ends. A comment whose opener is `/**` is a documentation comment by convention. The lexer does not distinguish it, and documentation tooling consumes the convention.

**Example 2.8-1.** A nested block comment.

```cajeta
/* outer /* nested */ still comment */
int32 x = 5;
System.stdout.println(x);
```
