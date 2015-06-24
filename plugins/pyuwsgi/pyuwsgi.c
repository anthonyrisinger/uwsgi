#include "../python/uwsgi_python.h"

// FIXME: upstream/python: needs PyAPI_FUNC(void)
extern void Py_GetArgcArgv(int *, char ***);
// used to determine how many args python processed
extern int _PyOS_optind;

extern struct uwsgi_server uwsgi;
extern struct uwsgi_python up;

extern char **environ;

// set by python during init_uwsgi()
static int py_argc = -1;
static char **py_argv = NULL;

// maybe copied to uwsgi during pyuwsgi_load()
static int binary_argc = -1;
static int new_argc = -1;
static char **new_argv = NULL;
static char *new_argv_buf = NULL;


PyObject *
pyuwsgi_setup(PyObject *self, PyObject *args, PyObject *kwds)
{
    if (new_argv) {
        PyErr_SetString(PyExc_RuntimeError, "uWSGI already setup");
        return NULL;
    }

    if (uwsgi.mywid) {
        PyErr_SetString(PyExc_RuntimeError, "uWSGI must be setup by master");
        return NULL;
    }

    PyObject *interp_argv = NULL;
    PyObject *script_argv = NULL;
    PyArg_ParseTuple(args, "OO:setup", &interp_argv, &script_argv);
    if (!PySequence_Check(interp_argv) || !PySequence_Check(script_argv)) {
        PyErr_SetString(PyExc_TypeError, "expected a sequence");
        return NULL;
    }

    PyObject *new_argv_tup = PyObject_GetAttrString(self, "new_argv");
    PyObject *new_interp_argv = PyTuple_GetItem(new_argv_tup, 0);
    PyObject *new_script_argv = PyTuple_GetItem(new_argv_tup, 1);
    PyList_SetSlice(new_interp_argv, 0, PyList_Size(new_interp_argv), NULL);
    PyList_SetSlice(new_interp_argv, 0, PyList_Size(new_script_argv), NULL);
    Py_DECREF(new_argv_tup);

    int i;
    int new_argv_buf_len = 1;
    int interp_argv_len = PySequence_Size(interp_argv);
    int script_argv_len = PySequence_Size(script_argv);
    for (i=0; i < interp_argv_len; i++) {
        PyObject *arg = PySequence_Fast_GET_ITEM(interp_argv, i);
        if (!PyString_Check(arg)) {
            // we have no refs
            PyErr_SetString(PyExc_TypeError, "expected a string");
            return NULL;
        }
        new_argv_buf_len += PyString_Size(arg) + 1;
    }
    for (i=0; i < script_argv_len; i++) {
        PyObject *arg = PySequence_Fast_GET_ITEM(script_argv, i);
        if (!PyString_Check(arg)) {
            // we have no refs
            PyErr_SetString(PyExc_TypeError, "expected a string");
            return NULL;
        }
        new_argv_buf_len += PyString_Size(arg) + 1;
    }

    // Set globals!
    binary_argc = interp_argv_len;
    new_argc = interp_argv_len + script_argv_len;
    new_argv = uwsgi_calloc(sizeof(char *) * (new_argc + 1));
    new_argv_buf = uwsgi_calloc(new_argv_buf_len);

    char *new_argv_ptr = new_argv_buf;
    for(i=0; i < interp_argv_len; i++) {
        PyObject *arg = PySequence_Fast_GET_ITEM(interp_argv, i);
        PyList_Append(new_interp_argv, arg);
        char *arg_str = PyString_AsString(arg);
        new_argv[i] = new_argv_ptr;
        strcpy(new_argv_ptr, arg_str);
        new_argv_ptr += strlen(arg_str) + 1;
    }
    for(i=i; i < new_argc; i++) {
        PyObject *arg = PySequence_Fast_GET_ITEM(script_argv, i - interp_argv_len);
        PyList_Append(new_script_argv, arg);
        char *arg_str = PyString_AsString(arg);
        new_argv[i] = new_argv_ptr;
        strcpy(new_argv_ptr, arg_str);
        new_argv_ptr += strlen(arg_str) + 1;
    }

    if (PyErr_Occurred()) {
        free(new_argv);
        free(new_argv_buf);
        binary_argc = -1;
        new_argc = -1;
        new_argv = NULL;
        new_argv_buf = NULL;
        return NULL;
    }

    PyThreadState *_tstate = PyThreadState_Get();
    uwsgi_setup(py_argc, py_argv, environ);
    PyThreadState_Swap(_tstate);

    Py_INCREF(self);
    return self;
}


