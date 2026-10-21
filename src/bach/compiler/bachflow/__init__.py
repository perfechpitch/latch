"""BachFlow packed layer to a BACHIR 12 bundle.

Run ``python -m bach.compiler.bachflow`` from this package's ``src``.
"""

from .convert import Bundle, ConvertError, convert

__all__ = ["Bundle", "ConvertError", "convert"]
