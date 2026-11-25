"""thimsy-import: reads one ROCKNIX tag and writes a family's hardware layer as data.

Design: docs/linux/IMPLEMENTATION.md section E.7. Standard library only; runs on Python 3.8.
Nothing read from ROCKNIX is ever executed: shell files are parsed as text, and every
imperative block the importer translates is hash-guarded (guards.py).
"""

__version__ = "1"
