dnl Golang checks
dnl
dnl Copyright (C) 2019 Red Hat, Inc.
dnl
dnl This library is free software; you can redistribute it and/or
dnl modify it under the terms of the GNU Lesser General Public
dnl License as published by the Free Software Foundation; either
dnl version 2.1 of the License, or (at your option) any later version.
dnl
dnl This library is distributed in the hope that it will be useful,
dnl but WITHOUT ANY WARRANTY; without even the implied warranty of
dnl MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
dnl Lesser General Public License for more details.
dnl
dnl You should have received a copy of the GNU Lesser General Public
dnl License along with this library.  If not, see
dnl <http://www.gnu.org/licenses/>.
dnl

AC_DEFUN([LIBVIRT_CHECK_GOLANG], [
  AC_PATH_PROGS([GO], [go], [no])
  if test "x$ac_cv_path_GO" != "xno"
  then
    with_go=yes
  else
    with_go=no
  fi
  AM_CONDITIONAL([HAVE_GOLANG], [test "$with_go" != "no"])

  if test "$with_go" != "no"
  then
    GOVERSION=`$ac_cv_path_GO version | awk '{print \$ 3}' | sed -e 's/go//' -e 's/rc.*//'`
    GOMAJOR=`echo $GOVERSION | awk -F . '{print \$ 1}'`
    GOMINOR=`echo $GOVERSION | awk -F . '{print \$ 2}'`

    dnl We want to use go modules which first arrived in 1.11
    AC_MSG_CHECKING([for Go version >= 1.11])
    if test "$GOMAJOR" != "1" || test "$GOMINOR" -lt "11"
    then
      with_go=no
      AC_MSG_RESULT([no])
    else
      AC_MSG_RESULT([yes])
    fi
  fi
])
