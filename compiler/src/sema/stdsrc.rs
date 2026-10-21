//! Parts of the standard library written in Slang: `std.task` (`Channel<T>`,
//! `Mutex`, `WaitGroup`, `Spawn`, `Timeout`, `Context`), `std.net` (TCP
//! over non-blocking sockets), `std.net.http` (HTTP/1.1 client and
//! server), `std.log` (structured logging, with compiled message templates
//! in [`super::logtmpl`]), `std.diag` (metrics, tracing, profiles),
//! `std.crypto.x509` (certificates) and `std.net.tls` (TLS 1.3/1.2 in three
//! files: `tls.sl`, `tls_client.sl`, `tls_server.sl` share the namespace), and
//! the tier 0/1 library of docs/STDLIB.md (`std.slices`, `maps`, `cmp`,
//! `bytes`, `strconv`, `time` types, `math.bits/big/complex`,
//! `unicode.utf8/utf16`, `hash.*`, `encoding.binary/base32/pem/asn1`, `io`,
//! `os` files, `os.signal`, `os.user`, `process`, more `collections`, `sync`,
//! `cli`, `testing`, `embed`), and the tier 3 modules (`std.database.sql`
//! with the `postgres` and `sqlite` drivers, `std.net.mail`,
//! `std.crypto.kdf/password`, RSA key generation), and the tier 3b
//! infrastructure clients (`std.data.redis`, `std.data.kafka`, `std.cloud.s3`,
//! `std.cloud.blob`; C helpers in `runtime/src/std_infra.c`). The sources live in `sema/stdlib_src/*.sl`; like
//! `std.collections` ([`super::collections`]) they are parsed once per
//! process and registered as stdlib templates, so only what a program uses
//! is instantiated, checked and compiled.
//!
//! Rules for the sources (they are instantiated in the scope of the user
//! file that first uses them):
//!
//! * structs, functions and enums become templates, also without type
//!   parameters ("plain" templates, instantiated with no arguments on first
//!   use: `task.Mutex`, `net.Dial`); interfaces are registered as ordinary
//!   interfaces. Top-level `var`/`const` are not supported;
//! * names of the module's own namespace are written short (`Conn`); every
//!   other namespace is written with its full path (`std.strings.Index`,
//!   `std.net.Conn`), because the importing file's imports are in effect;
//!   interface method signatures always use full paths;
//! * the runtime is reached through the stdlib externs of
//!   [`super::stdlib::FUNCS`] whose names start with `rt` (internal).

use super::generics::{Inst, TmplDecl};
use super::*;
use std::sync::OnceLock;

