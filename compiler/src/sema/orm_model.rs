//! The `data` model (docs/FRAMEWORK.md section 3): entities, columns,
//! keys, relations and indexes computed at compile time from attributes
//! (`@table`, `@key`, `@column`, `@index`, `@hasMany`, ...), conventions
//! (`Id` keys, snake_case names, `<Entity>Id` foreign keys, plural table
//! names) and the fluent calls in the `Configure(m *data.ModelBuilder)`
//! method of every context, which are read from the source (they must be
//! literal chains: `m.Entity<User>().HasIndex((u) => u.Email).IsUnique()`).
//!
//! The model is built once per analysis, for every entity reachable from
//! a context (a struct embedding `data.Context`) through its `Set<T>`
//! fields and navigations; [`Sema::orm_model_json`] serialises it for the
//! migrations CLI (`zell data migration add`, `crate::datacli`).

use super::*;
use crate::hir::{AttrKind, ConstValue};
use std::rc::Rc;

/// A mapped column.
#[derive(Clone, Debug)]
pub(crate) struct ColModel {
    pub field: String,
    pub name: String,
    /// Field type as declared (`*int`, `sql.Null<int>`, `time.Time`).
    pub ty: TypeId,
    /// The value type without nullability (`int`).
    pub base: TypeId,
    /// Provider-independent type: int64, int32, int16, float, float32,
    /// bool, string, bytes, time, decimal, json, enum.
    pub kind: &'static str,
    pub sql_type: String,
    pub nullable: bool,
    pub key: bool,
    pub auto: bool,
    pub max_len: i64,
    pub default: String,
    pub token: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum NavKind {
    /// `[]E` with the foreign key on `E`.
    Many,
    /// `*E` with the foreign key on `E`.
    One,
    /// `*P` with the foreign key on this entity.
    Belongs,
    /// `[]E` through a join table.
    ManyMany,
}

/// A navigation field.
#[derive(Clone, Debug)]
pub(crate) struct NavModel {
    pub field: String,
    pub kind: NavKind,
    /// The other entity (struct type).
    pub target: TypeId,
    /// The field holds pointers (`[]*E`, `*E`), not values.
    pub ptr: bool,
    /// The field is a slice.
    pub many: bool,
    /// Foreign key field: on the target (Many, One) or on this entity (Belongs).
    pub fk: String,
    /// Join table, its column for this entity and for the target (ManyMany).
    pub through: String,
    pub owner_col: String,
    pub target_col: String,
}

#[derive(Clone, Debug)]
pub(crate) struct IdxModel {
    pub name: String,
    pub cols: Vec<String>,
    pub unique: bool,
}

#[derive(Clone, Debug)]
pub(crate) struct FkModel {
    pub name: String,
    pub cols: Vec<String>,
    pub ref_table: String,
    pub ref_cols: Vec<String>,
    pub on_delete: String,
}

/// An entity (or a many-to-many join table: `sid == u32::MAX`).
#[derive(Clone, Debug)]
pub(crate) struct EntModel {
    pub ty: TypeId,
    pub sid: u32,
    pub name: String,
    pub table: String,
    pub cols: Vec<ColModel>,
    pub key: Option<usize>,
    pub token: Option<usize>,
    pub navs: Vec<NavModel>,
    pub indexes: Vec<IdxModel>,
    pub fks: Vec<FkModel>,
}

impl EntModel {
    pub fn col(&self, field: &str) -> Option<&ColModel> {
        self.cols.iter().find(|c| c.field == field)
    }

    pub fn nav(&self, field: &str) -> Option<&NavModel> {
        self.navs.iter().find(|n| n.field == field)
    }

