#include <torch/csrc/autograd/generated/python_functions.h>

// ${generated_comment}

#include <Python.h>
#include <ATen/ATen.h>

#include <c10/core/SymNodeImpl.h>
#include "torch/csrc/autograd/generated/Functions.h"
#include "torch/csrc/autograd/python_cpp_function.h"
#include <torch/csrc/autograd/python_variable.h>
#include <torch/csrc/autograd/saved_variable.h>
#include <torch/csrc/utils/pybind.h>
#include <pybind11/pybind11.h>
#include <torch/csrc/utils/pybind.h>

// NOTE: See [Sharded File] comment in VariableType

namespace torch { namespace autograd { namespace generated {

//addClass 会调用到 registerCppFunction 注册 type（ function_properties），
//我们这里参数 function_properties 就是 accumulate_grad_properties，type 就是 AccumulateGradClass。
template<typename C>
static void addClass(PyObject* module, PyTypeObject& type, const char* name,
  PyGetSetDef* function_properties=NULL, PyMethodDef* function_methods=NULL)
{
  //// 这里设置了 accumulate_grad_properties
  _initFunctionPyTypeObject(type, name, function_properties, function_methods);
  Py_INCREF(&type);
  PyModule_AddObject(module, name, (PyObject*)&type);

  // // 注册了 type
  registerCppFunction(typeid(C), &type);
}

${py_function_props_and_getters}

void initialize_autogenerated_functions${shard_id}(PyObject* module) {
  ${py_function_initializers}
}

}}} // namespace torch::autograd::generated
