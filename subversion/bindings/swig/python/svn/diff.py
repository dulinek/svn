#
# diff.py: public Python interface for diff components
#
# Subversion is a tool for revision control.
# See http://subversion.tigris.org for more information.
#
######################################################################
#
# Copyright (c) 2007 CollabNet.  All rights reserved.
#
# This software is licensed as described in the file COPYING, which
# you should have received as part of this distribution.  The terms
# are also available at http://subversion.tigris.org/license-1.html.
# If newer versions of this license are posted there, you may use a
# newer version instead, at your option.
#
######################################################################

from libsvn.diff import *
from svn.core import _unprefix_names
_unprefix_names(locals(), 'svn_diff_')
del _unprefix_names