    pub fn key_col(&self) -> Option<&ColModel> {
        self.key.map(|k| &self.cols[k])
    }
}

/// Fluent configuration of one entity (from `Configure`).
#[derive(Default, Clone, Debug)]
pub(crate) struct Fluent {
    pub table: Option<String>,
    pub key: Option<String>,
    pub ignore: Vec<String>,
    /// field -> settings
    pub props: Vec<(String, PropCfg)>,
    pub indexes: Vec<(Vec<String>, bool, String)>,
    /// navigation field -> (foreign key field, join table)
    pub navs: Vec<(String, String, String, &'static str)>,
}

#[derive(Default, Clone, Debug)]
pub(crate) struct PropCfg {
    pub column: Option<String>,
    pub max_len: Option<i64>,
    pub required: bool,
    pub default: Option<String>,
    pub token: bool,
    pub sql_type: Option<String>,
    pub auto: Option<bool>,
}

/// State of the ORM expansion.
#[derive(Default)]
pub(crate) struct OrmState {
    pub tmpls: HashMap<String, u32>,
    pub models: HashMap<TypeId, Rc<EntModel>>,
    pub failed: Vec<TypeId>,
    pub fluent: Option<HashMap<TypeId, Fluent>>,
    pub universe: bool,
    /// Contexts (struct ids) and their sets (field, entity type).
    pub contexts: Vec<(u32, Vec<(String, TypeId)>)>,
    pub joins: Vec<Rc<EntModel>>,
    pub counter: usize,
}

/// snake_case of a field or type name (`UserID` -> `user_id`).
pub(crate) fn snake(name: &str) -> String {
    let b: Vec<char> = name.chars().collect();
    let mut out = String::new();
    for i in 0..b.len() {
        let c = b[i];
        if c.is_ascii_uppercase() {
            let prev = if i > 0 { b[i - 1] } else { ' ' };
            let next = b.get(i + 1).copied().unwrap_or(' ');
            if i > 0 && (prev.is_ascii_lowercase() || prev.is_ascii_digit() || (prev.is_ascii_uppercase() && next.is_ascii_lowercase())) {
                out.push('_');
            }
            out.push(c.to_ascii_lowercase());
        } else {
            out.push(c);
        }
    }
    out
}

/// English plural of a snake_case name (`user` -> `users`, `category` ->
/// `categories`, `box` -> `boxes`).
pub(crate) fn plural(s: &str) -> String {
    let (head, last) = match s.rfind('_') {
        Some(i) => (&s[..=i], &s[i + 1..]),
        None => ("", s),
    };
    let p = if last.ends_with('s') || last.ends_with('x') || last.ends_with('z') || last.ends_with("ch") || last.ends_with("sh") {
        format!("{last}es")
    } else if last.ends_with('y') && !last.ends_with("ay") && !last.ends_with("ey") && !last.ends_with("oy") && !last.ends_with("uy") {
        format!("{}ies", &last[..last.len() - 1])
    } else if last == "person" {
        "people".into()
    } else {
        format!("{last}s")
    };
    format!("{head}{p}")
}

impl<'a> Sema<'a> {
    /// `(template name, type arguments)` if struct `sid` is an instance of
    /// a data template.
    pub(crate) fn orm_inst(&self, sid: u32) -> Option<(String, Vec<TypeId>)> {
        if let Some((t, args)) = self.gen.inst_structs.get(&sid) {
            let tm = &self.gen.templates[*t as usize];
            if tm.is_std && self.nss[tm.ns].path == "data" {
                return Some((tm.name.clone(), args.clone()));
            }
            return None;
        }
        let full = &self.structs[sid as usize].full_name;
        full.strip_prefix("data.").filter(|n| !n.contains('.') && !n.contains('<')).map(|n| (n.to_string(), Vec::new()))
    }

    /// Name of the data attribute of `a`.
    pub(crate) fn orm_attr(&self, a: &hir::Attr) -> Option<String> {
        match a.kind {
            AttrKind::User(s) => self.structs[s as usize].full_name.strip_prefix("data.").map(|x| x.to_string()),
            _ => None,
        }
    }

    fn attr_arg<'x>(a: &'x hir::Attr, key: &str) -> Option<&'x ConstValue> {
        a.args.iter().find(|(k, _)| k == key).map(|x| &x.1)
    }

    fn attr_s(a: &hir::Attr, key: &str) -> String {
        match Self::attr_arg(a, key) {
            Some(ConstValue::Str(s)) => s.clone(),
            _ => String::new(),
        }
    }

    fn attr_b(a: &hir::Attr, key: &str) -> bool {
        matches!(Self::attr_arg(a, key), Some(ConstValue::Bool(true)))
    }

    /// Whether struct `sid` embeds `data.Context` (a context).
    pub(crate) fn orm_is_context(&self, sid: u32) -> bool {
        self.structs[sid as usize].fields.iter().any(|f| f.embedded && self.struct_of(f.ty).is_some_and(|s| self.structs[s as usize].full_name == "data.Context"))
    }

    /// The entity type of a `Set<T>` field type.
    pub(crate) fn orm_set_arg(&self, t: TypeId) -> Option<TypeId> {
        let Type::Struct(d) = *self.tt.get(t) else { return None };
        match self.orm_inst(d.0) {
            Some((n, args)) if n == "Set" => args.first().copied(),
            _ => None,
        }
    }

    // -- universe ------------------------------------------------------------------

    /// Builds the model of every context's entities (once).
    pub(crate) fn orm_universe(&mut self) {
        // Unit mode: the model belongs to every file that needs it.
        let node = self.unit_memo(super::units::MemoKey::OrmUniverse);
        if self.orm.universe {
            return;
        }
        let prev = self.unit_enter(node);
        // Built once for the whole program: its diagnostics do not depend
        // on the code that needs it first (an instance being muted, say).
        let saved = (std::mem::take(&mut self.gen.mute), self.gen.note.take());
        self.orm_universe_build();
        (self.gen.mute, self.gen.note) = saved;
        self.unit_leave(prev);
    }

    fn orm_universe_build(&mut self) {
        self.orm.universe = true;
        let mut contexts = Vec::new();
        for sid in 0..self.structs.len() as u32 {
            if crate::sema::stdlib::is_std_namespace(&self.structs[sid as usize].full_name) || self.gen.inst_structs.contains_key(&sid) || !self.orm_is_context(sid) {
                continue;
            }
            let sets: Vec<(String, TypeId)> = self.structs[sid as usize].fields.iter().filter_map(|f| self.orm_set_arg(f.ty).map(|t| (f.name.clone(), t))).collect();
            contexts.push((sid, sets));
        }
        self.orm.contexts = contexts.clone();
        self.orm_read_fluent();
        let mut todo: Vec<TypeId> = contexts.iter().flat_map(|(_, s)| s.iter().map(|x| x.1)).collect();
        let mut seen = Vec::new();
        while let Some(t) = todo.pop() {
            if seen.contains(&t) {
                continue;
            }
            seen.push(t);
            if let Some(m) = self.orm_build(t) {
                for n in &m.navs {
                    todo.push(n.target);
                }
            }
        }
        self.orm_relations();
    }

