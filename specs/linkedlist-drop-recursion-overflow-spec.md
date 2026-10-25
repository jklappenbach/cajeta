# linkedlist-drop-recursion-overflow — dropping a long `LinkedList` overflows the stack (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 6 on
> v0.46.3.

## 1. Definition
Dropping a `LinkedList` of 1e6 nodes crashes with a stack overflow
(SIGSEGV): the node drop recurses once per node through the owned `next`
field. `collection/linkedlist-insert-traverse` empties the list with
`popHead` inside its measured window to avoid it. Any owned recursive
structure (a long singly-linked chain, a deep unbalanced tree) has the same
shape. A second observation from the same work: `s.equals("1")` on an
environment value returned false inside the bench binary and true in a
standalone probe with identical code; the bench switched to the harness's
parser and did not chase it, so it is recorded here as unconfirmed.

## 2. Use cases
- **2.1** As a program, when a `LinkedList` of any length goes out of scope,
  then it is freed without recursion (an iterative drop in `LinkedList`, or
  a compiler-emitted iterative drop for a self-referential owned field).
- **2.2** As the test suite, then a test drops a 1e7-node list under the
  default stack and a test drops a 1e6-deep owned chain of a user class.

## 3. Acceptance
The benchmark drops the list at scope end and its workaround note is removed.
