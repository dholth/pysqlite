/* connection.c - the connection type
 *
 * Copyright (C) 2004 Gerhard H�ring <gh@ghaering.de>
 *
 * This file is part of pysqlite.
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include "connection.h"
#include "cursor.h"
#include "prepare_protocol.h"
#include "util.h"
#include "pythread.h"

PyObject* connection_alloc(PyTypeObject* type, int aware)
{
    PyObject *self;

    self = (PyObject*)PyObject_MALLOC(sizeof(Connection));
    if (self == NULL)
        return (PyObject *)PyErr_NoMemory();
    PyObject_INIT(self, type);

    return self;
}

int connection_init(Connection* self, PyObject* args, PyObject* kwargs)
{
    static char *kwlist[] = {"database", "timeout", "more_types", "no_implicit_begin", "check_same_thread", "prepareProtocol", NULL, NULL};
    char* database;
    int more_types = 0;
    int no_implicit_begin = 0;
    PyObject* prepare_protocol = NULL;
    PyObject* proto_args;
    int check_same_thread = 1;
    double timeout = 5.0;
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|diiiO", kwlist,
                                     &database, &timeout, &more_types, &no_implicit_begin, &check_same_thread, &prepare_protocol))
    {
        return -1; 
    }

    Py_BEGIN_ALLOW_THREADS
    rc = sqlite3_open(database, &self->db);
    Py_END_ALLOW_THREADS

    if (rc != SQLITE_OK) {
        _seterror(self->db);
        return -1;
    }

    self->inTransaction = 0;
    self->advancedTypes = more_types;
    self->timeout = timeout;
    self->no_implicit_begin = no_implicit_begin;
    self->thread_ident = PyThread_get_thread_ident();
    self->check_same_thread = check_same_thread;
    self->converters = PyDict_New();

    if (prepare_protocol == NULL) {
        proto_args = Py_BuildValue("()");
        self->prepareProtocol = (PyObject*)PyObject_CallObject((PyObject*)&SQLitePrepareProtocolType, proto_args);
        Py_DECREF(proto_args);
    } else {
        Py_INCREF(prepare_protocol);
        self->prepareProtocol = prepare_protocol;
    }

    return 0;
}

PyObject* connection_new(PyTypeObject* type, PyObject* args, PyObject* kw)
{
    Connection* self = NULL;

    self = (Connection*) (type->tp_alloc(type, 0));

    return (PyObject*)self;
}

void connection_dealloc(Connection* self)
{
    /* Clean up if user has not called .close() explicitly. */
    if (self->db) {
        Py_BEGIN_ALLOW_THREADS
        sqlite3_close(self->db);
        Py_END_ALLOW_THREADS
    }

    Py_XDECREF(self->converters);
    Py_XDECREF(self->prepareProtocol);

    self->ob_type->tp_free((PyObject*)self);
}

PyObject* connection_cursor(Connection* self, PyObject* args)
{
    Cursor* cursor = NULL;

    if (!check_thread(self)) {
        return NULL;
    }

    cursor = (Cursor*) (CursorType.tp_alloc(&CursorType, 0));
    cursor->connection = self;
    cursor->statement = NULL;
    cursor->step_rc = UNKNOWN;
    cursor->row_cast_map = PyList_New(0);

    Py_INCREF(Py_None);
    cursor->description = Py_None;

    Py_INCREF(Py_None);
    cursor->lastrowid= Py_None;

    cursor->arraysize = 1;

    Py_INCREF(Py_None);
    cursor->rowcount = Py_None;

    Py_INCREF(Py_None);
    cursor->coltypes = Py_None;
    Py_INCREF(Py_None);
    cursor->next_coltypes = Py_None;

    return (PyObject*)cursor;
}

