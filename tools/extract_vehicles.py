"""MOVED to tools/py_extract_archive/extract_vehicles.py -- superseded by the C extractor
(tools/cextract).  Stub executes the ARCHIVED module with __file__ kept at
the ORIGINAL tools/ location so its relative paths resolve unchanged.
Executes into THIS module's own globals: a fresh-module swap into
sys.modules satisfies plain  but NOT spec_from_file_location /
exec_module, which hands the caller the module object IT created (that bug
silently killed every retail backend once).  Do not add code here."""
import os as _os
_here = _os.path.abspath(__file__)
_arch = _os.path.join(_os.path.dirname(_here), "py_extract_archive", "extract_vehicles.py")
with open(_arch) as _f:
    _code = compile(_f.read(), _here, "exec")
exec(_code, globals())
