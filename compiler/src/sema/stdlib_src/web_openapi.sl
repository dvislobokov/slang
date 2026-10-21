namespace std.web

// The OpenAPI 3.1 document of an App, built from the endpoint metadata
// that the compiler generates with the route table (parameters, body and
// response schemas from json.Schema<T>(), tags, summaries, security), and
// the /swagger page (swagger-ui from a CDN).

// The OpenAPI 3.1 document (JSON) of the mapped endpoints, paths in
// registration order.
func OpenApiDocument(a *App) string {
    var b = std.strings.Builder{}
    b.Write("{\"openapi\":\"3.1.0\",\"info\":{\"title\":")
    b.Write(std.encoding.json.Quote(a.Title))
    b.Write(",\"version\":")
    b.Write(std.encoding.json.Quote(a.Version))
    b.Write("},\"paths\":{")
    var patterns []string = []
    for e in a.endpoints {
        if e.hidden {
            continue
        }
        var seen = false
        for p in patterns {
            if p == e.Pattern {
                seen = true
            }
        }
        if !seen {
            patterns = append(patterns, e.Pattern)
        }
    }
    var secured = false
    for i, p in patterns {
        if i > 0 {
            b.Write(",")
        }
        b.Write(std.encoding.json.Quote(p))
        b.Write(":{")
        var n = 0
        for e in a.endpoints {
            if e.hidden || e.Pattern != p || e.Method == "*" {
                continue
            }
            if n > 0 {
                b.Write(",")
            }
            n += 1
            b.Write(std.encoding.json.Quote(std.strings.ToLower(e.Method)))
            b.Write(":")
            writeOperation(&b, e)
            if e.RequireAuth && !e.Anonymous {
                secured = true
            }
        }
        b.Write("}")
    }
    b.Write("},\"components\":{\"schemas\":{\"ProblemDetails\":{\"type\":\"object\",\"properties\":{\"type\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"},\"status\":{\"type\":\"integer\",\"format\":\"int32\"},\"detail\":{\"type\":\"string\"},\"instance\":{\"type\":\"string\"},\"errors\":{\"type\":\"object\",\"additionalProperties\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}}")
    if secured {
        b.Write(",\"securitySchemes\":{\"bearer\":{\"type\":\"http\",\"scheme\":\"bearer\",\"bearerFormat\":\"JWT\"}}")
    }
    b.Write("}}")
    return b.String()
}

func operationId(e *Endpoint) string {
    if e.Name != "" {
        return std.strings.Replace(e.Name, ".", "_")
    }
    var id = std.strings.ToLower(e.Method)
    for p in e.parts {
        var s = std.strings.TrimSuffix(std.strings.TrimPrefix(p, "{"), "}")
        if s == "*" {
            s = "rest"
        }
        id += "_" + s
    }
    return id
}

func writeOperation(b *std.strings.Builder, e *Endpoint) {
    b.Write("{\"operationId\":")
    b.Write(std.encoding.json.Quote(operationId(e)))
    if e.Tag != "" {
        b.Write(",\"tags\":[")
        b.Write(std.encoding.json.Quote(e.Tag))
        b.Write("]")
    }
    if e.Summary != "" {
        b.Write(",\"summary\":")
        b.Write(std.encoding.json.Quote(e.Summary))
    }
    var params = 0
    var forms []ParamDoc = []
    for p in e.Params {
        if p.In == "form" {
            forms = append(forms, p)
            continue
        }
        if params == 0 {
            b.Write(",\"parameters\":[")
        } else {
            b.Write(",")
        }
        params += 1
        b.Write("{\"name\":")
        b.Write(std.encoding.json.Quote(p.Name))
        b.Write(",\"in\":")
        b.Write(std.encoding.json.Quote(p.In))
        if p.Required {
            b.Write(",\"required\":true")
        }
        b.Write(",\"schema\":")
        b.Write(schemaText(p.Schema))
        b.Write("}")
    }
    if params > 0 {
        b.Write("]")
    }
    if e.RequestSchema != "" {
        b.Write(",\"requestBody\":{\"required\":true,\"content\":{")
        b.Write(std.encoding.json.Quote(e.RequestContentType))
        b.Write(":{\"schema\":")
        b.Write(e.RequestSchema)
        b.Write("}}}")
    } else if len(forms) > 0 {
        b.Write(",\"requestBody\":{\"content\":{\"application/x-www-form-urlencoded\":{\"schema\":{\"type\":\"object\",\"properties\":{")
        for i, f in forms {
            if i > 0 {
                b.Write(",")
            }
            b.Write(std.encoding.json.Quote(f.Name))
            b.Write(":")
            b.Write(schemaText(f.Schema))
        }
        b.Write("}}}}}")
    }
    b.Write(",\"responses\":{")
    if len(e.Responses) == 0 {
        b.Write("\"200\":{\"description\":\"OK\"}")
    }
    for i, r in e.Responses {
        if i > 0 {
            b.Write(",")
        }
        b.Write(std.encoding.json.Quote(std.fmt.Format("{0}", r.Status)))
        b.Write(":{\"description\":")
        b.Write(std.encoding.json.Quote(std.net.http.StatusText(r.Status)))
        if r.ContentType == "application/problem+json" {
            b.Write(",\"content\":{\"application/problem+json\":{\"schema\":{\"$ref\":\"#/components/schemas/ProblemDetails\"}}}")
        } else if r.ContentType != "" {
            b.Write(",\"content\":{")
            b.Write(std.encoding.json.Quote(r.ContentType))
            b.Write(":{\"schema\":")
            b.Write(schemaText(r.Schema))
            b.Write("}}")
        }
        b.Write("}")
    }
    if e.RequireAuth && !e.Anonymous {
        b.Write(",\"401\":{\"description\":\"Unauthorized\"}")
        if len(e.Roles) > 0 {
            b.Write(",\"403\":{\"description\":\"Forbidden\"}")
        }
    }
    b.Write("}")
    if e.RequireAuth && !e.Anonymous {
        b.Write(",\"security\":[{\"bearer\":[]}]")
    }
    b.Write("}")
}

func schemaText(s string) string {
    if s == "" {
        return "{}"
    }
    return s
}

// The swagger-ui page for the document at specUrl (scripts and styles
// from unpkg; nothing is bundled).
func SwaggerPage(title string, specUrl string) string {
    return "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>" + escapeHtml(title) + " - Swagger UI</title>\n<link rel=\"stylesheet\" href=\"https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui.css\">\n</head>\n<body>\n<div id=\"swagger-ui\"></div>\n<script src=\"https://unpkg.com/swagger-ui-dist@5.17.14/swagger-ui-bundle.js\" crossorigin></script>\n<script>\nwindow.onload = () => { window.ui = SwaggerUIBundle({ url: " + std.encoding.json.Quote(specUrl) + ", dom_id: \"#swagger-ui\" }); };\n</script>\n</body>\n</html>\n"
}

func escapeHtml(s string) string {
    var r = std.strings.Replace(s, "&", "&amp;")
    r = std.strings.Replace(r, "<", "&lt;")
    r = std.strings.Replace(r, ">", "&gt;")
    return std.strings.Replace(r, "\"", "&quot;")
}