/// The Slang-source modules: (namespace, file name, source).
const MODULES: &[(&str, &str, &str)] = &[
    ("std.task", "task.sl", include_str!("stdlib_src/task.sl")),
    ("std.net", "net.sl", include_str!("stdlib_src/net.sl")),
    ("std.net", "net_udp.sl", include_str!("stdlib_src/net_udp.sl")),
    ("std.net.http", "http.sl", include_str!("stdlib_src/http.sl")),
    ("std.net.http", "http_server.sl", include_str!("stdlib_src/http_server.sl")),
    ("std.net.http", "http_client.sl", include_str!("stdlib_src/http_client.sl")),
    ("std.net.http", "http2.sl", include_str!("stdlib_src/http2.sl")),
    ("std.net.http", "http_extra.sl", include_str!("stdlib_src/http_extra.sl")),
    ("std.net.http.hpack", "hpack.sl", include_str!("stdlib_src/hpack.sl")),
    ("std.net.http.test", "httptest.sl", include_str!("stdlib_src/httptest.sl")),
    ("std.net.ws", "ws.sl", include_str!("stdlib_src/ws.sl")),
    ("std.crypto.x509", "x509.sl", include_str!("stdlib_src/x509.sl")),
    ("std.net.tls", "tls.sl", include_str!("stdlib_src/tls.sl")),
    ("std.net.tls", "tls_client.sl", include_str!("stdlib_src/tls_client.sl")),
    ("std.net.tls", "tls_server.sl", include_str!("stdlib_src/tls_server.sl")),
    ("std.log", "log.sl", include_str!("stdlib_src/log.sl")),
    ("std.diag", "diag.sl", include_str!("stdlib_src/diag.sl")),
    ("std.arena", "arena.sl", include_str!("stdlib_src/arena.sl")),
    // >>> debugger (docs/DEBUGGER.md stage 2)
    ("std.debug", "debug.sl", include_str!("stdlib_src/debug.sl")),
    ("std.debug", "debug_dap.sl", include_str!("stdlib_src/debug_dap.sl")),
    ("std.debug", "debug_vars.sl", include_str!("stdlib_src/debug_vars.sl")),
    // <<< debugger
    // >>> tier 2 stdlib (docs/STDLIB.md level 2)
    ("std.regex", "regex.sl", include_str!("stdlib_src/regex.sl")),
    ("std.regex", "regex_syntax.sl", include_str!("stdlib_src/regex_syntax.sl")),
    ("std.regex", "regex_vm.sl", include_str!("stdlib_src/regex_vm.sl")),
    ("std.compress", "compress.sl", include_str!("stdlib_src/compress.sl")),
    ("std.compress.deflate", "deflate.sl", include_str!("stdlib_src/deflate.sl")),
    ("std.compress.zlib", "zlib.sl", include_str!("stdlib_src/zlib.sl")),
    ("std.compress.gzip", "gzip.sl", include_str!("stdlib_src/gzip.sl")),
    ("std.compress.zstd", "zstd.sl", include_str!("stdlib_src/zstd.sl")),
    ("std.archive.zip", "zip.sl", include_str!("stdlib_src/zip.sl")),
    ("std.archive.tar", "tar.sl", include_str!("stdlib_src/tar.sl")),
    ("std.encoding.csv", "csv.sl", include_str!("stdlib_src/csv.sl")),
    ("std.encoding", "codec.sl", include_str!("stdlib_src/codec.sl")),
    ("std.encoding.xml", "xml.sl", include_str!("stdlib_src/xml.sl")),
    ("std.encoding.yaml", "yaml.sl", include_str!("stdlib_src/yaml.sl")),
    ("std.encoding.toml", "toml.sl", include_str!("stdlib_src/toml.sl")),
    ("std.text.template", "template.sl", include_str!("stdlib_src/template.sl")),
    ("std.text.template", "template_exec.sl", include_str!("stdlib_src/template_exec.sl")),
    ("std.text.template", "template_check.sl", include_str!("stdlib_src/template_check.sl")),
    ("std.text.template", "htmlctx.sl", include_str!("stdlib_src/htmlctx.sl")),
    ("std.html.template", "htmltemplate.sl", include_str!("stdlib_src/htmltemplate.sl")),
    ("std.net.textproto", "textproto.sl", include_str!("stdlib_src/textproto.sl")),
    ("std.mime", "mime.sl", include_str!("stdlib_src/mime.sl")),
    ("std.mime.quotedprintable", "quotedprintable.sl", include_str!("stdlib_src/quotedprintable.sl")),
    ("std.mime.multipart", "multipart.sl", include_str!("stdlib_src/multipart.sl")),
    ("std.net.url", "url.sl", include_str!("stdlib_src/url.sl")),
    ("std.text.scanner", "scanner.sl", include_str!("stdlib_src/scanner.sl")),
    ("std.text.table", "table.sl", include_str!("stdlib_src/table.sl")),
    // <<< tier 2 stdlib
    // >>> tier 3 stdlib (docs/STDLIB.md level 3)
    ("std.database.sql", "sql.sl", include_str!("stdlib_src/sql.sl")),
    ("std.database.sqlite", "sqlite.sl", include_str!("stdlib_src/sqlite.sl")),
    ("std.crypto.kdf", "kdf.sl", include_str!("stdlib_src/kdf.sl")),
    ("std.crypto.password", "password.sl", include_str!("stdlib_src/password.sl")),
    ("std.crypto.rsa", "rsa.sl", include_str!("stdlib_src/rsa.sl")),
    ("std.database.postgres", "postgres.sl", include_str!("stdlib_src/postgres.sl")),
    ("std.database.postgres", "postgres_types.sl", include_str!("stdlib_src/postgres_types.sl")),
    ("std.net.mail", "mail.sl", include_str!("stdlib_src/mail.sl")),
    ("std.compress.brotli", "brotli.sl", include_str!("stdlib_src/brotli.sl")),
    // <<< tier 3 stdlib
    // >>> tier 3b stdlib (docs/STDLIB.md level 3b: infrastructure clients)
    ("std.data.redis", "redis.sl", include_str!("stdlib_src/redis.sl")),
    ("std.data.redis", "redis_route.sl", include_str!("stdlib_src/redis_route.sl")),
    ("std.data.redis", "redis_client.sl", include_str!("stdlib_src/redis_client.sl")),
    ("std.data.kafka", "kafka.sl", include_str!("stdlib_src/kafka.sl")),
    ("std.data.kafka", "kafka_producer.sl", include_str!("stdlib_src/kafka_producer.sl")),
    ("std.data.kafka", "kafka_consumer.sl", include_str!("stdlib_src/kafka_consumer.sl")),
    ("std.data.kafka", "kafka_admin.sl", include_str!("stdlib_src/kafka_admin.sl")),
    ("std.cloud.s3", "s3.sl", include_str!("stdlib_src/s3.sl")),
    ("std.cloud.s3", "s3_multi.sl", include_str!("stdlib_src/s3_multi.sl")),
    ("std.cloud.blob", "blob.sl", include_str!("stdlib_src/blob.sl")),
    // <<< tier 3b stdlib
    // >>> std.web (docs/FRAMEWORK.md section 2)
    ("std.web", "web_app.sl", include_str!("stdlib_src/web_app.sl")),
    ("std.web", "web_di.sl", include_str!("stdlib_src/web_di.sl")),
    ("std.web", "web_bind.sl", include_str!("stdlib_src/web_bind.sl")),
    ("std.web", "web_mw.sl", include_str!("stdlib_src/web_mw.sl")),
    ("std.web", "web_openapi.sl", include_str!("stdlib_src/web_openapi.sl")),
    ("std.web.config", "web_config.sl", include_str!("stdlib_src/web_config.sl")),
    ("std.web.auth.jwt", "web_jwt.sl", include_str!("stdlib_src/web_jwt.sl")),
    ("std.encoding.json", "web_schema.sl", include_str!("stdlib_src/web_schema.sl")),
    // <<< std.web
    // >>> std.data (docs/FRAMEWORK.md section 3)
    ("std.data", "data.sl", include_str!("stdlib_src/data.sl")),
    ("std.data", "data_query.sl", include_str!("stdlib_src/data_query.sl")),
    ("std.data", "data_save.sl", include_str!("stdlib_src/data_save.sl")),
    ("std.data", "data_migrate.sl", include_str!("stdlib_src/data_migrate.sl")),
    // <<< std.data
    // tier 0/1 stdlib (docs/STDLIB.md)
    ("std.slices", "slices.sl", include_str!("stdlib_src/slices.sl")),
    ("std.maps", "maps.sl", include_str!("stdlib_src/maps.sl")),
    ("std.cmp", "cmp.sl", include_str!("stdlib_src/cmp.sl")),
    ("std.bytes", "bytes.sl", include_str!("stdlib_src/bytes.sl")),
    ("std.strconv", "strconv.sl", include_str!("stdlib_src/strconv.sl")),
    ("std.unicode.utf8", "utf8.sl", include_str!("stdlib_src/utf8.sl")),
    ("std.unicode.utf16", "utf16.sl", include_str!("stdlib_src/utf16.sl")),
    ("std.math.bits", "bits.sl", include_str!("stdlib_src/bits.sl")),
    ("std.math.complex", "complex.sl", include_str!("stdlib_src/complex.sl")),
    ("std.math.big", "big.sl", include_str!("stdlib_src/big.sl")),
    ("std.hash", "hash.sl", include_str!("stdlib_src/hash.sl")),
    ("std.hash.fnv", "fnv.sl", include_str!("stdlib_src/fnv.sl")),
    ("std.hash.crc32", "crc32.sl", include_str!("stdlib_src/crc32.sl")),
    ("std.hash.crc64", "crc64.sl", include_str!("stdlib_src/crc64.sl")),
    ("std.hash.adler32", "adler32.sl", include_str!("stdlib_src/adler32.sl")),
    ("std.hash.maphash", "maphash.sl", include_str!("stdlib_src/maphash.sl")),
    ("std.encoding.binary", "binary.sl", include_str!("stdlib_src/binary.sl")),
    ("std.encoding.base32", "base32.sl", include_str!("stdlib_src/base32.sl")),
    ("std.encoding.pem", "pem.sl", include_str!("stdlib_src/pem.sl")),
    ("std.encoding.asn1", "asn1.sl", include_str!("stdlib_src/asn1.sl")),
    ("std.collections", "collections2.sl", include_str!("stdlib_src/collections2.sl")),
    ("std.sync", "sync.sl", include_str!("stdlib_src/sync.sl")),
    ("std.os.signal", "signal.sl", include_str!("stdlib_src/signal.sl")),
    ("std.process", "process.sl", include_str!("stdlib_src/process.sl")),
    ("std.os.user", "user.sl", include_str!("stdlib_src/user.sl")),
    ("std.cli", "cli.sl", include_str!("stdlib_src/cli.sl")),
    ("std.testing", "testing.sl", include_str!("stdlib_src/testing.sl")),
    ("std.embed", "embed.sl", include_str!("stdlib_src/embed.sl")),
    ("std.time", "time.sl", include_str!("stdlib_src/time.sl")),
    ("std.io", "io.sl", include_str!("stdlib_src/io.sl")),
    ("std.os", "os.sl", include_str!("stdlib_src/os.sl")),
];