PyObject *
pyuwsgi_run(PyObject *self, PyObject *args, PyObject *kwds)
{
    int rc = uwsgi_run();
    // never(?) here
    return Py_BuildValue("i", rc);
}


PyMethodDef methods[] = {
    {"setup", (PyCFunction) pyuwsgi_setup, METH_VARARGS | METH_KEYWORDS,
     "setup([sys.executable, '-muwsgi'], ['--master'])\n"},
    {"run", (PyCFunction) pyuwsgi_run, METH_VARARGS | METH_KEYWORDS,
     "run()\n"},
    {NULL, NULL, 0, NULL}
};


static void
pyuwsgi_set_py_argv(PyObject *self)
{
    // ask python for the original argc/argv saved in Py_Main()
    Py_GetArgcArgv(&py_argc, &py_argv);

    // export to _uwsgi.argv
    PyObject *m_argv = PyObject_GetAttrString(self, "argv");
    PyObject *m_argv_interp = PyTuple_GetItem(m_argv, 0);
    PyObject *m_argv_script = PyTuple_GetItem(m_argv, 1);
    Py_DECREF(m_argv);

    int i = 0;
    for(i=0; i < py_argc; i++) {
        // TODO: upstream/python: py_argv could be mangled; reset
        // related: http://bugs.python.org/issue8202
        if (py_argv[i + 1]) {
            py_argv[i + 1] = py_argv[i] + strlen(py_argv[i]) + 1;
        }

        PyObject *arg = PyString_FromString(py_argv[i]);
        if (i <= _PyOS_optind) {
            // interpreter argument
            PyList_Append(m_argv_interp, arg);
        }
        else {
            // script argument
            PyList_Append(m_argv_script, arg);
        }
        Py_DECREF(arg);
    }
}


PyMODINIT_FUNC
init_uwsgi()
{
    PyObject *self;

    // sys.modules
    self = PyImport_GetModuleDict();
    if (!self) {
        return NULL;
    }

    // self = sys.modules.setdefault('_uwsgi', ModuleType('_uwsgi'))
    self = PyDict_GetItemString(self, "_uwsgi");
    if (!self) {
        self = Py_InitModule("_uwsgi", NULL);
        // self.argv, self.new_argv = ([], []), ([], [])
        char **keys = (char *[]){"argv", "new_argv", NULL};
        while (keys[0]) {
            PyObject *tuple = PyTuple_New(2);
            PyTuple_SetItem(tuple, 0, PyList_New(0));
            PyTuple_SetItem(tuple, 1, PyList_New(0));
            PyObject_SetAttrString(self, keys[0], tuple);
            Py_DECREF(tuple);
            keys++;
        }
	}

    // fixup and export the original argv passed to Py_Main
    if (py_argc < 0) {
        pyuwsgi_set_py_argv(self);
    }

    int i;
    for (i=0; methods[i].ml_name != NULL; i++) {
        if (PyObject_HasAttrString(self, methods[i].ml_name)) {
            continue;
        }

        // related: Python/modsupport.c:Py_InitModule4
        // self.method = MethodType(fun, self, None)
        PyObject *name = PyString_FromString(methods[i].ml_name);
        PyObject *fun = PyCFunction_NewEx(&methods[i], self, name);
        PyObject_SetAttr(self, name, fun);
        Py_DECREF(name);
        Py_DECREF(fun);
    }
}


void pyuwsgi_load()
{
    if (binary_argc > -1) {
        uwsgi.binary_argc = binary_argc;
        uwsgi.new_argc = new_argc;
        uwsgi.new_argv = new_argv;
    }
}


struct uwsgi_plugin pyuwsgi_plugin = {
        .name = "pyuwsgi",
        .on_load = pyuwsgi_load,
};
