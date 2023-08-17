#include <torch/csrc/autograd/python_engine.h>

#include <ATen/LegacyBatchedTensorImpl.h>
#include <ATen/LegacyVmapMode.h>
#include <c10/util/irange.h>
#include <pybind11/pybind11.h>
#include <torch/csrc/DynamicTypes.h>
#include <torch/csrc/THP.h>
#include <torch/csrc/autograd/edge.h>
#include <torch/csrc/autograd/engine.h>
#include <torch/csrc/autograd/function.h>
#include <torch/csrc/autograd/functions/basic_ops.h>
#include <torch/csrc/autograd/python_anomaly_mode.h>
#include <torch/csrc/autograd/python_function.h>
#include <torch/csrc/autograd/python_saved_variable_hooks.h>
#include <torch/csrc/utils/pybind.h>
#include <torch/csrc/utils/pycfunction_helpers.h>

#ifndef _WIN32
#include <pthread.h>
#endif

#include <memory> // for unique_ptr
#include <unordered_set>
#include <utility>

using namespace torch::autograd;

struct THPEngine {
  PyObject_HEAD
};

static bool _reinitialize_engine = false;

namespace torch {
namespace autograd {
namespace python {

PythonEngine::PythonEngine() = default;
// get_python_engine这里定义了一个静态变量。整个PyTorch程序全局只维护一个Engine实例，也就是PythonEngine实例。
Engine& PythonEngine::get_python_engine() {
  static PythonEngine engine;
  // This is "probably" thread-safe because the flag is set in a fork handler
  // before any threads are created, and this function is only called with the
  // GIL held. However, using fork + threads is playing with fire so this is
  // more of a "best effort" thing. For example, if the fork occurs while the
  // backwards threads hold a lock, we'll probably deadlock in the engine
  // destructor.
  if (_reinitialize_engine) {
    engine.release_workers();
    engine.~PythonEngine();
    new (&engine) torch::autograd::python::PythonEngine();
    _reinitialize_engine = false;
  }
  return engine;
}

PythonEngine::~PythonEngine() {
  Engine::stop();
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 9
#define IS_PYTHON_3_9_PLUS
#endif

void PythonEngine::thread_init(
    int device,
    const std::shared_ptr<ReadyQueue>& ready_queue,
    bool should_increment) {
  // Increment thread usage count before acquiring the GIL
  if (should_increment) {
    increment_non_reentrant_thread_count();
  }
  // Create a PyThreadState, but release the GIL. This lets
  // pybind11::gil_scoped_acquire calls inside thread_main acquire the GIL
  // without having to create a new PyThreadState each time.
#if defined(IS_PYTHON_3_9_PLUS)
  auto gil = std::make_unique<pybind11::gil_scoped_acquire>();
#else
  pybind11::gil_scoped_acquire gil;
#endif
  pybind11::gil_scoped_release no_gil;
  Engine::thread_init(device, ready_queue, false);

  if (should_increment) {
    // Decrement the count during shutdown if we incremented earlier.
    decrement_non_reentrant_thread_count();
  }

#if defined(IS_PYTHON_3_9_PLUS)
  // Do not call PyEval_RestoreThread, PyThreadState_[Clear|DeleteCurrent] if
  // runtime is finalizing
  if (!Py_IsInitialized()) {
    no_gil.disarm();
    // TODO: call disarm rather than leak gil_scoped_acquired once
    // PyThreadState_Clear can safely be called from finalize NOTE: deploy.cpp
    // calls `PyInterpreterState_Delete` to destruct PyThreadState, so avoid
    // use-after-free here.
    gil.release();
  }
#endif
}

void PythonEngine::thread_on_exception(
    std::shared_ptr<GraphTask> graph_task,
    const std::shared_ptr<Node>& fn,
    std::exception& e) {
  auto python_err = dynamic_cast<python_error*>(&e);
  if (python_err) {
    python_err->persist();
  }
  Engine::thread_on_exception(std::move(graph_task), fn, e);
}

std::unique_ptr<AnomalyMetadata> PythonEngine::make_anomaly_metadata() {
  return std::unique_ptr<AnomalyMetadata>(new PyAnomalyMetadata());
}

std::unique_ptr<SavedVariableHooks> PythonEngine::
    get_default_saved_variable_hooks() {
  return PyDefaultSavedVariableHooks::get_hooks();
}

variable_list PythonEngine::execute(
    const edge_list& roots,
    const variable_list& inputs,
    bool keep_graph,
    bool create_graph,
    bool accumulate_grad,
    const edge_list& outputs) {
  TORCH_CHECK(
      !PyGILState_Check(),
      "The autograd engine was called while holding the GIL. If you are using the C++ "
      "API, the autograd engine is an expensive operation that does not require the "
      "GIL to be held so you should release it with 'pybind11::gil_scoped_release no_gil;'"
      ". If you are not using the C++ API, please report a bug to the pytorch team.")
  try {
    return Engine::execute(
        roots, inputs, keep_graph, create_graph, accumulate_grad, outputs);
  } catch (python_error& e) {
    e.restore();
    throw;
  }
}

c10::intrusive_ptr<at::ivalue::Future> PythonEngine::execute_with_graph_task(
    const std::shared_ptr<GraphTask>& graph_task,
    std::shared_ptr<Node> graph_root,
    InputBuffer&& input_buffer) {
  try {
    return Engine::execute_with_graph_task(
        graph_task, std::move(graph_root), std::move(input_buffer));
  } catch (python_error& e) {
    pybind11::gil_scoped_acquire gil;
    if (!PyErr_Occurred()) {
      // Set the error indicator only if it is not set already.
      e.restore();
    }
    throw;
  }
}
} // namespace python
} // namespace autograd
} // namespace torch

PyObject* THPEngineClass = nullptr;

/*
THPEngine_run_backward 是 C++ 引擎的入口，位于：torch/csrc/autograd/python_engine.cpp。

主要逻辑如下：

首先，是通过函数PyArg_ParseTupleAndKeywords对输入的参数重新解析，并赋值给新定义的变量:

新的变量为：tensors，grad_tensors，keep_graph，create_graph，inputs以及allow_unreachable。比如 inputs就是一个vector。
python世界中的输入是 torch.autograd.backward(tensors, grad_tensors)，这些参数分别转换被成了C++世界中的tensors和grad_tensors变量。这两个变量在C++中的类型是PyObject，并且size为1。PyObject是任何python对象的基类，在本方法之中，tensors和grad_tensors 其实是THPVariable类的实例。
从输入获取输入张量和梯度张量，主要是检查tensors和grad_tensors的变量类型以及tuple size是否一致。

依据输入构建了三个变量 edge_list roots，output_edges 和variable_list grads，这三个分别是反向传播（求导）的起始点，模型最终输出的边信息和梯度。

roots是包含有前向传播输出节点的 gradient_edge()（即输出节点的(grad_fn_, 0)）的 vector。需要注意，grad_fn_ 是 Node 的派生类。
grads 是前向传播产生的梯度，如果没有配置，则初始化为(tensor(1.),)。
output_edges 是依据前向传播输入节点 inputs 构建的后向传播输出边。
调用outputs = engine.execute(roots, grads, keep_graph, create_graph, output_edges)，正式进入反向传播引擎。
*/
// Implementation of torch._C._EngineBase.run_backward
PyObject* THPEngine_run_backward(
    PyObject* self,
    PyObject* args,
    PyObject* kwargs) {
  HANDLE_TH_ERRORS
  PyObject* tensors = nullptr;
  PyObject* grad_tensors = nullptr;
  unsigned char keep_graph = 0;
  unsigned char create_graph = 0;
  PyObject* inputs = nullptr;
  unsigned char allow_unreachable = 0;
  unsigned char accumulate_grad =
      0; // Indicate whether to accumulate grad into leaf Tensors or capture
  constexpr char* accepted_kwargs[] = {// NOLINT
                                       "tensors",
                                       "grad_tensors",
                                       "keep_graph",
                                       "create_graph",
                                       "inputs",
                                       "allow_unreachable",
                                       "accumulate_grad",
                                       nullptr};
  //// 对输入的参数重新解析并赋值给新定义的变量tensors,grad_tensors等等，比如 inputs就是一个vector
  if (!PyArg_ParseTupleAndKeywords(
          args,
          kwargs,
          "OObb|Obb",
          const_cast<char**>(accepted_kwargs),
          &tensors,
          &grad_tensors,
          &keep_graph,
          &create_graph,
          &inputs,
          &allow_unreachable,
          &accumulate_grad))
    return nullptr;
  THPUtils_assert(
      PyTuple_Check(tensors),
      "tensors argument is expected to "
      "be a tuple, but got %s",
      THPUtils_typename(tensors));
  THPUtils_assert(
      PyTuple_Check(grad_tensors),
      "grad_tensors argument is "
      "expected to be a tuple, but got %s",
      THPUtils_typename(grad_tensors));

  //// 从输入获取输入张量和梯度张量，主要是检查tensors和grad_tensors的变量类型以及tuple size是否一致。
  Py_ssize_t num_tensors = PyTuple_GET_SIZE(tensors);
  Py_ssize_t num_gradients = PyTuple_GET_SIZE(grad_tensors);
  THPUtils_assert(
      num_tensors == num_gradients,
      "got %ld tensors and %ld "
      "gradients",
      num_tensors,
      num_gradients);

  // The user either called autograd.backward(...) or autograd.grad(...) to get
  // here
  bool backward_api_called = accumulate_grad;
  TORCH_CHECK(
      !backward_api_called || at::impl::VmapMode::current_vmap_level() == 0,
      "backward() called inside torch.vmap. This is not supported, "
      "please call backward() outside torch.vmap or instead use "
      "torch.autograd.grad inside torch.vmap");

  // 我们回忆一下定义
  // using variable_list = std::vector<Variable>;
  // using edge_list = std::vector<Edge>
  edge_list roots; // 就是反向传播的起点（根节点）
  roots.reserve(num_tensors);
  variable_list grads; // 就是反向传播的梯度
  grads.reserve(num_tensors);

  // 依据输入来配置roots和grads
  for (const auto i : c10::irange(num_tensors)) {
  // tensors是输入节点，即前向传播图的输出
    PyObject* _tensor = PyTuple_GET_ITEM(tensors, i);
    THPUtils_assert(
        THPVariable_Check(_tensor),
        "element %d of tensors "
        "tuple is not a Tensor",
        i);
    const auto& variable = THPVariable_Unpack(_tensor);
    TORCH_CHECK(
        !isBatchedTensor(variable),
        "torch.autograd.grad(outputs, inputs, grad_outputs) called inside ",
        "torch.vmap. We do not support the case where any outputs are ",
        "vmapped tensors (output ",
        i,
        " is being vmapped over). Please "
        "call autograd.grad() outside torch.vmap or file a bug report "
        "with your use case.")
    //// 得到 gradient_edge = Edge(grad_fn(), output_nr())
    auto gradient_edge = torch::autograd::impl::gradient_edge(variable);
    THPUtils_assert(
        gradient_edge.function,
        "element %d of tensors does not require grad and does not have a grad_fn",
        i);
    roots.push_back(std::move(gradient_edge));  // root增加一个Edge

    PyObject* grad = PyTuple_GET_ITEM(grad_tensors, i);
    if (THPVariable_Check(grad)) {
      const Variable& grad_var = THPVariable_Unpack(grad);
      if (grad_var.has_names()) {
        TORCH_WARN(
            "Autograd was passed a named grad tensor with dims ",
            grad_var.names(),
            ". Autograd does not yet support named tensor semantics, so all names ",
            "will be ignored. In practice all computed gradients will still be correct "
            "according to regular tensor semantics.");
      }
      grads.push_back(grad_var); // 增加一个梯度
    } else {
      THPUtils_assert(
          grad == Py_None,
          "element %d of gradients tuple is not a Tensor or None",
          i);
      THPUtils_assert(
          !variable.requires_grad(),
          "element %d of gradients tuple is None, but the corresponding Tensor requires grad");
    }
  }
  // 构建一个输出Edge列表
  std::vector<Edge> output_edges;
  if (inputs != nullptr) {
    int num_inputs = PyTuple_GET_SIZE(inputs);
    output_edges.reserve(num_inputs);

    // 遍历输入列表
    for (const auto i : c10::irange(num_inputs)) {
      PyObject* input = PyTuple_GET_ITEM(inputs, i);
      THPUtils_assert(
          THPVariable_Check(input),
          "all inputs have to be Tensors, but got %s",
          THPUtils_typename(input));
      const auto& tensor = THPVariable_Unpack(input);
      TORCH_CHECK(
          !isBatchedTensor(tensor),
          "torch.autograd.grad(outputs, inputs, grad_outputs) called inside ",
          "torch.vmap. We do not support the case where any inputs are ",
          "vmapped tensors (input ",
          i,
          " is being vmapped over). Please "
          "call autograd.grad() outside torch.vmap or file a bug report "
          "with your use case.")
      const auto output_nr = tensor.output_nr();
      auto grad_fn = tensor.grad_fn();
      if (!grad_fn) {
      // 获取 grad_accumulator，用来判断是否是叶子节点
        grad_fn = torch::autograd::impl::try_get_grad_accumulator(tensor);
      }
      if (accumulate_grad) {
        tensor.retain_grad();
      }
      THPUtils_assert(
          tensor.requires_grad(),
          "One of the differentiated Tensors does not require grad");
      if (!grad_fn) {
        // NOTE [ Autograd Unreachable Input ]
        // Since input has no grad_accumulator, its guaranteed to be
        // unreachable. We initialize an edge pointing to a non-nullptr Node so
        // nodes in the graph (e.g., mul when an operand is scalar) that have
        // edges pointing to nullptr don't get erroneously assigned `needed =
        // True` in exec_info.
        // 说明是叶子节点
        output_edges.emplace_back(std::make_shared<Identity>(), 0);
      } else {
        // 是中间节点
        output_edges.emplace_back(grad_fn, output_nr);
      }
    }
  }

  // 现在，roots是包含有(前向传播输出节点的grad_fn_, 0)的vector。
  // grads 是前向传播产生的梯度，如果没有配置，则初始化为(tensor(1.),)
  // output_edges 是依据前向传播输入节点 input 构建的后向传播输出边
  variable_list outputs;
  {
    pybind11::gil_scoped_release no_gil;
    auto& engine = python::PythonEngine::get_python_engine();

    // 进入引擎执行
    outputs = engine.execute(
        roots, grads, keep_graph, create_graph, accumulate_grad, output_edges);
  }

  if (!backward_api_called && inputs != nullptr) {
    int num_inputs = PyTuple_GET_SIZE(inputs);
    THPObjectPtr py_outputs{PyTuple_New(num_inputs)};
    if (!py_outputs)
      return nullptr;
    for (const auto i : c10::irange(num_inputs)) {
      THPUtils_assert(
          allow_unreachable || outputs[i].defined(),
          "One of the "
          "differentiated Tensors appears to not have been used "
          "in the graph. Set allow_unused=True if this is the "
          "desired behavior.");
      PyTuple_SET_ITEM(py_outputs.get(), i, THPVariable_Wrap(outputs[i]));
    }
    return py_outputs.release();
  } else {
    Py_RETURN_NONE;
  }
  END_HANDLE_TH_ERRORS
}

PyObject* THPEngine_queue_callback(PyObject* self, PyObject* _callback) {
  HANDLE_TH_ERRORS
  auto& engine = python::PythonEngine::get_python_engine();
  std::shared_ptr<PyObject> callback(_callback, [](PyObject* obj) {
    pybind11::gil_scoped_acquire gil;
    Py_DECREF(obj);
  });
  Py_INCREF(_callback);
  engine.queue_callback([callback]() {
    pybind11::gil_scoped_acquire gil;
    THPObjectPtr result{PyObject_CallFunctionObjArgs(callback.get(), nullptr)};
    if (!result)
      throw python_error();
  });
  Py_RETURN_NONE;
  END_HANDLE_TH_ERRORS
}

PyObject* THPEngine_is_checkpoint_valid(PyObject* self, PyObject* noargs) {
  HANDLE_TH_ERRORS
  auto& engine = python::PythonEngine::get_python_engine();
  if (engine.is_checkpoint_valid()) {
    Py_RETURN_TRUE;
  } else {
    Py_RETURN_FALSE;
  }
  END_HANDLE_TH_ERRORS
}

PyObject* THPEngine_new(PyTypeObject* type, PyObject* args, PyObject* kwargs) {
  return type->tp_alloc(type, 0);
}
//对于torch._C._EngineBase，其成员函数是 THPEngine_methods。THPEngine_methods 的类型就是我们前面介绍的 PyMethodDef，用来进行 Python 拓展。
//这里定义了 run_backward，queue_callback 和 is_checkpoint_valid。我们回忆一下，run_backward 就是 Python世界的切入点。
// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-avoid-non-const-global-variables)
static struct PyMethodDef THPEngine_methods[] = {
    {(char*)"run_backward",
     castPyCFunctionWithKeywords(THPEngine_run_backward),  // 与Python对应
     METH_VARARGS | METH_KEYWORDS,
     nullptr},
    {(char*)"queue_callback", THPEngine_queue_callback, METH_O, nullptr},
    {(char*)"is_checkpoint_valid",
     THPEngine_is_checkpoint_valid,
     METH_NOARGS,
     nullptr},
    {nullptr}};

PyTypeObject THPEngineType = {
    PyVarObject_HEAD_INIT(nullptr, 0) "torch._C._EngineBase", /* tp_name */
    sizeof(THPEngine), /* tp_basicsize */
    0, /* tp_itemsize */
    nullptr, /* tp_dealloc */
    0, /* tp_vectorcall_offset */
    nullptr, /* tp_getattr */
    nullptr, /* tp_setattr */
    nullptr, /* tp_reserved */
    nullptr, /* tp_repr */
    nullptr, /* tp_as_number */
    nullptr, /* tp_as_sequence */
    nullptr, /* tp_as_mapping */
    nullptr, /* tp_hash  */
    nullptr, /* tp_call */
    nullptr, /* tp_str */
    nullptr, /* tp_getattro */
    nullptr, /* tp_setattro */
    nullptr, /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    nullptr, /* tp_doc */
    nullptr, /* tp_traverse */
    nullptr, /* tp_clear */
    nullptr, /* tp_richcompare */
    0, /* tp_weaklistoffset */
    nullptr, /* tp_iter */
    nullptr, /* tp_iternext */
    THPEngine_methods, /* tp_methods */
    nullptr, /* tp_members */
    nullptr, /* tp_getset */
    nullptr, /* tp_base */
    nullptr, /* tp_dict */
    nullptr, /* tp_descr_get */
    nullptr, /* tp_descr_set */
    0, /* tp_dictoffset */
    nullptr, /* tp_init */
    nullptr, /* tp_alloc */
    THPEngine_new /* tp_new */
};

static void child_atfork() {
  _reinitialize_engine = true;
}
/*
THPEngine_initModule(module)创建了torch._C._EngineBase，
Engine实现了从中间输出的variable的grad到root variable（用户创建的Variable）之间的反向传播:


上面的初始化代码向torch._C中注册了_ImperativeEngine属性。在variable的python接口中（torch/autograd/variable.py），会以下面的方式使用_ImperativeEngine：

from torch._C import _ImperativeEngine as ImperativeEngine
Variable._execution_engine = ImperativeEngine()
除此之外，还需要注册默认的engine stub（其实就是返回初始化好的PythonEngine）：

static torch::autograd::python::PythonEngine engine;

std::atomic<EngineStub> engine_stub(get_base_engine);

void set_default_engine_stub(EngineStub stub) {
  engine_stub.store(stub);
}

static Engine& get_python_engine() {
  return engine;
}

set_default_engine_stub(get_python_engine);
如果Python是启用的话（当然了，毕竟是PyTorch嘛），那么base engine就是一个Python Engine。


*/
bool THPEngine_initModule(PyObject* module) {
#ifndef _WIN32
  if (pthread_atfork(nullptr, nullptr, child_atfork) != 0) {
    throw std::runtime_error("unable to set pthread_atfork handler");
  }
#endif
  if (PyType_Ready(&THPEngineType) < 0)
    return false;
  Py_INCREF(&THPEngineType);

  // 为 Python 注册了引擎
  PyModule_AddObject(module, "_ImperativeEngine", (PyObject*)&THPEngineType);
  set_default_engine_stub(python::PythonEngine::get_python_engine);
  return true;
}