/// The parsed [`MODULES`] (parsed once per process).
fn module_asts() -> &'static [ast::File] {
    static ASTS: OnceLock<Vec<ast::File>> = OnceLock::new();
    ASTS.get_or_init(|| {
        let _t = crate::build::phase("std-parse");
        // From `std/` on disk when installed (sema/stdfs.rs), in order (the
        // origins log keeps it); then lexed and parsed on several threads.
        let texts: Vec<std::borrow::Cow<'static, str>> = MODULES.iter().map(|(ns, name, embedded)| super::stdfs::load(ns, name, embedded)).collect();
        let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1).clamp(1, 8);
        let chunk = MODULES.len().div_ceil(n).max(1);
        let idx: Vec<usize> = (0..MODULES.len()).collect();
        std::thread::scope(|sc| {
            let hs: Vec<_> = idx.chunks(chunk).map(|is| {
                let texts = &texts;
                sc.spawn(move || is.iter().map(|&i| parse_module(i, &texts[i])).collect::<Vec<ast::File>>())
            }).collect();
            hs.into_iter().flat_map(|h| h.join().unwrap_or_else(|e| std::panic::resume_unwind(e))).collect()
        })
    })
}

/// Parses stdlib module `MODULES[i]` from `src`; panics on errors.
fn parse_module(i: usize, src: &str) -> ast::File {
    let (ns, name, _) = MODULES[i];
    let mut diags = crate::diagnostics::Diagnostics::new();
    let tokens = crate::lexer::lex(src, &mut diags);
    let file = crate::parser::parse(tokens, &mut diags);
    match file {
        Some(mut f) if !diags.has_errors() && f.namespace.to_string() == ns => {
            // Interfaces are resolved like user declarations (in
            // the first user file's scope): without spans, so the
            // IDE index records nothing for them.
            for d in &mut f.decls {
                if let ast::Decl::Interface(it) = d {
                    erase_spans(it);
                }
            }
            f
        }
        _ => {
            let errs: Vec<String> = diags
                .items
                .iter()
                .map(|d| {
                    let line = src[..(d.span.start as usize).min(src.len())].matches('\n').count() + 1;
                    format!("{name}:{line}: {}", d.message)
                })
                .collect();
            panic!("internal error: stdlib source {name} does not parse: {errs:?}")
        }
    }
}

