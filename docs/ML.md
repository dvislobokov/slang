# Slang для машинного обучения

Цель: не заменить PyTorch, а быть быстрым языком для инференса,
препроцессинга, сервисов вокруг моделей и собственных моделей, который
без трения работает с экосистемой Python. База уже есть: нативный код без
GC, LLVM (векторизация, GPU-бэкенды, Enzyme), generics, compile-time
рефлексия, async.

## 1. Язык

| Возможность | Зачем |
|---|---|
| Перегрузка операторов через фиксированные интерфейсы `Add`, `Sub`, `Mul`, `Div`, `Rem`, `Neg`, `MatMul` (`@`), `Index`, `Compare` | `a @ b + c` вместо `MatMul(Add(a, b), c)`; никаких произвольных операторов |
| Многомерные срезы `t[1.., ..3]`, `t[.., 0]`, `t[::2, 1]` | views без копий, как NumPy |
| Числовые типы `float16`, `bfloat16`, `int8`, `int4` (упакованный), `complex64/128` | квантизация, смешанная точность |
| SIMD-типы `float32x8`, `int32x4` с арифметикой и `simd.Load/Store/Reduce`; `@fastmath` | ложатся на AVX-512/NEON напрямую |
| Многомерные литералы `[[1, 2], [3, 4]]` → `Tensor<int>` по контексту | удобство |
| `@kernel` функции → PTX (CUDA), SPIR-V (Vulkan compute), Metal через LLVM; `kernel.Launch(grid, block, args)` | один язык для хоста и GPU, как CUDA C++ и Mojo |
| `grad(f)`, `jvp`, `vjp` — автодифференцирование на уровне LLVM IR через Enzyme | градиенты любой функции без tape и графа, со скоростью ручного кода |
| `@extern` FFI к C с zero-copy буферами | BLAS, oneDNN, cuBLAS, ONNX Runtime, llama.cpp |

Перегрузка операторов — единственное изменение, затрагивающее общий
дизайн; оно зафиксировано в DESIGN.md («Перегрузка операторов»).

## 2. `std.tensor`

```slang
import std.tensor

var x = tensor.Rand<float32>([64, 784])
var w = tensor.Zeros<float32>([784, 10])
var y = (x @ w + b).Softmax(axis: 1)
var loss = y.CrossEntropy(labels).Mean()
var g = grad((w) => (x @ w + b).Softmax(1).CrossEntropy(labels).Mean())(w)
w -= g * 0.01
```

- `Tensor<T>`: форма, strides, device, views, broadcasting по правилам
  NumPy, in-place операции при единственном владельце (наш ARC это знает).
- Бэкенды: CPU (SIMD + задачи планировщика), BLAS через FFI, GPU через
  `@kernel`; `t.To(device.Cuda(0))`.
- Слияние цепочек операций (fusion) компилятором там, где типы известны.
- `std.tensor.nn`: слои, оптимизаторы, инициализация; `std.tensor.io`:
  safetensors, GGUF, ONNX (загрузка графов и выполнение через ORT или свой
  интерпретатор), NPY.

## 3. Интероп

- **Python**: `python.Import("torch")` из Slang (встроенный интерпретатор
  через C API), и Slang-модули как расширения Python (`slang build
  --python-module`); массивы через DLPack и buffer protocol без копий.
  Обучение в PyTorch — инференс и сервис на Slang, в одном репозитории.
- **Данные**: `std.data` — Arrow (zero-copy), Parquet, CSV, `data.Frame<...>`
  датафрейм на generics с ленивыми запросами (как Polars).
- **Модели**: ONNX Runtime, llama.cpp, TensorRT через `@extern`.

## 4. Инструменты

- JIT через LLVM ORC → REPL `slang repl` и ядро Jupyter.
- `slang bench`, `slang.diag` с профилем GPU-ядер.
- Типы тензоров с формой в LSP (`Tensor<float32, [64, 784]>` при
  статически известной форме; проверка размерностей при компиляции там,
  где форма константна).

## Порядок

1. Перегрузка операторов, `float16/bfloat16`, SIMD-типы, многомерные
   срезы (компилятор).
2. `std.tensor` CPU с SIMD и BLAS, safetensors/NPY, `std.data` Arrow/Parquet.
3. `@extern` FFI полноценный, Python-интероп через DLPack.
4. Enzyme-автодифференцирование, `@kernel` для CUDA/Vulkan/Metal.
5. REPL/Jupyter, `nn`, ONNX.
