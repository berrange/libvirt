#!/usr/bin/env python
#
# Copyright (C) 2012-2019 Red Hat, Inc.
#
# check-spacing.pl: Report any usage of 'function (..args..)'
# Also check for other syntax issues, such as correct use of ';'
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library.  If not, see
# <http://www.gnu.org/licenses/>.

from __future__ import print_function

import re
import sys


def check_whitespace(filename):
    errs = False
    with open(filename, 'r') as fh:
        quotedmetaprog = re.compile(r"""'[";,=]'""")
        quotedstringprog = re.compile(r'''"(?:[^\\\"]|\\.)*"''')
        commentstartprog = re.compile(r'''^(.*)/\*.*$''')
        commentendprog = re.compile(r'''^.*\*/(.*)$''')
        commentprog = re.compile(r'''^(.*)/\*.*\*/(.*)''')
        funcprog = re.compile(r'''(\w+)\s\((?!\*)''')
        keywordprog = re.compile(
            r'''^.*\b(?:if|for|while|switch|return)\(.*$''')
        functypedefprog = re.compile(r'''^.*\(\*\w+\)\s+\(.*$''')
        whitespaceprog1 = re.compile(r'''^.*\s\).*$''')
        whitespaceprog2 = re.compile(r'''^\s+\);?$''')
        whitespaceprog3 = re.compile(r'''^.*\((?!$)\s.*''')
        commasemiprog1 = re.compile(r'''.*\s[;,].*''')
        commasemiprog2 = re.compile(r'''.*\S; ; .*''')
        commasemiprog3 = re.compile(r'''^\s+;''')
        semicolonprog = re.compile(r'''.*;[^	 \\\n;)].*''')
        commaprog = re.compile(r'''.*,[^ \\\n)}].*''')
        assignprog1 = re.compile(r'''[^ ]\b[!<>&|\-+*\/%\^=]?=''')
        assignprog2 = re.compile(r'''=[^= \\\n]''')
        condstartprog = re.compile(r'''^\s*(if|while|for)\b.*\{$''')
        statementprog = re.compile(r'''^[^;]*;[^;]*$''')
        condendprog = re.compile(r'''^\s*}\s*$''')

        incomment = False
        # Per-file variables for multiline Curly Bracket (cb_) check
        cb_lineno = 0
        cb_code = ""
        cb_scolon = False

        lineno = 0
        for line in fh:
            lineno = lineno + 1
            data = line
            # For temporary modifications

            # Kill any quoted , ; = or "
            data = quotedmetaprog.sub("'X'", data)

            # Kill any quoted strings
            data = quotedstringprog.sub('"XXX"', data)

            if data[0] == '#':
                continue

            # Kill contents of multi-line comments
            # and detect end of multi-line comments
            if incomment:
                if commentendprog.match(data):
                    data = commentendprog.sub('*/\2', data)
                    incomment = False
                else:
                    data = ""

            # Kill single line comments, and detect
            # start of multi-line comments
            if commentprog.match(data):
                data = commentprog.sub(r'''\1/* */\2''', data)
            elif commentstartprog.match(data):
                data = commentstartprog.sub(r'''\1/*''', data)
                incomment = True

            # We need to match things like
            #
            #  int foo (int bar, bool wizz);
            #  foo (bar, wizz);
            #
            # but not match things like:
            #
            #  typedef int (*foo)(bar wizz)
            #
            # we can't do this (efficiently) without
            # missing things like
            #
            #  foo (*bar, wizz);
            #
            for match in funcprog.finditer(data):
                kw = match.group(1)

                # Allow space after keywords only
                if kw not in ["if", "for", "while", "switch", "return"]:
                    print("Whitespace after non-keyword:",
                          file=sys.stderr)
                    print("%s:%d: %s" % (filename, lineno, line),
                          file=sys.stderr)
                    errs = True
                    break

            # Require whitespace immediately after keywords
            if keywordprog.match(data):
                print("No whitespace after keyword:",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # Forbid whitespace between )( of a function typedef
            if functypedefprog.match(data):
                print("Whitespace between ')' and '(':",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # Forbid whitespace following ( or prior to )
            # but allow whitespace before ) on a single line
            # (optionally followed by a semicolon)
            if ((whitespaceprog1.match(data) and
                 not whitespaceprog2.match(data))
                or whitespaceprog3.match(data)):
                print("Whitespace after '(' or before ')':",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # Forbid whitespace before ";" or ",". Things like
            # below are allowed:
            #
            # 1) The expression is empty for "for" loop. E.g.
            #   for (i = 0; ; i++)
            #
            # 2) An empty statement. E.g.
            #   while (write(statuswrite, &status, 1) == -1 &&
            #          errno == EINTR)
            #       ;
            #
            if commasemiprog1.match(data) and not (
                    commasemiprog2.match(data) or
                    commasemiprog3.match(data)):
                print("Whitespace before semicolon or comma:",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # Require EOL, macro line continuation, or whitespace after ";".
            # Allow "for (;;)" as an exception.
            if semicolonprog.match(data):
                print("Invalid character after semicolon:",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # Require EOL, space, or enum/struct end after comma.
            if commaprog.match(data):
                print("Invalid character after comma:",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # Require spaces around assignment '=', compounds and '=='
            if assignprog1.match(data) or assignprog2.match(data):
                print("Spacing around '=' or '==':",
                      file=sys.stderr)
                print("%s:%d: %s" % (filename, lineno, line),
                      file=sys.stderr)
                errs = True

            # One line conditional statements with one line bodies should
            # not use curly brackets.
            if condstartprog.match(data):
                cb_lineno = lineno
                cb_code = line
                cb_scolon = False

            # We need to check for exactly one semicolon inside the body,
            # because empty statements (e.g. with comment only) are
            # allowed
            if (cb_lineno == lineno - 1) and statementprog.match(data):
                cb_code = cb_code + line
                cb_scolon = True

            if (condendprog.match(data) and
                (cb_lineno == lineno - 2) and
                cb_scolon):
                print("Curly brackets around single-line body:",
                      file=sys.stderr)
                print("%s:%d:\n%s%s" % (filename, cb_lineno - lineno,
                                        cb_code, line),
                      file=sys.stderr)
                errs = True

                # There _should_ be no need to reset the values; but to
                # keep my inner peace...
                cb_lineno = 0
                cb_scolon = False
                cb_code = ""

    return errs


ret = 0
for filename in sys.argv[1:]:
    if check_whitespace(filename):
        ret = 1

sys.exit(ret)
