/* Copyright (C) 1991, 1995, 1996, 1997, 2002 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "ros_syscall.h"
#include "ros_stat.h"

/* Get information about the file descriptor FD in BUF.  */
int
__fxstat (int vers, int fd, struct stat *buf)
{
  if (vers != _STAT_VER)
    {
      __set_errno (EINVAL);
      return -1;
    }

  if (fd < 0)
    {
      __set_errno (EBADF);
      return -1;
    }
  else if (buf == NULL)
    {
      __set_errno (EINVAL);
      return -1;
    }

  
  struct newlib_stat nst;
  int ret = (int)ros_syscall(SYS_fstat,fd,&nst,0,0,0);
  __convert_stat(&nst,buf);
  return ret;
}
stub_warning (fstat)
hidden_def (__fxstat)
weak_alias (__fxstat, _fxstat)
#include <stub-tag.h>