    /// The model of entity type `t` (an error was reported if `None`).
    pub(crate) fn orm_model(&mut self, t: TypeId) -> Option<Rc<EntModel>> {
        self.orm_universe();
        let node = self.unit_memo(super::units::MemoKey::OrmModel(t));
        if let Some(m) = self.orm.models.get(&t) {
            return Some(m.clone());
        }
        if self.orm.failed.contains(&t) {
            return None;
        }
        let prev = self.unit_enter(node);
        let saved = (std::mem::take(&mut self.gen.mute), self.gen.note.take());
        let m = self.orm_model_build(t);
        (self.gen.mute, self.gen.note) = saved;
        self.unit_leave(prev);
        m
    }

    fn orm_model_build(&mut self, t: TypeId) -> Option<Rc<EntModel>> {
        let m = self.orm_build(t)?;
        // relations of an entity outside every context
        let targets: Vec<TypeId> = m.navs.iter().map(|n| n.target).collect();
        for x in targets {
            if !self.orm.models.contains_key(&x) {
                self.orm_build(x);
            }
        }
        self.orm_relations();
        self.orm.models.get(&t).cloned()
    }

    /// Classifies a column type: (kind, value type, nullable). `None` if
    /// the type is not a scalar column type.
    pub(crate) fn orm_scalar(&self, t: TypeId) -> Option<(&'static str, TypeId, bool)> {
        let base = |s: &Self, t: TypeId| -> Option<&'static str> {
            Some(match s.tt.get(t) {
                Type::Int(k) => match format!("{k:?}").as_str() {
                    "I32" | "U16" => "int32",
                    "I16" | "I8" | "U8" => "int16",
                    _ => "int64",
                },
                Type::Float64 => "float",
                Type::Float32 => "float32",
                Type::Bool => "bool",
                Type::String => "string",
                Type::Slice(e) if matches!(s.tt.get(*e), Type::Int(k) if format!("{k:?}") == "U8") => "bytes",
                Type::Enum(d) if s.enums[d.0 as usize].is_plain => "enum",
                Type::Struct(d) => match s.structs[d.0 as usize].full_name.as_str() {
                    "time.Time" => "time",
                    "math.big.Decimal" => "decimal",
                    "encoding.json.Value" => "json",
                    _ => return None,
                },
                _ => return None,
            })
        };
        if let Some(k) = base(self, t) {
            return Some((k, t, k == "json"));
        }
        match self.tt.get(t) {
            Type::Pointer(x) => {
                let x = *x;
                let k = base(self, x)?;
                (k != "enum").then_some((k, x, true))
            }
            Type::Struct(d) => {
                let (tm, args) = self.gen.inst_structs.get(&d.0)?;
                let tm = &self.gen.templates[*tm as usize];
                if tm.is_std && tm.name == "Null" && self.nss[tm.ns].path == "database.sql" {
                    let k = base(self, args[0])?;
                    return (k != "enum").then_some((k, args[0], true));
                }
                None
            }
            _ => None,
        }
    }

    /// An entity-like struct type (navigation target): a user struct with
    /// a key (`Id`, `<Type>Id` or `@key`).
    fn orm_entity_like(&self, t: TypeId) -> bool {
        match self.tt.get(t) {
            Type::Struct(d) => {
                let st = &self.structs[d.0 as usize];
                !crate::sema::stdlib::is_std_namespace(&st.full_name)
                    && self.orm_scalar(t).is_none()
                    && st.fields.iter().any(|f| f.name == "Id" || f.name == format!("{}Id", st.name) || f.attrs.iter().any(|a| self.orm_attr(a).as_deref() == Some("key")))
            }
            _ => false,
        }
    }

    fn orm_fail(&mut self, t: TypeId, file: usize, msg: String, span: Span) {
        let saved = self.cur_file;
        self.cur_file = file;
        self.err(msg, span);
        self.cur_file = saved;
        if !self.orm.failed.contains(&t) {
            self.orm.failed.push(t);
        }
    }

    /// Builds the model of entity `t` (without the foreign keys of
    /// relations, see [`Self::orm_relations`]).
    fn orm_build(&mut self, t: TypeId) -> Option<Rc<EntModel>> {
        if let Some(m) = self.orm.models.get(&t) {
            return Some(m.clone());
        }
        if self.orm.failed.contains(&t) {
            return None;
        }
        let Type::Struct(d) = *self.tt.get(t) else {
            let tn = self.tn(t);
            let file = self.cur_file;
            self.orm_fail(t, file, format!("data: {tn} is not a struct and cannot be an entity"), Span::DUMMY);
            return None;
        };
        let sid = d.0;
        let st = &self.structs[sid as usize];
        let (name, file, sspan) = (st.name.clone(), st.file, st.span);
        let sattrs = st.attrs.clone();
        let fields = st.fields.clone();
        let fl = self.orm.fluent.as_ref().and_then(|f| f.get(&t)).cloned().unwrap_or_default();
        let mut table = snake(&name);
        table = plural(&table);
        for a in &sattrs {
            if self.orm_attr(a).as_deref() == Some("table") {
                table = Self::attr_s(a, "name");
            }
        }
        if let Some(tb) = &fl.table {
            table = tb.clone();
        }
        let mut cols = Vec::new();
        let mut navs = Vec::new();
        let mut indexes = Vec::new();
        let mut key_field: Option<String> = fl.key.clone();
        let mut ok = true;
        for f in &fields {
            if f.embedded || fl.ignore.contains(&f.name) {
                continue;
            }
            let attrs: Vec<(String, hir::Attr)> = f.attrs.iter().filter_map(|a| self.orm_attr(a).map(|n| (n, a.clone()))).collect();
            let has = |n: &str| attrs.iter().find(|(x, _)| x == n).map(|x| x.1.clone());
            if has("ignore").is_some() {
                continue;
            }
            let pc = fl.props.iter().find(|(n, _)| *n == f.name).map(|x| x.1.clone()).unwrap_or_default();
            if let Some((kind, base, nullable)) = self.orm_scalar(f.ty) {
                let mut c = ColModel {
                    field: f.name.clone(),
                    name: snake(&f.name),
                    ty: f.ty,
                    base,
                    kind,
                    sql_type: String::new(),
                    nullable,
                    key: false,
                    auto: false,
                    max_len: 0,
                    default: String::new(),
                    token: false,
                };
                if let Some(a) = has("column") {
                    let n = Self::attr_s(&a, "name");
                    if !n.is_empty() {
                        c.name = n;
                    }
                    c.sql_type = Self::attr_s(&a, "type");
                }
                if let Some(a) = has("key") {
                    if key_field.is_some() && key_field.as_deref() != Some(&f.name) {
                        self.orm_fail(t, file, format!("data: entity {name} has more than one @key (composite keys are not supported)"), a.span);
                        ok = false;
                    }
                    key_field = Some(f.name.clone());
                    if matches!(Self::attr_arg(&a, "generated"), Some(ConstValue::Bool(false))) {
                        c.auto = false;
                        c.token = false;
                        c.max_len = -1; // marker: never generated
                    }
                }
                if has("autoincrement").is_some() {
                    if !kind.starts_with("int") {
                        self.orm_fail(t, file, format!("data: @autoincrement on {name}.{} needs an integer field", f.name), f.span);
                        ok = false;
                    }
                    c.auto = true;
                }
                if let Some(a) = has("maxLength") {
                    if let Some(ConstValue::Int(n)) = Self::attr_arg(&a, "length") {
                        c.max_len = *n as i64;
                    }
                    if kind != "string" {
                        self.orm_fail(t, file, format!("data: @maxLength on {name}.{}: only strings have a maximum length", f.name), a.span);
                        ok = false;
                    }
                }
                if has("required").is_some() || pc.required {
                    c.nullable = false;
                }
                if let Some(a) = has("default") {
                    c.default = Self::attr_s(&a, "sql");
                }
                if has("concurrencyToken").is_some() || pc.token {
                    c.token = true;
                }
                if let Some(a) = has("index") {
                    let n = Self::attr_s(&a, "name");
                    indexes.push((vec![f.name.clone()], Self::attr_b(&a, "unique"), n));
                }
                if let Some(n) = &pc.column {
                    c.name = n.clone();
                }
                if let Some(n) = pc.max_len {
                    c.max_len = n;
                }
                if let Some(d) = &pc.default {
                    c.default = d.clone();
                }
                if let Some(s) = &pc.sql_type {
                    c.sql_type = s.clone();
                }
                if let Some(a) = pc.auto {
                    c.auto = a;
                }
                cols.push(c);
                continue;
            }
            // navigations
            let (target, ptr, many) = match self.tt.get(f.ty).clone() {
                Type::Slice(e) => match self.tt.get(e).clone() {
                    Type::Pointer(x) => (x, true, true),
                    _ => (e, false, true),
                },
                Type::Pointer(x) => (x, true, false),
                _ => (f.ty, false, false),
            };
            if !self.orm_entity_like(target) {
                let tn = self.tn(f.ty);
                self.orm_fail(t, file, format!("data: field {name}.{} of type {tn} cannot be mapped to a column (use @ignore, or a navigation to an entity)", f.name), f.span);
                ok = false;
                continue;
            }
            let tname = self.struct_of(target).map(|s| self.structs[s as usize].name.clone()).unwrap_or_default();
            let flnav = fl.navs.iter().find(|n| n.0 == f.name).cloned();
            let mut nav = NavModel {
                field: f.name.clone(),
                kind: if many { NavKind::Many } else { NavKind::Belongs },
                target,
                ptr,
                many,
                fk: String::new(),
                through: String::new(),
                owner_col: String::new(),
                target_col: String::new(),
            };
            let mut explicit = false;
            for (n, a) in &attrs {
                match n.as_str() {
                    "hasMany" | "hasOne" | "belongsTo" | "manyToMany" => {
                        explicit = true;
                        nav.kind = match n.as_str() {
                            "hasMany" => NavKind::Many,
                            "hasOne" => NavKind::One,
                            "belongsTo" => NavKind::Belongs,
                            _ => NavKind::ManyMany,
                        };
                        nav.fk = Self::attr_s(a, "foreignKey");
                        nav.through = Self::attr_s(a, "through");
                        if (nav.kind == NavKind::Many || nav.kind == NavKind::ManyMany) != many {
                            let want = if many { "a slice" } else { "a pointer or a struct" };
                            self.orm_fail(t, file, format!("data: @{n} on {name}.{}: the field must be {}", f.name, if many { "a pointer or a struct" } else { want }), a.span);
                            ok = false;
                        }
                    }
                    _ => {}
                }
            }
            if let Some((_, fk, through, kind)) = flnav {
                if !fk.is_empty() {
                    nav.fk = fk;
                }
                if !through.is_empty() {
                    nav.through = through;
                }
                match kind {
                    "many" => nav.kind = NavKind::Many,
                    "one" => nav.kind = NavKind::One,
                    "belongs" => nav.kind = NavKind::Belongs,
                    "manymany" => nav.kind = NavKind::ManyMany,
                    _ => {}
                }
                explicit = true;
            }
            if !explicit && many {
                // a collection on both sides without a foreign key: many-to-many
                let fk = format!("{name}Id");
                if let Some(ts) = self.struct_of(target) {
                    let tf = &self.structs[ts as usize].fields;
                    let back = tf.iter().any(|x| match self.tt.get(x.ty) {
                        Type::Slice(e) => *e == t || matches!(self.tt.get(*e), Type::Pointer(p) if *p == t),
                        _ => false,
                    });
                    if back && !tf.iter().any(|x| x.name == fk) {
                        nav.kind = NavKind::ManyMany;
                    }
                }
            }
            if !explicit && !many {
                // reference: belongsTo if this entity has <Field>Id, else hasOne
                let fk = format!("{}Id", f.name);
                if fields.iter().any(|x| x.name == fk) {
                    nav.kind = NavKind::Belongs;
                } else {
                    nav.kind = NavKind::One;
                }
            }
            if nav.fk.is_empty() {
                nav.fk = match nav.kind {
                    NavKind::Belongs => format!("{}Id", f.name),
                    _ => format!("{name}Id"),
                };
            }
            if nav.kind == NavKind::ManyMany {
                if nav.through.is_empty() {
                    // both sides of a two-way many-to-many name the same table
                    let (x, y) = if name <= tname { (&name, &tname) } else { (&tname, &name) };
                    nav.through = format!("{}_{}", snake(x), plural(&snake(y)));
                }
                nav.owner_col = format!("{}_id", snake(&name));
                nav.target_col = format!("{}_id", snake(&tname));
                if nav.owner_col == nav.target_col {
                    nav.target_col = format!("{}_id", snake(&f.name));
                }
            }
            navs.push(nav);
        }
        for (cs, u, n) in &fl.indexes {
            indexes.push((cs.clone(), *u, n.clone()));
        }
        // key
        if key_field.is_none() {
            let conv = [String::from("Id"), format!("{name}Id")];
            key_field = conv.iter().find(|k| cols.iter().any(|c| c.field == **k)).cloned();
        }
        let key = key_field.as_ref().and_then(|k| cols.iter().position(|c| c.field == *k));
        if key_field.is_some() && key.is_none() {
            self.orm_fail(t, file, format!("data: the key {} of entity {name} is not a mapped column", key_field.clone().unwrap()), sspan);
            ok = false;
        }
        if let Some(k) = key {
            let never = cols[k].max_len == -1;
            if never {
                cols[k].max_len = 0;
            }
            let c = &mut cols[k];
            c.key = true;
            if c.nullable {
                self.orm_fail(t, file, format!("data: the key {name}.{} cannot be nullable", c.field), sspan);
                ok = false;
            }
            if !never && c.kind.starts_with("int") && fl.props.iter().find(|(n, _)| *n == c.field).and_then(|p| p.1.auto) != Some(false) {
                c.auto = true;
            }
        }
        let token = cols.iter().position(|c| c.token);
        if cols.iter().filter(|c| c.token).count() > 1 {
            self.orm_fail(t, file, format!("data: entity {name} has more than one concurrency token"), sspan);
            ok = false;
        }
        if cols.iter().filter(|c| c.auto).count() > 1 || cols.iter().any(|c| c.auto && !c.key) {
            self.orm_fail(t, file, format!("data: only the key of {name} can be generated (@autoincrement)"), sspan);
            ok = false;
        }
        let mut idx = Vec::new();
        for (fs, unique, n) in indexes {
            let mut cnames = Vec::new();
            for f in &fs {
                match cols.iter().find(|c| c.field == *f) {
                    Some(c) => cnames.push(c.name.clone()),
                    None => {
                        self.orm_fail(t, file, format!("data: index on {name}.{f}: not a mapped column"), sspan);
                        ok = false;
                    }
                }
            }
            let iname = if n.is_empty() { format!("{}_{}_{}", if unique { "ux" } else { "ix" }, table, cnames.join("_")) } else { n };
            idx.push(IdxModel { name: iname, cols: cnames, unique });
        }
        if !ok {
            return None;
        }
        let m = Rc::new(EntModel { ty: t, sid, name, table, cols, key, token, navs, indexes: idx, fks: Vec::new() });
        self.orm.models.insert(t, m.clone());
        Some(m)
    }

    /// Checks the relations of every model and derives the foreign keys,
    /// their indexes and the join tables.
    fn orm_relations(&mut self) {
        let mut models: Vec<Rc<EntModel>> = self.orm.models.values().cloned().collect();
        models.sort_by_key(|m| m.sid);
        let mut fks: HashMap<TypeId, Vec<FkModel>> = HashMap::new();
        let mut joins: Vec<Rc<EntModel>> = Vec::new();
        let mut bad = Vec::new();
        for m in &models {
            for n in &m.navs {
                let Some(target) = self.orm.models.get(&n.target).cloned() else { continue };
                let file = self.structs[m.sid as usize].file;
                let span = self.structs[m.sid as usize].fields.iter().find(|f| f.name == n.field).map(|f| f.span).unwrap_or(Span::DUMMY);
                let (dep, principal) = match n.kind {
                    NavKind::Belongs => (m.clone(), target.clone()),
                    NavKind::Many | NavKind::One => (target.clone(), m.clone()),
                    NavKind::ManyMany => {
                        let (Some(ok), Some(tk)) = (m.key_col(), target.key_col()) else {
                            self.orm_fail(m.ty, file, format!("data: many-to-many {}.{} needs keys on both entities", m.name, n.field), span);
                            bad.push(m.ty);
                            continue;
                        };
                        if joins.iter().any(|j| j.table == n.through) {
                            continue;
                        }
                        let mk_col = |name: &str, k: &ColModel| ColModel { field: name.into(), name: name.into(), ty: k.base, base: k.base, kind: k.kind, sql_type: String::new(), nullable: false, key: true, auto: false, max_len: k.max_len, default: String::new(), token: false };
                        let cols = vec![mk_col(&n.owner_col, ok), mk_col(&n.target_col, tk)];
                        let fks = vec![
                            FkModel { name: format!("fk_{}_{}_{}", n.through, m.table, n.owner_col), cols: vec![n.owner_col.clone()], ref_table: m.table.clone(), ref_cols: vec![ok.name.clone()], on_delete: "CASCADE".into() },
                            FkModel { name: format!("fk_{}_{}_{}", n.through, target.table, n.target_col), cols: vec![n.target_col.clone()], ref_table: target.table.clone(), ref_cols: vec![tk.name.clone()], on_delete: "CASCADE".into() },
                        ];
                        let indexes = vec![IdxModel { name: format!("ix_{}_{}", n.through, n.target_col), cols: vec![n.target_col.clone()], unique: false }];
                        joins.push(Rc::new(EntModel { ty: TypeId(u32::MAX), sid: u32::MAX, name: String::new(), table: n.through.clone(), cols, key: None, token: None, navs: Vec::new(), indexes, fks }));
                        continue;
                    }
                };
                let Some(fkc) = dep.col(&n.fk).cloned() else {
                    let attr = match n.kind {
                        NavKind::Belongs => "@belongsTo",
                        NavKind::One => "@hasOne",
                        _ => "@hasMany",
                    };
                    self.orm_fail(m.ty, file, format!("data: navigation {}.{}: {} has no foreign key field {} (add it, or name it with {attr}(foreignKey: \"...\"))", m.name, n.field, dep.name, n.fk), span);
                    bad.push(m.ty);
                    continue;
                };
                let Some(pk) = principal.key_col().cloned() else {
                    self.orm_fail(m.ty, file, format!("data: navigation {}.{}: {} has no key", m.name, n.field, principal.name), span);
                    bad.push(m.ty);
                    continue;
                };
                if fkc.base != pk.base {
                    let (a, b) = (self.tn(fkc.base), self.tn(pk.base));
                    self.orm_fail(m.ty, file, format!("data: navigation {}.{}: foreign key {}.{} has type {a}, the key of {} has type {b}", m.name, n.field, dep.name, fkc.field, principal.name), span);
                    bad.push(m.ty);
                    continue;
                }
                let list = fks.entry(dep.ty).or_default();
                if !list.iter().any(|f| f.cols == vec![fkc.name.clone()]) {
                    list.push(FkModel {
                        name: format!("fk_{}_{}_{}", dep.table, principal.table, fkc.name),
                        cols: vec![fkc.name.clone()],
                        ref_table: principal.table.clone(),
                        ref_cols: vec![pk.name.clone()],
                        on_delete: if fkc.nullable { "SET NULL".into() } else { "CASCADE".into() },
                    });
                }
            }
        }
        for (t, list) in fks {
            if let Some(m) = self.orm.models.get(&t).cloned() {
                let mut mm = (*m).clone();
                for fk in list {
                    if !mm.indexes.iter().any(|i| i.cols.first() == fk.cols.first()) {
                        mm.indexes.push(IdxModel { name: format!("ix_{}_{}", mm.table, fk.cols.join("_")), cols: fk.cols.clone(), unique: false });
                    }
                    mm.fks.push(fk);
                }
                self.orm.models.insert(t, Rc::new(mm));
            }
        }
        for t in bad {
            self.orm.models.remove(&t);
        }
        for j in joins {
            if !self.orm.joins.iter().any(|x| x.table == j.table) {
                self.orm.joins.push(j);
            }
        }
    }

    // -- fluent configuration ----------------------------------------------------------

    /// Reads the `Configure` methods of every context.
    fn orm_read_fluent(&mut self) {
        let mut out: HashMap<TypeId, Fluent> = HashMap::new();
        let ctxs: Vec<u32> = self.orm.contexts.iter().map(|c| c.0).collect();
        for sid in ctxs {
            let decl = self.struct_src[sid as usize];
            let file = self.structs[sid as usize].file;
            let Some(m) = decl.methods.iter().find(|m| m.name.name == "Configure") else { continue };
            let Some(body) = &m.body else { continue };
            let Some(p) = m.params.first() else { continue };
            let pname = p.name.name.clone();
            let mut vars: Vec<(String, TypeId)> = Vec::new();
            self.gen.mute += 1;
            self.in_file(file, |s| {
                for st in &body.stmts {
                    match &st.kind {
                        ast::StmtKind::Expr(e) => {
                            s.orm_fluent_chain(e, &pname, &vars, &mut out);
                        }
                        ast::StmtKind::Var(v) => {
                            if let (Some(init), [name]) = (&v.init, v.names.as_slice()) {
                                if let Some(t) = s.orm_fluent_chain(init, &pname, &vars, &mut out) {
                                    vars.push((name.name.clone(), t));
                                }
                            }
                        }
                        _ => {}
                    }
                }
            });
            self.gen.mute -= 1;
        }
        self.orm.fluent = Some(out);
    }

    /// One `m.Entity<X>().A(...).B(...)` chain (or `e.A(...)` with `e` a
    /// variable bound to an entity builder); returns the entity type.
    fn orm_fluent_chain(&mut self, e: &ast::Expr, m: &str, vars: &[(String, TypeId)], out: &mut HashMap<TypeId, Fluent>) -> Option<TypeId> {
        // flatten the chain, innermost first
        let mut calls: Vec<(String, Option<&[ast::TypeExpr]>, &[ast::Expr])> = Vec::new();
        let mut cur = expr::strip_parens(e);
        let base_name;
        loop {
            match &cur.kind {
                ast::ExprKind::Call { callee, args } => {
                    let c = expr::strip_parens(callee);
                    let (b, name, targs) = match &c.kind {
                        ast::ExprKind::Field { base, name } => (base, name, None),
                        ast::ExprKind::TypeArgs { base, args: ta } => match &expr::strip_parens(base).kind {
                            ast::ExprKind::Field { base, name } => (base, name, Some(ta.as_slice())),
                            _ => return None,
                        },
                        _ => return None,
                    };
                    calls.push((name.name.clone(), targs, args.as_slice()));
                    cur = expr::strip_parens(b);
                }
                ast::ExprKind::Ident(id) => {
                    base_name = id.name.clone();
                    break;
                }
                _ => return None,
            }
        }
        calls.reverse();
        let mut it = calls.into_iter();
        let ety = if base_name == m {
            let (n, targs, _) = it.next()?;
            if n != "Entity" {
                return None;
            }
            let ta = targs?;
            if ta.len() != 1 {
                return None;
            }
            self.resolve_type(&ta[0])
        } else {
            vars.iter().find(|v| v.0 == base_name)?.1
        };
        if self.tt.is_invalid(ety) {
            return None;
        }
        let f = out.entry(ety).or_default();
        // current target: 0 entity, 1 property, 2 index, 3 navigation
        let mut target = 0;
        let mut prop = String::new();
        let mut nav = String::new();
        let str_arg = |args: &[ast::Expr]| -> String {
            match args.first().map(|a| &expr::strip_parens(a).kind) {
                Some(ast::ExprKind::Str(s)) => s.clone(),
                _ => String::new(),
            }
        };
        for (n, _, args) in it {
            let fields = args.first().map(|a| lambda_fields(a)).unwrap_or_default();
            match (target, n.as_str()) {
                (0, "ToTable") => f.table = Some(str_arg(args)),
                (0, "HasKey") => f.key = fields.first().cloned(),
                (0, "Ignore") => f.ignore.extend(fields),
                (0, "Property") => {
                    target = 1;
                    prop = fields.first().cloned().unwrap_or_default();
                }
                (0, "HasIndex") => {
                    target = 2;
                    f.indexes.push((fields, false, String::new()));
                }
                (0, "HasMany" | "HasOne" | "BelongsTo" | "HasManyToMany") => {
                    target = 3;
                    nav = fields.first().cloned().unwrap_or_default();
                    let kind = match n.as_str() {
                        "HasMany" => "many",
                        "HasOne" => "one",
                        "BelongsTo" => "belongs",
                        _ => "manymany",
                    };
                    f.navs.push((nav.clone(), String::new(), String::new(), kind));
                }
                (1, _) => {
                    if !f.props.iter().any(|p| p.0 == prop) {
                        f.props.push((prop.clone(), PropCfg::default()));
                    }
                    let p = &mut f.props.iter_mut().find(|p| p.0 == prop).unwrap().1;
                    match n.as_str() {
                        "HasColumnName" => p.column = Some(str_arg(args)),
                        "HasMaxLength" => {
                            if let Some(ast::ExprKind::Int(v)) = args.first().map(|a| &expr::strip_parens(a).kind) {
                                p.max_len = Some(*v as i64);
                            }
                        }
                        "IsRequired" => p.required = true,
                        "HasDefaultSql" => p.default = Some(str_arg(args)),
                        "IsConcurrencyToken" => p.token = true,
                        "HasColumnType" => p.sql_type = Some(str_arg(args)),
                        "ValueGeneratedOnAdd" => p.auto = Some(true),
                        "ValueGeneratedNever" => p.auto = Some(false),
                        _ => {}
                    }
                }
                (2, "IsUnique") => {
                    if let Some(x) = f.indexes.last_mut() {
                        x.1 = true;
                    }
                }
                (2, "HasName" | "HasDatabaseName") => {
                    if let Some(x) = f.indexes.last_mut() {
                        x.2 = str_arg(args);
                    }
                }
                (3, "WithForeignKey") => {
                    if let Some(x) = f.navs.iter_mut().find(|x| x.0 == nav) {
                        x.1 = str_arg(args);
                    }
                }
                (3, "UsingTable") => {
                    if let Some(x) = f.navs.iter_mut().find(|x| x.0 == nav) {
                        x.2 = str_arg(args);
                    }
                }
                _ => {}
            }
        }
        Some(ety)
    }

    // -- snapshot -----------------------------------------------------------------------

    /// The model of every context as JSON (the migrations snapshot format):
    /// `{"version": 1, "tables": [{"name", "entity", "columns": [...],
    /// "indexes": [...], "foreignKeys": [...]}]}` with tables ordered so
    /// that referenced tables come first.
    pub(crate) fn orm_model_json(&mut self) -> String {
        self.orm_universe();
        let mut tables = self.orm_tables_ordered();
        tables.dedup_by(|a, b| a.table == b.table);
        let mut ctxs = Vec::new();
        for (sid, sets) in &self.orm.contexts {
            ctxs.push(serde_json::json!({
                "name": self.structs[*sid as usize].name,
                "namespace": self.structs[*sid as usize].full_name.rsplit_once('.').map(|x| x.0.to_string()).unwrap_or_default(),
                "sets": sets.iter().map(|(f, t)| serde_json::json!({"field": f, "entity": self.tn(*t)})).collect::<Vec<_>>(),
            }));
        }
        let tjs: Vec<serde_json::Value> = tables.iter().map(|m| table_json(m)).collect();
        let v = serde_json::json!({"version": 1, "contexts": ctxs, "tables": tjs});
        serde_json::to_string_pretty(&v).unwrap()
    }

    /// Every table of the model (entities of all contexts, join tables),
    /// referenced tables first.
    pub(crate) fn orm_tables_ordered(&mut self) -> Vec<Rc<EntModel>> {
        let mut all: Vec<Rc<EntModel>> = self.orm.models.values().cloned().collect();
        all.sort_by_key(|m| m.sid);
        all.extend(self.orm.joins.iter().cloned());
        order_tables(all)
    }
}

