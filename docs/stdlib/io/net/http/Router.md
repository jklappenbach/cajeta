# Router

`cajeta.io.net.http.Router` — the minimal HTTP router: maps an incoming
request's `(method, path)` to a registered handler by method + path-pattern
matching (`/users/{id}` path parameters), answering `404 Not Found` when no
route's path matches and `405 Method Not Allowed` (with an `Allow` header)
when a path matches but the method differs. Routes are walked in registration
order; the first match wins, and a `{param}` segment captures any non-empty
segment (percent-decoded before binding). Deliberately minimal — no
middleware, wildcards, or route groups. Mount it on a server by forwarding:
`(req) -> theRouter.dispatch(req)` is the handler
[`HttpServer.bind`](HttpServer.md) takes.

```cajeta
(HttpRequest) -> #HttpResponse h = (HttpRequest req) -> {
    String id = req.pathParam("id");
    if (id == null) { return HttpResponse.of(400); }
    return HttpResponse.of(200);
};
Router r = heap Router();
r.route("GET", "/users/{id}", h);
int32 registered = r.size();     // 1
```

## Methods

| Signature | |
|---|---|
| `Router()` ⚑ | An empty router (no routes; everything 404s until registered) |
| `Router route(String method, String pattern, (HttpRequest) -> #HttpResponse handler)` ⚑ | Register a handler for `method` + path `pattern` (`/users/{id}`); registrations chain |
| `int32 size()` | The number of registered routes |
| `#HttpResponse dispatch(HttpRequest request)` | Route `request` to its handler, or to the `404`/`405` default |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/http/Router.cajeta`](../../../../../runtime/src/cajeta/io/net/http/Router.cajeta)
- [HttpServer](HttpServer.md) — where a router's `dispatch` runs
