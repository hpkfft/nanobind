#include "internals.h"

NAMESPACE_BEGIN(NB_NAMESPACE)
NAMESPACE_BEGIN(detail)

template <typename T>
NB_INLINE T *get_data(void *o) {
    return (T*) ((char *) o + Py_TYPE(o)->tp_basicsize);
}

static int inst_init(PyObject *self, PyObject *, PyObject *) {
    PyErr_Format(PyExc_TypeError, "%s: no constructor defined!",
                 Py_TYPE(self)->tp_name);
    return -1;
}

static void *inst_data(nb_inst *self) {
    void *ptr = (void *) ((uintptr_t) self + self->offset);
    return self->internal ? ptr : *(void **) ptr;
}

// Allocate a new instance with internal or external storage
PyObject *inst_new_impl(PyTypeObject *tp, void *value) {
    const type_data *t = &((nb_type *) tp)->t;
    const bool has_gc = tp->tp_flags & Py_TPFLAGS_HAVE_GC;

    size_t gc_head = has_gc ? sizeof(uintptr_t) * 2 : 0,
           basicsize_plus_gc = (size_t) tp->tp_basicsize + gc_head,
           align = (size_t) t->align;

    size_t size = basicsize_plus_gc;
    if (value) {
        // External: only allocate space for a pointer
        size += sizeof(void *);
    } else {
        // Internal: allocate space for the object and padding for alignment
        size += t->size;
        if (align > sizeof(void *))
            size += align - sizeof(void *);
    }

    uint8_t *alloc = (uint8_t *) PyObject_Malloc(size);
    if (!alloc)
        return PyErr_NoMemory();

    // Clear only the initial part of the object (GC head, nb_inst contents)
    // memset(alloc, 0, basicsize_plus_gc);
    memset(alloc, 0, size);

    // Initialize the Python object and register it with the GC if needed
    PyObject *o = (PyObject *) (alloc + gc_head);
    PyObject_Init(o, tp);
    if (has_gc)
        PyObject_GC_Track(o);

    nb_inst *self = (nb_inst *) o;
    uint8_t *data = alloc + basicsize_plus_gc;

    if (value) {
        *(void **) data = value;
    } else {
        data = (uint8_t *) ((((uintptr_t) data) + align - 1) / align * align);
        self->internal = true;
        value = data;
    }

    self->offset = (uintptr_t) data - (uintptr_t) o;

    // Update hash table that maps from C++ to Python instance
    auto [it, success] = internals_get().inst_c2p.try_emplace(
        std::pair<void *, std::type_index>(value, *t->type),
        self);

    if (!success)
        fail("nanobind::detail::inst_new(): duplicate object!");

    return (PyObject *) self;
}

// Allocate a new instance with co-located storage
PyObject *inst_new(PyTypeObject *type, PyObject *, PyObject *) {
    return (PyObject *) inst_new_impl(type, nullptr);
}