/// Replaces every span of an interface declaration with [`Span::DUMMY`].
fn erase_spans(it: &mut ast::InterfaceDecl) {
    fn ty(t: &mut ast::TypeExpr) {
        t.span = Span::DUMMY;
        match &mut t.kind {
            ast::TypeKind::Named { path, generics } => {
                path.span = Span::DUMMY;
                for s in &mut path.segments {
                    s.span = Span::DUMMY;
                }
                generics.iter_mut().for_each(ty);
            }
            ast::TypeKind::Fallible(x) | ast::TypeKind::Pointer(x) | ast::TypeKind::Weak(x) | ast::TypeKind::Slice(x) => ty(x),
            ast::TypeKind::Map(k, v) => {
                ty(k);
                ty(v);
            }
            ast::TypeKind::Func { params, ret } => {
                params.iter_mut().for_each(ty);
                if let Some(r) = ret {
                    ty(r);
                }
            }
            ast::TypeKind::Tuple(ts) => ts.iter_mut().for_each(ty),
            _ => {}
        }
    }
    it.name.span = Span::DUMMY;
    it.span = Span::DUMMY;
    for m in &mut it.methods {
        m.name.span = Span::DUMMY;
        m.span = Span::DUMMY;
        for p in &mut m.params {
            p.name.span = Span::DUMMY;
            p.span = Span::DUMMY;
            ty(&mut p.ty);
        }
        if let Some(r) = &mut m.ret {
            ty(r);
        }
    }
    it.embeds.iter_mut().for_each(ty);
}

