"""Repository paths for the Python tests (run: python3 -m unittest discover -s tests)."""
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / 'scripts') not in sys.path:
    sys.path.insert(0, str(ROOT / 'scripts'))

# The built-in mods (scripts/builtin_mods) stay out of the library tests unless a test names them.
os.environ.setdefault('BB_BUILTIN_MODS', '')
