from dataclasses import dataclass
from typing import List, Optional, Set

from torchgen.api.types import Binding, CType
from torchgen.model import FunctionSchema, NativeFunction


@dataclass(frozen=True)
class CppSignature:
    """
    This signature is merely a CppSignature with Executorch types. The inline definition
    of CppSignature is generated in Functions.h and it's used by unboxing functions.
    """

    # The schema this signature is derived from
    func: FunctionSchema

    # The set of C++ arguments which should not have defaults applied to them
    cpp_no_default_args: Set[str]

    # Allows you to prepend an arbitrary prefix to the signature name.
    # This is useful for parts of the codegen that generate wrappers around kernels,
    # and need to avoid naming collisions.
    prefix: str = ""

    def arguments(self) -> List[Binding]:
        return cpp.arguments(
            self.func.arguments,
            faithful=True,  # always faithful, out argument at the end
            method=False,  # method not supported
            cpp_no_default_args=self.cpp_no_default_args,
        )

    def name(self) -> str:
        return self.prefix + cpp.name(
            self.func,
            faithful_name_for_out_overloads=True,
        )

    def decl(self, name: Optional[str] = None) -> str:
        args_str = ", ".join(a.decl() for a in self.arguments())
        if name is None:
            name = self.name()
        return f"{self.returns_type().cpp_type()} {name}({args_str})"

    def defn(self, name: Optional[str] = None) -> str:
        args = [a.defn() for a in self.arguments()]
        args_str = ", ".join(args)
        if name is None:
            name = self.name()
        return f"{self.returns_type().cpp_type()} {name}({args_str})"

    def returns_type(self) -> CType:
        return cpp.returns_type(self.func.returns)

    @staticmethod
    def from_native_function(f: NativeFunction, *, prefix: str = "") -> "CppSignature":
        return CppSignature(
            func=f.func, prefix=prefix, cpp_no_default_args=f.cpp_no_default_args
        )


from torchgen.executorch.api import cpp
