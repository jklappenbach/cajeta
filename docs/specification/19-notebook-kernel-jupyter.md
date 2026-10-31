# 19 — Notebook Kernel & Jupyter Compatibility

This chapter defines the notebook kernel. One kernel process hosts one persistent JIT session, cells are script units compiled into it, and the kernel's conformance to the Jupyter messaging protocol is stated per channel. The kernel works with any frontend speaking Jupyter messaging v5.3, such as Jupyter Lab, classic Notebook, `jupyter console`, and VS Code's notebook UI.

## 19.1 The Session Model

Each cell compiles into the session and sees everything earlier cells declared. Bindings, types, and top-level methods persist across cells. A cell is a script unit (Script Units §18). Its top-level owner declarations are session bindings, and its trailing expression is the cell result, rendered as `Out[N]` through the value's `toString()`.

**Generational redefinition.** Re-declaring a type in a later cell replaces it for subsequent cells. Values of the earlier generation continue to exist under their original definition. Rebinding a session name drops the old value at the rebind (Script Units §18.2).

**Failure isolation.** A cell that fails to compile, and a cell that throws, both come back as a Jupyter `error` carrying the type, message, and traceback. The session's bindings stay intact, and the next cell runs normally. Traceback frames name cells (`In[3], line 2`), and the synthesized classes cells compile into never appear. A runtime-fatal condition is different. The kernel reports it and exits, because continuing over a world the runtime has declared broken is worse than stopping.

Warnings reach the notebook as `stderr` output, and only the cell's own diagnostics are shown.

## 19.2 Registration and Connection

`cajeta init --kernel` writes a kernelspec into the Jupyter data directory, pointing at the invoking binary. It does not overwrite an existing spec without `--force`. Run by hand, `cajeta kernel` with no connection file binds free ports, writes a connection file into the Jupyter runtime directory, prints the attach command, and removes the file on exit. `cajeta kernel -f connection.json` uses the frontend's file and leaves it alone.

Launched from inside a project directory, the kernel applies that project's `cajeta.json` classpath, so notebooks import project dependencies and Olla libraries exactly as a compiled program would (Script Units §18.5).

## 19.3 Protocol Conformance

The kernel implements Jupyter messaging v5.3. Conformance by channel:

| Channel | Status |
|---|---|
| **shell** | Implemented, with execution requests with streaming output, results, and error replies, plus the execution counter. Completion and introspection requests return empty results rather than hanging the frontend, since there is no engine behind them yet. |
| **iopub** | Implemented, with `stream` output while a cell runs, `execute_result` for the trailing expression, `error` with cell-named tracebacks. |
| **control** | Implemented. Interrupt is handled off the execution thread, so the channel answers even during a runaway loop (§19.4), and restart (§19.4) and shutdown are handled too. |
| **heartbeat** | Implemented. |
| **stdin** | Not supported, and declared as such. A cell cannot prompt for input. |

**Message security.** Messages are HMAC-SHA256 signed with the key from the connection file, per the protocol. A message whose signature does not verify is dropped. It is not answered, not logged back to the sender, and not dispatched. An empty key is the protocol's explicit unsigned mode.

**Display.** Rendering is text plus a JSON bundle. Rich display types such as images and HTML tables are staged behind display protocols in a later revision.

## 19.4 Interrupt and Restart

Interrupting from the frontend stops the running cell at its next statement boundary. The cell ends as a `KeyboardInterrupt` error, the session and its bindings survive, and the next cell runs normally. Interruption is safepoint-granular, and safepoints exist in the cell's own code, so a cell parked inside a long stdlib or native call stops when that call returns. An interrupt with nothing running is a no-op.

Restart drops every session binding, and destructors run. It resets the execution counter to 1 and starts from an empty session.

## 19.5 Declared Gaps

These are stated as conformance items as of v1. There is no completion or introspection engine behind the answered protocol requests, display is text and JSON only, and there is no debugger bridge. Breakpoints, pause, and step inside a notebook are a later layer with its own specification.
