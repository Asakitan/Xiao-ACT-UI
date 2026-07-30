#pragma once

// CPython's Windows headers select the debug ABI whenever MSVC defines
// _DEBUG. This host deliberately embeds a release Python DLL, so hide _DEBUG
// only while Python.h is parsed; the enclosing translation unit still uses
// the Debug CRT and its normal debug configuration afterward.
#if defined(_DEBUG)
#  define SAO_PYHOST_RESTORE_MSVC_DEBUG
#  undef _DEBUG
#endif

#include <Python.h>

#if defined(SAO_PYHOST_RESTORE_MSVC_DEBUG)
#  define _DEBUG 1
#  undef SAO_PYHOST_RESTORE_MSVC_DEBUG
#endif

#if defined(SAO_PYHOST_DYNAMIC_PY_DATA)
namespace sao::plugins::python_host::detail {

struct python_data_exports {
    PyTypeObject* py_bool_type = nullptr;
    PyTypeObject* py_capsule_type = nullptr;
    PyTypeObject* py_float_type = nullptr;
    PyTypeObject* py_frozen_set_type = nullptr;
    PyTypeObject* py_function_type = nullptr;
    PyTypeObject* py_module_type = nullptr;
    PyTypeObject* py_set_type = nullptr;
    PyObject** py_exc_import_error = nullptr;
    PyObject** py_exc_not_implemented_error = nullptr;
    PyObject** py_exc_overflow_error = nullptr;
    PyObject** py_exc_permission_error = nullptr;
    PyObject** py_exc_runtime_error = nullptr;
    PyObject** py_exc_type_error = nullptr;
    PyObject** py_exc_value_error = nullptr;
    PyLongObject* py_false_struct = nullptr;
    PyObject* py_none_struct = nullptr;
    PyLongObject* py_true_struct = nullptr;
};

python_data_exports& dynamic_python_data() noexcept;

} // namespace sao::plugins::python_host::detail

#  define PyBool_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_bool_type)
#  define PyCapsule_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_capsule_type)
#  define PyFloat_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_float_type)
#  define PyFrozenSet_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_frozen_set_type)
#  define PyFunction_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_function_type)
#  define PyModule_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_module_type)
#  define PySet_Type \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_set_type)
#  define PyExc_ImportError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_import_error)
#  define PyExc_NotImplementedError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_not_implemented_error)
#  define PyExc_OverflowError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_overflow_error)
#  define PyExc_PermissionError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_permission_error)
#  define PyExc_RuntimeError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_runtime_error)
#  define PyExc_TypeError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_type_error)
#  define PyExc_ValueError \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_exc_value_error)
#  define _Py_FalseStruct \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_false_struct)
#  define _Py_NoneStruct \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_none_struct)
#  define _Py_TrueStruct \
	(*::sao::plugins::python_host::detail::dynamic_python_data().py_true_struct)
#endif
