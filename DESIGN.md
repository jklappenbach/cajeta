#Parsing Tree

```
(compilationUnit 
  (PackageDeclaration packageName 
    (qualifiedName 
      (identifier cajeta)
    );
  ) 
  (typeDeclaration 
    (classOrInterfaceModifier public) 
      (classDeclaration class 
        (identifier System) 
          (classBody { 
            (classBodyDeclaration 
              (modifier 
                (classOrInterfaceModifier private)
              ) 
              (memberDeclaration 
                (fieldDeclaration 
                  (typeType 
                    (primitiveType int32)
                  ) 
                  (variableDeclarators 
                    (variableDeclarator 
                      (variableDeclaratorId 
                        (identifier value)
                      )
                    )
                  );
                )
              )
            ) 
            (classBodyDeclaration 
              (modifier 
                (classOrInterfaceModifier private)
              ) 
              (memberDeclaration 
                (fieldDeclaration 
                  (typeType 
                    (classOrInterfaceType 
                      (identifier Object)
                    )
                  ) 
                  (variableDeclarators 
                    (variableDeclarator 
                      (variableDeclaratorId 
                        (identifier obj)
                      )
                    )
                  );
                )
              )
            )            
            (classBodyDeclaration 
              (modifier 
                (classOrInterfaceModifier public)
              ) 
              (modifier 
                (classOrInterfaceModifier static)
              ) 
              (memberDeclaration 
                (methodDeclaration 
                  (typeTypeOrVoid void) 
                  (identifier main) 
                  (formalParameters 
                    ( 
                      (formalParameterList 
                        (formalParameter 
                          (typeType 
                            (classOrInterfaceType 
                              (identifier String)
                            )[ ]
                          ) 
                          (variableDeclaratorId 
                            (identifier args)
                          )
                        )
                      ) 
                    )
                  ) 
                  (methodBody 
                    (block { 
                      (blockStatement 
                        (statement 
                          (expression 
                            (expression 
                              (primary 
                                (identifier value)
                              )
                            ) = 
                            (expression 
                              (primary 
                                (literal (integerLiteral 5))
                              )
                            )
                          );
                        )
                      )
                      (blockStatement 
                        (statement 
                          (expression 
                            (expression 
                              (expression 
                                (primary 
                                  (identifier System)
                                )
                              ) . 
                              (identifier out)
                            ) . 
                            (methodCall 
                              (identifier printf) 
                            ( 
                              (expressionList 
                                (expression 
                                  (primary 
                                    (literal "Hello World!\n")
                                  )
                                )
                              ) 
                            )
                          )
                        );
                      )
                    )}
                  )
                )
              )
            )
          )}
        )
      )
    )
  )
)
```
#Class Design
- LLVM Structure for Classes
  - className: String
  - Virtual Table
    - Entry per curMethod
    - Function pointers to highest superclass entry if no override
  - Reflection Table
    - methodName: String
      - overrides: Array<Array<Type*>>
        - parameters: Array<Type*>
          - parameter: Type*
      - Entries ordered by methodName for quick binary search during compilation
- Annotations
  - Like Java, have a version of annotations that act as markers
    - Example: Spring @Components, which are automatically instantiated by the Spring Framework
  - Also feature injection annotation types with the ability to inject llvm IR with the following strategy:
    - When injection annotation is detected, 
      - The implementation annotation class will be JIT compiled 
      - Directly executed as part of the compilation process
      - Compiler will call the injection code, providing the callback with the current event and injection API object
        - Events defined below
        - API object has access to the current module definition, and can call IR generation methods just like the listener
    - AOP Annotations specifically for
      - Class
        - Base interface features a callback curMethod offering an AOP object
          - Callback Events
            - Class fields declared
            - Class methods declared
            - Class definition complete
          - AOP Capabilities
            - Add field
            - Set field value
            - Add curMethod
            - Wrap curMethod
      - Class field
        - Callback Events
          - Class field declared
        - AOP Capability
          - Set field value
          - Add curMethod
          - Wrap curMethod
      - Class curMethod
        - Callback Events
          - Class curMethod declared
        - AOP Capability
          - Wrap curMethod
      - Class curMethod parameter
        - Callback Events
          - Parameter declared
        - AOP Capability
          - Set parameter value
- Runtime Module
  - [cajeta.lang.Class<T>](https://docs.oracle.com/javase/8/docs/api/java/lang/Class.html)
  - [cajeta.lang.Object](https://docs.oracle.com/javase/8/docs/api/java/lang/Object.html)
    - hashCode: default curMethod based on murmur3
    - equals
    - toString
    - finalize
    - getClass
    - wait
    - notify
    - notifyAll
    - wait(long timeout)
    - wait(long timeout, int nanos)
  - StringLibrary
    - Entry for each string allocated
      - All strings allocated at compile time are located inline
      - Pointers to strings allocated at runtime are to the heap
    - String Class Interaction
      - Strings receive argument strings on the stack
      - GetOrAdd string by hash in the library.