/// Orders tables so that every table comes after the tables its foreign
/// keys reference (cycles keep their original order).
pub(crate) fn order_tables(all: Vec<Rc<EntModel>>) -> Vec<Rc<EntModel>> {
    let mut out: Vec<Rc<EntModel>> = Vec::new();
    let mut rest = all;
    while !rest.is_empty() {
        let pos = rest.iter().position(|m| m.fks.iter().all(|f| f.ref_table == m.table || out.iter().any(|o| o.table == f.ref_table) || !rest.iter().any(|r| r.table == f.ref_table))).unwrap_or(0);
        out.push(rest.remove(pos));
    }
    out
}

fn table_json(m: &EntModel) -> serde_json::Value {
    serde_json::json!({
        "name": m.table,
        "entity": m.name,
        "columns": m.cols.iter().map(|c| serde_json::json!({
            "name": c.name, "type": c.kind, "sqlType": c.sql_type, "nullable": c.nullable,
            "key": c.key, "auto": c.auto, "maxLength": c.max_len, "default": c.default,
        })).collect::<Vec<_>>(),
        "indexes": m.indexes.iter().map(|i| serde_json::json!({"name": i.name, "columns": i.cols, "unique": i.unique})).collect::<Vec<_>>(),
        "foreignKeys": m.fks.iter().map(|f| serde_json::json!({"name": f.name, "columns": f.cols, "refTable": f.ref_table, "refColumns": f.ref_cols, "onDelete": f.on_delete})).collect::<Vec<_>>(),
    })
}

/// Field names of a lambda `(u) => u.F` or `(u) => (u.A, u.B)`.
pub(crate) fn lambda_fields(e: &ast::Expr) -> Vec<String> {
    let ast::ExprKind::Lambda { params, body, .. } = &expr::strip_parens(e).kind else { return Vec::new() };
    let Some(p) = params.first() else { return Vec::new() };
    let body = match &**body {
        ast::LambdaBody::Expr(x) => x,
        ast::LambdaBody::Block(b) => match b.stmts.as_slice() {
            [ast::Stmt { kind: ast::StmtKind::Return(v), .. }] if v.len() == 1 => &v[0],
            _ => return Vec::new(),
        },
    };
    let one = |x: &ast::Expr| -> Option<String> {
        match &expr::strip_parens(x).kind {
            ast::ExprKind::Field { base, name } => match &expr::strip_parens(base).kind {
                ast::ExprKind::Ident(id) if id.name == p.name.name => Some(name.name.clone()),
                _ => None,
            },
            _ => None,
        }
    };
    match &expr::strip_parens(body).kind {
        ast::ExprKind::Tuple(xs) => xs.iter().filter_map(one).collect(),
        _ => one(body).into_iter().collect(),
    }
}