PyObject* connection_close(Connection* self, PyObject* args)
{
    int rc;

    if (!check_thread(self)) {
        return NULL;
    }

    if (self->db) {
        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_close(self->db);
        Py_END_ALLOW_THREADS

        if (rc != SQLITE_OK) {
            _seterror(self->db);
            return NULL;
        } else {
            self->db = NULL;
        }
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* _connection_begin(Connection* self, PyObject* args)
{
    int rc;
    const char* tail;
    sqlite3_stmt* statement;

    if (!check_thread(self)) {
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = sqlite3_prepare(self->db, "BEGIN", -1, &statement, &tail);
    Py_END_ALLOW_THREADS

    if (rc != SQLITE_OK) {
        _seterror(self->db);
        return NULL;
    }

    rc = _sqlite_step_with_busyhandler(statement, self);
    if (rc != SQLITE_DONE) {
        _seterror(self->db);
        return NULL;
    }

    Py_BEGIN_ALLOW_THREADS
    rc = sqlite3_finalize(statement);
    Py_END_ALLOW_THREADS

    if (rc != SQLITE_OK) {
        _seterror(self->db);
        return NULL;
    }

    self->inTransaction = 1;

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* connection_begin(Connection* self, PyObject* args)
{
    if (!self->no_implicit_begin) {
        PyErr_SetString(ProgrammingError,
            "begin() can only be called when the connection was created with the no_implicit_begin parameter set to true.");
        return NULL;
    }

    return _connection_begin(self, args);
}

PyObject* connection_commit(Connection* self, PyObject* args)
{
    int rc;
    const char* tail;
    sqlite3_stmt* statement;

    if (!check_thread(self)) {
        return NULL;
    }

    if (self->inTransaction) {
        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_prepare(self->db, "COMMIT", -1, &statement, &tail);
        Py_END_ALLOW_THREADS
        if (rc != SQLITE_OK) {
            _seterror(self->db);
            return NULL;
        }

        rc = _sqlite_step_with_busyhandler(statement, self);

        if (rc != SQLITE_DONE) {
            _seterror(self->db);
            return NULL;
        }

        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_finalize(statement);
        Py_END_ALLOW_THREADS
        if (rc != SQLITE_OK) {
            _seterror(self->db);
            return NULL;
        }

        self->inTransaction = 0;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* connection_rollback(Connection* self, PyObject* args)
{
    int rc;
    const char* tail;
    sqlite3_stmt* statement;

    if (!check_thread(self)) {
        return NULL;
    }

    if (self->inTransaction) {
        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_prepare(self->db, "ROLLBACK", -1, &statement, &tail);
        Py_END_ALLOW_THREADS
        if (rc != SQLITE_OK) {
            _seterror(self->db);
            return NULL;
        }

        rc = _sqlite_step_with_busyhandler(statement, self);
        if (rc != SQLITE_DONE) {
            _seterror(self->db);
            return NULL;
        }

        Py_BEGIN_ALLOW_THREADS
        rc = sqlite3_finalize(statement);
        Py_END_ALLOW_THREADS
        if (rc != SQLITE_OK) {
            _seterror(self->db);
            return NULL;
        }

        self->inTransaction = 0;
    }

    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* connection_register_converter(Connection* self, PyObject* args)
{
    PyObject* name;
    PyObject* func;

    if (!check_thread(self)) {
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "OO", &name, &func)) {
        return NULL;
    }

    Py_INCREF(name);
    Py_INCREF(func);
    PyDict_SetItem(self->converters, name, func);

    Py_INCREF(Py_None);
    return Py_None;
}

void _func_callback(sqlite3_context* context, int argc, sqlite3_value** argv)
{
    PyObject* args;
    sqlite3_value* cur_value;
    PyObject* cur_py_value;
    int i;
    PyObject* py_func;
    PyObject* py_retval;

    const char* val_str;
    long long int val_int;
    long longval;
    const char* buffer;
    int buflen;

    PyObject* stringval;

    PyGILState_STATE threadstate;

    threadstate = PyGILState_Ensure();

    py_func = (PyObject*)sqlite3_user_data(context);

    args = PyTuple_New(argc);

    for (i = 0; i < argc; i++) {
        cur_value = argv[i];
        switch (sqlite3_value_type(argv[i])) {
            case SQLITE_INTEGER:
                val_int = sqlite3_value_int64(cur_value);
                cur_py_value = PyInt_FromLong((long)val_int);
                break;
            case SQLITE_FLOAT:
                cur_py_value = PyFloat_FromDouble(sqlite3_value_double(cur_value));
                break;
            case SQLITE_TEXT:
                val_str = sqlite3_value_text(cur_value);
                cur_py_value = PyUnicode_DecodeUTF8(val_str, strlen(val_str), NULL);
                /* TODO: error handling, check cur_py_value for NULL */
                break;
            case SQLITE_NULL:
            default:
                Py_INCREF(Py_None);
                cur_py_value = Py_None;
        }
        PyTuple_SetItem(args, i, cur_py_value);

    }

    py_retval = PyObject_CallObject(py_func, args);
    Py_DECREF(args);

    if (PyErr_Occurred()) {
        /* Errors in callbacks are ignored, and we return NULL */
        PyErr_Clear();
        sqlite3_result_null(context);
    } else if (py_retval == Py_None) {
        sqlite3_result_null(context);
    } else if (PyInt_Check(py_retval)) {
        longval = PyInt_AsLong(py_retval);
        /* TODO: investigate what to do with range overflows - long vs. long long */
        sqlite3_result_int64(context, (long long int)longval);
    } else if (PyFloat_Check(py_retval)) {
        sqlite3_result_double(context, PyFloat_AsDouble(py_retval));
    } else if (PyBuffer_Check(py_retval)) {
        if (PyObject_AsCharBuffer(py_retval, &buffer, &buflen) != 0) {
            /* TODO: decide what error to raise */
            PyErr_SetString(PyExc_ValueError, "could not convert BLOB to buffer");
        }
        sqlite3_result_blob(context, buffer, buflen, SQLITE_TRANSIENT);
    } else if (PyString_Check(py_retval)) {
        sqlite3_result_text(context, PyString_AsString(py_retval), -1, SQLITE_TRANSIENT);
    } else if (PyUnicode_Check(py_retval)) {
        stringval = PyUnicode_AsUTF8String(py_retval);
        sqlite3_result_text(context, PyString_AsString(stringval), -1, SQLITE_TRANSIENT);
        Py_DECREF(stringval);
    } else {
        /* TODO: raise error */
    }

    PyGILState_Release(threadstate);
}

static void _step_callback(sqlite3_context *context, int argc, sqlite3_value** params)
{
    int i;
    PyObject* args;
    PyObject* function_result;
    PyObject* aggregate_class;
    PyObject** aggregate_instance;
    PyObject* stepmethod;
    char* strparam;

    PyGILState_STATE threadstate;

    threadstate = PyGILState_Ensure();

    aggregate_class = (PyObject*)sqlite3_user_data(context);

    aggregate_instance = (PyObject**)sqlite3_aggregate_context(context, sizeof(PyObject*));

    if (*aggregate_instance == 0) {
        args = PyTuple_New(0);
        *aggregate_instance = PyObject_CallObject(aggregate_class, args);
        Py_DECREF(args);

        if (PyErr_Occurred())
        {
            //PRINT_OR_CLEAR_ERROR
            PyGILState_Release(threadstate);
            return;
        }
    }

    stepmethod = PyObject_GetAttrString(*aggregate_instance, "step");
    if (!stepmethod)
    {
        /* PRINT_OR_CLEAR_ERROR */
        PyGILState_Release(threadstate);
        return;
    }

    args = PyTuple_New(argc);
    for (i = 0; i < argc; i++) {
        /* TODO: switch on different types, call appropriate sqlite3_value_...
         * functions, or rather, factor that stuff out in a helper function */
        strparam = (char*)sqlite3_value_text(params[i]);
        if (!strparam)
        {
            Py_INCREF(Py_None);
            PyTuple_SetItem(args, i, Py_None);
        } else {
            PyTuple_SetItem(args, i, PyString_FromString(strparam));
        }
    }

    if (PyErr_Occurred())
    {
        //PRINT_OR_CLEAR_ERROR
    }

    function_result = PyObject_CallObject(stepmethod, args);
    Py_DECREF(args);
    Py_DECREF(stepmethod);

    if (function_result == NULL)
    {
        //PRINT_OR_CLEAR_ERROR
        /* Don't use sqlite_set_result_error here. Else an assertion in
         * the SQLite code will trigger and create a core dump.
         *
         * This was true with SQLite 2.x. Not checked with 3.x, yet.
         */
    }
    else
    {
        Py_DECREF(function_result);
    }

    PyGILState_Release(threadstate);
}

void _final_callback(sqlite3_context* context)
{
    PyObject* args;
    PyObject* function_result;
    PyObject* s;
    PyObject** aggregate_instance;
    PyObject* aggregate_class;
    PyObject* finalizemethod;

    PyGILState_STATE threadstate;

    threadstate = PyGILState_Ensure();

    aggregate_class = (PyObject*)sqlite3_user_data(context);

    aggregate_instance = (PyObject**)sqlite3_aggregate_context(context, sizeof(PyObject*));

    finalizemethod = PyObject_GetAttrString(*aggregate_instance, "finalize");

    if (!finalizemethod) {
        PyErr_SetString(PyExc_ValueError, "finalize method missing");
        goto error;
    }

    args = PyTuple_New(0);
    function_result = PyObject_CallObject(finalizemethod, args);
    Py_DECREF(args);
    Py_DECREF(finalizemethod);

    if (PyErr_Occurred())
    {
        //PRINT_OR_CLEAR_ERROR
        sqlite3_result_error(context, NULL, -1);
    }
    else if (function_result == Py_None)
    {
        Py_DECREF(function_result);
        sqlite3_result_null(context);
    }
    else
    {
        s = PyObject_Str(function_result);
        Py_DECREF(function_result);
        sqlite3_result_text(context, PyString_AsString(s), -1, SQLITE_TRANSIENT);
        Py_DECREF(s);
    }

error:
    Py_XDECREF(*aggregate_instance);

    PyGILState_Release(threadstate);
}


PyObject* connection_create_function(Connection* self, PyObject* args, PyObject* kwargs)
{
    static char *kwlist[] = {"name", "narg", "func", NULL, NULL};

    PyObject* func;
    char* name;
    int narg;
    int rc;


    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "siO", kwlist,
                                     &name, &narg, &func))
    {
        return NULL;
    }

    Py_INCREF(func);

    rc = sqlite3_create_function(self->db, name, narg, SQLITE_UTF8, (void*)func, _func_callback, NULL, NULL);
;
    Py_INCREF(Py_None);
    return Py_None;
}

PyObject* connection_create_aggregate(Connection* self, PyObject* args, PyObject* kwargs)
{
    PyObject* aggregate_class;

    int n_arg;
    char* name;
    static char *kwlist[] = { "name", "n_arg", "aggregate_class", NULL };
    int rc;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "siO:create_aggregate",
                                      kwlist, &name, &n_arg, &aggregate_class)) {
        return NULL;
    }

    Py_INCREF(aggregate_class);

    rc = sqlite3_create_function(self->db, name, n_arg, SQLITE_UTF8, (void*)aggregate_class, 0, &_step_callback, &_final_callback);
    if (rc != SQLITE_OK) {
        _seterror(self->db);
        return NULL;
    } else {
        Py_INCREF(Py_None);
        return Py_None;
    }
}

int check_thread(Connection* self)
{
    if (self->check_same_thread) {
        if (PyThread_get_thread_ident() != self->thread_ident) {
            PyErr_Format(ProgrammingError,
                        "SQLite objects created in a thread can only be used in that same thread."
                        "The object was created in thread id %d and this is thread id %d",
                        PyThread_get_thread_ident(), self->thread_ident);
            return 0;
        }

    }

    return 1;
}

static char connection_doc[] =
PyDoc_STR("<missing docstring>");

static PyMethodDef connection_methods[] = {
    {"cursor", (PyCFunction)connection_cursor, METH_NOARGS,
        PyDoc_STR("Return a cursor for the connection.")},
    {"close", (PyCFunction)connection_close, METH_NOARGS,
        PyDoc_STR("Closes the connection.")},
    {"begin", (PyCFunction)connection_begin, METH_NOARGS,
        PyDoc_STR("Starts a new transaction.")},
    {"commit", (PyCFunction)connection_commit, METH_NOARGS,
        PyDoc_STR("Commit the current transaction.")},
    {"rollback", (PyCFunction)connection_rollback, METH_NOARGS,
        PyDoc_STR("Roll back the current transaction.")},
    {"register_converter", (PyCFunction)connection_register_converter, METH_VARARGS,
        PyDoc_STR("Registers a new type converter.")},
    {"create_function", (PyCFunction)connection_create_function, METH_VARARGS|METH_KEYWORDS,
        PyDoc_STR("Creates a new function.")},
    {"create_aggregate", (PyCFunction)connection_create_aggregate, METH_VARARGS|METH_KEYWORDS,
        PyDoc_STR("Creates a new aggregate.")},
    {NULL, NULL}
};

PyTypeObject ConnectionType = {
        PyObject_HEAD_INIT(NULL)
        0,                                              /* ob_size */
        "pysqlite2.dbapi2.Connection",                  /* tp_name */
        sizeof(Connection),                             /* tp_basicsize */
        0,                                              /* tp_itemsize */
        (destructor)connection_dealloc,                 /* tp_dealloc */
        0,                                              /* tp_print */
        0,                                              /* tp_getattr */
        0,                                              /* tp_setattr */
        0,                                              /* tp_compare */
        0,                                              /* tp_repr */
        0,                                              /* tp_as_number */
        0,                                              /* tp_as_sequence */
        0,                                              /* tp_as_mapping */
        0,                                              /* tp_hash */
        0,                                              /* tp_call */
        0,                                              /* tp_str */
        0,                                              /* tp_getattro */
        0,                                              /* tp_setattro */
        0,                                              /* tp_as_buffer */
        Py_TPFLAGS_DEFAULT,                             /* tp_flags */
        connection_doc,                                 /* tp_doc */
        0,                                              /* tp_traverse */
        0,                                              /* tp_clear */
        0,                                              /* tp_richcompare */
        0,                                              /* tp_weaklistoffset */
        0,                                              /* tp_iter */
        0,                                              /* tp_iternext */
        connection_methods,                             /* tp_methods */
        0,                                              /* tp_members */
        0,                                              /* tp_getset */
        0,                                              /* tp_base */
        0,                                              /* tp_dict */
        0,                                              /* tp_descr_get */
        0,                                              /* tp_descr_set */
        0,                                              /* tp_dictoffset */
        (initproc)connection_init,                      /* tp_init */
        0,                                              /* tp_alloc */
        0,                                              /* tp_new */
        0                                               /* tp_free */
};
 