impl<'a> Sema<'a> {
    /// Registers the declarations of the Slang-source stdlib modules.
    pub(crate) fn register_std_sources(&mut self) {
        if self.files.is_empty() {
            return;
        }
        let saved = (self.cur_file, self.cur_ns);
        for file in module_asts() {
            let ns = self.ensure_ns(&file.namespace.to_string(), Span::DUMMY);
            self.cur_ns = ns;
            self.cur_file = 0;
            for d in &file.decls {
                let (name, t) = match d {
                    ast::Decl::Struct(s) => (&s.name.name, self.new_template(&s.name.name, TmplDecl::Struct(s), &s.generics, Vec::new(), false)),
                    ast::Decl::Func(f) => (&f.name.name, self.new_template(&f.name.name, TmplDecl::Func(f), &f.generics, Vec::new(), false)),
                    ast::Decl::Enum(e) => (&e.name.name, self.new_template(&e.name.name, TmplDecl::Enum(e), &e.generics, Vec::new(), false)),
                    ast::Decl::Interface(it) => {
                        let id = self.interfaces.len() as u32;
                        self.interfaces.push(hir::Interface {
                            name: it.name.name.clone(),
                            full_name: format!("{}.{}", self.nss[ns].path, it.name.name),
                            namespace: hir::NamespaceId(0),
                            methods: Vec::new(),
                            sealed: it.is_sealed,
                            attrs: Vec::new(),
                            exported: true,
                            file: 0,
                            span: Span::DUMMY,
                        });
                        self.iface_src.push(it);
                        self.nss[ns].members.insert(it.name.name.clone(), (Member::Interface(id), Span::DUMMY, 0));
                        // Default methods: ordinary functions of the stdlib
                        // namespace (signature and body in its scope).
                        for m in it.methods.iter().filter(|m| m.body.is_some()) {
                            assert!(
                                !m.is_static && !m.is_async && m.generics.is_empty(),
                                "internal error: default method {}.{} in a stdlib source is static, async or generic",
                                it.name.name,
                                m.name.name
                            );
                            let fid = self.new_func(m, hir::FuncKind::Free, FuncSrc::Free(m), &format!("{}.{}", it.name.name, m.name.name));
                            self.iface_defaults.insert((id, m.name.name.clone()), fid);
                            self.iface_default_owner.insert(fid.0, id);
                            self.std_fn_ns.insert(fid.0, ns);
                        }
                        continue;
                    }
                    _ => panic!("internal error: unsupported declaration in stdlib source {}", file.namespace.to_string()),
                };
                self.nss[ns].members.insert(name.clone(), (Member::Generic(t), Span::DUMMY, 0));
            }
        }
        (self.cur_file, self.cur_ns) = saved;
    }

    /// True if template `t` is a stdlib template without type parameters.
    fn is_plain_std(&self, t: u32) -> bool {
        let tm = &self.gen.templates[t as usize];
        tm.is_std && tm.params.is_empty()
    }