static void inst_dealloc(PyObject *self) {
    nb_type *type = (nb_type *) Py_TYPE(self);
    nb_inst *inst = (nb_inst *) self;
    void *p = inst_data(inst);

    if (inst->destruct) {
        if (type->t.flags & (int16_t) type_flags::is_destructible) {
            if (type->t.flags & (int16_t) type_flags::has_destruct)
                type->t.destruct(p);
        } else {
            fail("nanobind::detail::inst_dealloc(\"%s\"): attempted to call "
                 "the destructor of a non-destructible type!",
                 type->ht.ht_type.tp_name);
        }
    }

    if (inst->free) {
        if (type->t.align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
            operator delete(p);
        else
            operator delete(p, std::align_val_t(type->t.align));
    }

    internals &internals = internals_get();
    if (inst->clear_keep_alive) {
        auto it = internals.keep_alive.find(self);
        if (it == internals.keep_alive.end())
            fail("nanobind::detail::inst_dealloc(\"%s\"): inconsistent keep_alive information",
                 type->ht.ht_type.tp_name);

        tsl_ptr_set ref_set = std::move(it.value());
        internals.keep_alive.erase(it);

        for (void *v: ref_set)
            Py_DECREF((PyObject *) v);
    }

    // Update hash table that maps from C++ to Python instance
    auto it = internals.inst_c2p.find(
        std::pair<void *, std::type_index>(p, *type->t.type));
    if (it == internals.inst_c2p.end())
        fail("nanobind::detail::inst_dealloc(\"%s\"): attempted to delete "
             "an unknown instance!", type->ht.ht_type.tp_name);
    internals.inst_c2p.erase(it);

    type->ht.ht_type.tp_free(self);
    Py_DECREF(type);
}

void nb_type_dealloc(PyObject *o) {
    nb_type *nbt = (nb_type *) o;

    if ((nbt->t.flags & (uint16_t) type_flags::is_python_type) == 0) {
        // Try to find type in data structure
        internals &internals = internals_get();
        auto it = internals.type_c2p.find(std::type_index(*nbt->t.type));
        if (it == internals.type_c2p.end())
            fail("nanobind::detail::nb_type_dealloc(\"%s\"): could not find type!",
                 ((PyTypeObject *) o)->tp_name);
        internals.type_c2p.erase(it);
    }

    PyType_Type.tp_dealloc(o);
}

/// Called when a C++ type is extended from within Python
int nb_type_init(PyObject *self, PyObject *args, PyObject *kwds) {
    if (PyTuple_GET_SIZE(args) != 3)
        return -1;

    int rv = PyType_Type.tp_init(self, args, kwds);
    if (rv)
        return rv;

    nb_type *type = (nb_type *) self;
    nb_type *parent = (nb_type *) type->ht.ht_type.tp_base;

    type->t = parent->t;
    type->t.flags |= (uint32_t) type_flags::is_python_type;

    return 0;
}

/// Called when a C++ type is bound via nb::class_<>
PyObject *nb_type_new(const type_data *t) noexcept {
    const bool has_scope   = t->flags & (uint16_t) type_flags::has_scope,
               has_doc     = t->flags & (uint16_t) type_flags::has_doc,
               has_base    = t->flags & (uint16_t) type_flags::has_base,
               has_base_py = t->flags & (uint16_t) type_flags::has_base_py;

    str name(t->name), qualname = name, fullname = name;

    if (has_scope && !PyModule_Check(t->scope)) {
        object scope_qualname = borrow(getattr(t->scope, "__qualname__", nullptr));
        if (scope_qualname.is_valid())
            qualname = steal<str>(PyUnicode_FromFormat(
                "%U.%U", scope_qualname.ptr(), name.ptr()));
    }

    object scope_name;
    if (has_scope) {
        scope_name = getattr(t->scope, "__module__", handle());
        if (!scope_name.is_valid())
            scope_name = getattr(t->scope, "__name__", handle());

        if (scope_name.is_valid())
            fullname = steal<str>(
                PyUnicode_FromFormat("%U.%U", scope_name.ptr(), name.ptr()));
    }

    /* Danger zone: from now (and until PyType_Ready), make sure to
       issue no Python C API calls which could potentially invoke the
       garbage collector (the GC will call type_traverse(), which will in
       turn find the newly constructed type in an invalid state) */

    internals &internals = internals_get();
    nb_type *nbt =
        (nb_type *) internals.nb_type->tp_alloc(internals.nb_type, 0);
    PyTypeObject *type = &nbt->ht.ht_type;

    memcpy(&nbt->t, t, sizeof(type_data));

    nbt->ht.ht_name = name.release().ptr();
    nbt->ht.ht_qualname = qualname.release().ptr();

    PyTypeObject *base = nullptr;
    if (has_base_py) {
        if (has_base)
            fail("nanobind::detail::nb_type_new(\"%s\"): multiple base types "
                 "specified!", t->name);
        base = t->base_py;
    } else if (has_base) {
        auto it = internals.type_c2p.find(std::type_index(*t->base));
        if (it == internals.type_c2p.end())
            fail("nanobind::detail::nb_type_new(\"%s\"): base type "
                 "\"%s\" not found!", t->name, type_name(t->base));
        base = it->second->type_py;
    }

    type->tp_name = t->name;
    type->tp_basicsize = (Py_ssize_t) sizeof(nb_inst);

    type->tp_dealloc = inst_dealloc;
    type->tp_as_async = &nbt->ht.as_async;
    type->tp_as_number = &nbt->ht.as_number;
    type->tp_as_sequence = &nbt->ht.as_sequence;
    type->tp_as_mapping = &nbt->ht.as_mapping;
    type->tp_flags |=
        Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HEAPTYPE | Py_TPFLAGS_BASETYPE;
    if (has_doc)
        type->tp_doc = t->doc;
    type->tp_base = base;
    type->tp_init = inst_init;
    type->tp_new = inst_new;

    if (PyType_Ready(type) < 0)
        fail("nanobind::detail::nb_type_new(\"%s\"): PyType_Ready() failed!", t->name);

    Py_INCREF(type->tp_base);

    if (scope_name.is_valid())
        setattr((PyObject *) type, "__module__", scope_name);

    if (has_scope)
        setattr(t->scope, t->name, (PyObject *) type);

    nbt->t.type_py = type;

    // Update hash table that maps from std::type_info to Python type
    auto [it, success] =
        internals.type_c2p.try_emplace(std::type_index(*t->type), &nbt->t);
    if (!success)
        fail("nanobind::detail::nb_type_new(\"%s\"): type was already registered!",
             t->name);

    return (PyObject *) type;
}

bool nb_type_get(const std::type_info *cpp_type, PyObject *o, uint8_t flags,
                 void **out) noexcept {
    if (o == nullptr || o == Py_None) {
        *out = nullptr;
        return o != nullptr;
    }

    internals &internals = internals_get();
    PyTypeObject *type = Py_TYPE(o);

    // Reject if this object doesn't have the nanobind metaclass
    if (Py_TYPE(type) != internals.nb_type)
        return false;

    nb_type *nbt = (nb_type *) type;

    if (nbt->t.type == cpp_type || *nbt->t.type == *cpp_type ||
        PyType_IsSubtype(type, nbt->t.type_py)) {
        nb_inst *inst = (nb_inst *) o;
        if (!inst->ready && (flags & (uint8_t) cast_flags::construct) == 0) {
            fprintf(stderr,
                   "nb_type_get(): attempted to access an uninitialized instance "
                   "of type '%s'!\n", nbt->t.name);
            return false;
        }
        *out = inst_data(inst);
        return true;
    } else {
        return false;
    }
}

void inst_keep_alive(PyObject *nurse, PyObject *patient) {
    if (!patient)
        return;

    internals &internals = internals_get();
    if (!nurse || Py_TYPE(Py_TYPE(nurse)) != internals.nb_type)
        raise("inst_keep_alive(): expected a nb_type 'nurse' argument");

    tsl_ptr_set &keep_alive = internals.keep_alive[nurse];

    auto [it, success] = keep_alive.insert(patient);
    if (success) {
        Py_INCREF(patient);
        ((nb_inst *) nurse)->clear_keep_alive = true;
    }
}

PyObject *nb_type_put(const std::type_info *cpp_type, void *value,
                      rv_policy rvp, PyObject *parent) noexcept {
    // Convert nullptr -> None
    if (!value) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    // Check if the instance is already registered with nanobind
    internals &internals = internals_get();
    auto it = internals.inst_c2p.find(
        std::pair<void *, std::type_index>(value, *cpp_type));
    if (it != internals.inst_c2p.end()) {
        PyObject *result = (PyObject *) it->second;
        Py_INCREF(result);
        return result;
    } else if (rvp == rv_policy::none) {
        return nullptr;
    }

    // Look up the corresponding type
    auto it2 = internals.type_c2p.find(std::type_index(*cpp_type));
    if (it2 == internals.type_c2p.end())
        return nullptr;

    type_data *t = it2->second;

    bool store_in_obj = rvp == rv_policy::copy || rvp == rv_policy::move;

    nb_inst *inst =
        (nb_inst *) inst_new_impl(t->type_py, store_in_obj ? nullptr : value);
    if (!inst)
        return nullptr;

    inst->free = inst->destruct && !store_in_obj;

    void *new_value = inst_data(inst);
    if (rvp == rv_policy::move) {
        if (t->flags & (uint16_t) type_flags::is_move_constructible) {
            if (t->flags & (uint16_t) type_flags::has_move) {
                try {
                    t->move(new_value, value);
                } catch (...) {
                    Py_DECREF(inst);
                    return nullptr;
                }
            } else {
                memcpy(new_value, value, t->size);
            }
        } else {
            fail("nanobind::detail::nb_type_put(\"%s\"): attempted to move "
                 "an instance that is not move-constructible!", t->name);
        }
    }

    if (rvp == rv_policy::copy) {
        if (t->flags & (uint16_t) type_flags::is_copy_constructible) {
            if (t->flags & (uint16_t) type_flags::has_copy) {
                try {
                    t->copy(new_value, value);
                } catch (...) {
                    Py_DECREF(inst);
                    return nullptr;
                }
            } else {
                memcpy(new_value, value, t->size);
            }
        } else {
            fail("nanobind::detail::nb_type_put(\"%s\"): attempted to copy "
                 "an instance that is not copy-constructible!", t->name);
        }
    }

    inst->ready = true;
    inst->destruct =
        rvp != rv_policy::reference && rvp != rv_policy::reference_internal;

    if (rvp == rv_policy::reference_internal)
        inst_keep_alive((PyObject *) inst, parent);

    return (PyObject *) inst;
}

NAMESPACE_END(detail)
NAMESPACE_END(NB_NAMESPACE)