"""The k27 tree for the Python tools: tooling/k27/k27-tree.sh's rule (PIANO_K27_TREE from the environment, else the line
PIANO_K27_TREE=/path in ~/.config/piano/env; no committed file names it). The tree is the local XNU 27.0.1 kernel tree
(xnu-12377 + the baseline series 0001-0052) that patches/k27 applies to; the native build only reads it.

    from k27_tree import k27_tree      (a script in tooling/k27; others put tooling/k27 on sys.path first)
    TREE = k27_tree()                  # a Path, or the script stops with what to set
    TREE = k27_tree(required=False)    # None when it is not set
"""
import os
import sys
from pathlib import Path

HOW = ('the k27 tree (xnu-12377 + the baseline series 0001-0052, XNU 27.0.1) the native build reads: set it in the '
       'environment or as PIANO_K27_TREE=/path in ~/.config/piano/env (docs/bringup-debugging.md §4)')


def configured():
    """PIANO_K27_TREE as configured (environment first, then ~/.config/piano/env), or None."""
    value = os.environ.get('PIANO_K27_TREE')
    if not value:
        env = Path.home() / '.config/piano/env'
        if env.is_file():
            for line in env.read_text().splitlines():
                line = line.strip().removeprefix('export ').strip()
                if line.startswith('PIANO_K27_TREE='):
                    value = line.split('=', 1)[1].strip().strip('"').strip("'")
    return Path(os.path.expanduser(os.path.expandvars(value))) if value else None


def k27_tree(required=True):
    tree = configured()
    if tree is None:
        if required:
            sys.exit(f'PIANO_K27_TREE is not set: {HOW}')
        return None
    if not tree.is_dir():
        if required:
            sys.exit(f'PIANO_K27_TREE={tree} is not a directory: {HOW}')
        return None
    return tree
