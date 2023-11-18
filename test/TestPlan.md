# Test Plan
## Compile
- Can compile empty class
## Class 
### Can Write & Read Class Metadata
- Can write property block
  - Can write size of struct
  - Can write count of all props
  - Can write 
### Memory Management
- Can run main on empty class
- Can allocate class and its freed after scope drop
- Can allocate class and its methods are all mapped and freed after scope drop
- Can allocate class and its properties are all mapped and freed after scope drop
- Can write definition in module for class
- Can read definition in module for class
- Can find definition in module for class
### References
- Can create arbitrary scopes
- Can transfer a reference from the original and deallocate correctly
### Method calls
- Can run main and call method, returning its value
- Can run main, allocate class, call method, ...
### Class Loader
- Can load a class locally
- Can load a class from a URL
- Can load a class from a signed jarvcx
### Aspects
- Can set a property's value
- Can add a property
- Can intercept a method
- Can add a method
### Reflection
- Can get read all information on a class
  - Properties
    - Type
    - Annotations
  - Constructor
    - Annotations
    - Arguments
      - Type
      - Annotations
  - Methods
- Can allocate class from constructor
## System
- Can get current time in ms
- Can get environment variables
- Can write to stdout
- Can write to stderr
- Can read from stdin
- Can wrap all FILE methods
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
#### Overloading
- May not overload all numeric operators on numeric arguments
- May not overload all bitwise operators on numeric arguments
- Can overload all operators with classes
- Can overload all operators with template classes
## Exception Handling (https://llvm.org/docs/ExceptionHandling.html)
- Can catch exceptions
- Can throw exceptions
- Can throw through multiple frames
- Can throw without memory leaks
## Templates
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
## Enums
- Support elements
- Support properties
- Support methods
## Lambdas
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
## Runtime
### Threading
- Thread class
  - Can create thread
  - Can join thread
  - Can interrupt thread 
### Strings
### Collections
- List
- Array
  - Can create an array (literal processing)
  - numbers
  - Strings
  - Objects
  - Can set and get elements on array
  - Can deallocate an array
- ArrayList
- Trees
  - RedBlueTree
  - B+Tree
- Map
  - DenseHashMap
  - SparseHashMap
  - Concurrence
  - Ltm
- Set
  - DenseHashMap
  - SparseHashMap
  - Concurrence
  - Ltm
### Synchronization
- Mutex
  - Can lock
  - Can try lock
  - Can unlock
- Condition Variable
  - Can notify one
  - Can notify all
  - Can wait
  - Can wait for
  - Can wait until
- Locks
  - ReadWriteLock
    - Can allow simultaneous reads
    - Can allow solitary writes
      - Assure that all reads have completed before write
  - MultiQueueReadWriteLock
    - Can allow simultaneous reads
    - Can allow writes over multiple queues
      - Assure that reads have completed prior to write
- ### Networking
- Socket
- ePoll vs IOCP vs AIO Common
- 