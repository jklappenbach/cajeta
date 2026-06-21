---
id: file-open
applies-to: [cajeta/io/File, cajeta/io/File.open]
title: Opening files
description: How to open and dispose File handles correctly.
---
# Opening files

Use `File.open` to obtain a handle and dispose it deterministically — prefer a
scoped binding so the handle is released at end of scope rather than relying on
finalization.

```cajeta
let f = cajeta.io.File.open("data.txt", Mode.Read);
defer f.close();
```
