# Test Plan
## Basic 
- Can compile empty class
- Can write definition in module for class
- Can read definition in module for class
- Can find definition in module for class
- Can create arbitrary scopes
- Can write to stdout
- Can write to stderr
- Can read from stdin
- Can wrap all FILE methods
## Memory Management
- Can run main on empty class
- Can allocate class and its freed after scope drop
- Can allocate class and its methods are all mapped and freed after scope drop
- Can allocate class and its properties are all mapped and freed after scope drop
- Can transfer a reference from the original and deallocate correctly
## Method calls
- Can run main and call method, returning its value
- Can run main, allocate class, call method, ...
## Native Type Tests
### Numbers
- Can create and return numbers 
- Can promote numbers 
- Can demote numbers
- Can execute all numeric operators on numeric arguments
- Can execute all bitwise operators on numeric arguments
- Can create max number for each type and print it
- Can create min number for each type and print it
- Can signflip a signed number
- Can integer overflow, MAX_INT + 1 = MIN_INT
### Operators
#### Boolean Logic
- Can if
- Can and conditional
- Can or conditional
- Can not conditional
- Can then conditional
- Can else conditional
- Can handle parenthetical order of operation
#### Bitwise Operators
- and
- or
- xor
- not
- #### Overloading
- May not overload all numeric operators on numeric arguments
- May not overload all bitwise operators on numeric arguments
- Can overload all operators with classes
- Can overload all operators with template classes
### Exception Handling (https://llvm.org/docs/ExceptionHandling.html)
- Can catch exceptions
- Can throw exceptions
- Can throw through multiple frames
- Can throw without memory leaks
### Templates
- Support mono-valued templates
  - Can get full RTTI from type
  - Can get compiler support for parameterized type
  - Can allocate type
  - Can free type
- Support multi-valued templates
  - Can get full RTTI from type
  - Can get compiler support for parameterized type
  - Can allocate type
  - Can free type
### Enums
- Support elements
- Support properties
- Support methods
### Lambdas
- Support MonoFunctions
  - Can assign lambda to variable
  - Can call lambda
  - Can get returnvalue
  - Can support all return types 
- Support BiFunctions
  - Can assign lambda to variable
  - Can call lambda
  - Can get returnvalue
  - Can support all return types
- Support TriFunctions
  - Can assign lambda to variable
  - Can call lambda
  - Can get returnvalue
  - Can support all return types- 
### Threading
- Thread class
  - Can create thread
  - Can join thread
  - Can interrupt thread 
- Mutex
### Arrays
- Can create an array
  - numbers
  - Strings
- Can set and get elements on array
- Can deallocate an array