    /// Instantiates plain stdlib template `t` (no type parameters), or
    /// returns `None` if `t` has type parameters or is no stdlib template.
    pub(crate) fn plain_std_inst(&mut self, t: u32, span: Span) -> Option<Inst> {
        if !self.is_plain_std(t) {
            return None;
        }
        if let Some(i) = self.gen.cache.get(&(t, Vec::new())) {
            return Some(*i);
        }
        let funcs = self.funcs.len();
        let inst = self.instantiate(t, Vec::new(), span)?;
        // `Mutex<>` -> `Mutex` in names (symbols stay unique as they are).
        let fix = |s: &mut String| *s = s.replace("<>", "");
        match inst {
            Inst::Struct(s) => {
                let st = &mut self.structs[s as usize];
                fix(&mut st.name);
                fix(&mut st.full_name);
            }
            Inst::Enum(e) => {
                let en = &mut self.enums[e as usize];
                fix(&mut en.name);
                fix(&mut en.full_name);
            }
            Inst::Func(_) => {}
        }
        for f in &mut self.funcs[funcs..] {
            fix(&mut f.name);
            fix(&mut f.full_name);
        }
        Some(inst)
    }

    /// The type named by plain stdlib template `t` (`task.Mutex`).
    pub(crate) fn plain_std_type(&mut self, t: u32, span: Span) -> Option<TypeId> {
        if !self.is_plain_std(t) || !matches!(self.gen.templates[t as usize].decl, TmplDecl::Struct(_) | TmplDecl::Enum(_)) {
            return None;
        }
        Some(match self.plain_std_inst(t, span) {
            Some(Inst::Struct(s)) => self.struct_ty(s),
            Some(Inst::Enum(e)) => self.enum_ty(e),
            _ => self.tt.invalid,
        })
    }

    /// The struct or enum of plain stdlib type template `t` as a member
    /// (for paths through it: `log.Level.Debug`, `log.Context.Push`).
    pub(crate) fn plain_std_member(&mut self, t: u32, span: Span) -> Option<Member> {
        if !self.is_plain_std(t) || !matches!(self.gen.templates[t as usize].decl, TmplDecl::Struct(_) | TmplDecl::Enum(_)) {
            return None;
        }
        match self.plain_std_inst(t, span)? {
            Inst::Struct(s) => Some(Member::Struct(s)),
            Inst::Enum(e) => Some(Member::Enum(e)),
            Inst::Func(_) => None,
        }
    }

    /// The struct of plain stdlib struct template `t` (literals `task.Mutex{}`).
    pub(crate) fn plain_std_struct(&mut self, t: u32, span: Span) -> Option<u32> {
        if !matches!(self.gen.templates[t as usize].decl, TmplDecl::Struct(_)) {
            return None;
        }
        match self.plain_std_inst(t, span)? {
            Inst::Struct(s) => Some(s),
            _ => None,
        }
    }

    /// The function of plain stdlib function template `t`, as a value.
    pub(crate) fn plain_std_func_value(&mut self, t: u32, span: Span) -> Option<hir::Expr> {
        if !matches!(self.gen.templates[t as usize].decl, TmplDecl::Func(_)) {
            return None;
        }
        match self.plain_std_inst(t, span)? {
            Inst::Func(f) => {
                let ty = self.func_value_ty(f);
                Some(mk(hir::ExprKind::FuncRef(f), ty, span))
            }
            _ => None,
        }
    }

    /// `task.Channel<int>(8)`: calling a stdlib generic struct type calls
    /// its static `New` method. `None` if `t` is no stdlib struct template.
    pub(crate) fn std_type_call(&mut self, t: u32, targs: Option<&[ast::TypeExpr]>, args: &[ast::Expr], span: Span) -> Option<hir::Expr> {
        let tm = &self.gen.templates[t as usize];
        if !tm.is_std || !matches!(tm.decl, TmplDecl::Struct(_)) {
            return None;
        }
        let name = tm.name.clone();
        let sid = match targs {
            Some(ta) => {
                let Some(ts) = self.type_args(t, ta, span) else { return Some(self.invalid(span)) };
                match self.instantiate(t, ts, span) {
                    Some(Inst::Struct(s)) => s,
                    _ => return Some(self.invalid(span)),
                }
            }
            None => match self.plain_std_inst(t, span) {
                Some(Inst::Struct(s)) => s,
                _ => {
                    self.err(format!("missing type arguments for generic type {name} (write {name}<...>(...))"), span);
                    self.check_args_loose(args);
                    return Some(self.invalid(span));
                }
            },
        };
        let ty = self.struct_ty(sid);
        let new = ast::Ident { name: "New".to_string(), span };
        Some(self.type_member_call(ty, &new, args, span))
    }
}
