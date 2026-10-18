"""Windows bundle: the data tools use relative paths as names inside the game data (and in GitHub URLs),
which must use forward slashes, so os.path.relpath returns them that way here. Windows accepts both."""
import os
import ntpath

_relpath = ntpath.relpath


def _relpath_slash(path, start=None):
    return _relpath(path, start).replace('\\', '/')


ntpath.relpath = _relpath_slash
os.path.relpath = _relpath_slash
