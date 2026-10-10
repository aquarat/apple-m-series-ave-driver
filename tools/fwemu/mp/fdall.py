# SPDX-License-Identifier: GPL-2.0-only
import os as _os
REPO_ROOT = _os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))))
import sys
sys.argv=['fd.py',sys.argv[1],'0',sys.argv[2]]
exec(open(_os.path.join(_os.path.dirname(_os.path.abspath(__file__)), 'fd.py')).read())
