/*
Copyright (c) Fraunhofer ITWM 

This file is part of gpi_cp.

gpi_cp is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License
version 3 as published by the Free Software Foundation.

gpi_cp is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with gpi_cp. If not, see <http://www.gnu.org/licenses/>.
*/


#include <GASPI.h>
#include <gpi_cp.h>

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

#define ASSERT(ec) assert (ec);

#define ERROR(message)                          \
  do                                            \
  {                                             \
    printf ( "Error[%s:%i]: %s\n"               \
                 , __FILE__, __LINE__, message  \
                 );                             \
                                                \
    exit (EXIT_FAILURE);                        \
  } while (0)


#define SUCCESS_OR_DIE(f, args...)		\
  do						\
  {						\
    gaspi_return_t const r = f (args);	\
						\
    if (r != GASPI_SUCCESS)			\
    {						\
      ERROR (gaspi_error_str (r));		\
    }						\
  } while (0)

int
main(int argc, char *argv[])
{
  SUCCESS_OR_DIE (gaspi_proc_init, GASPI_BLOCK);

  gaspi_rank_t iProc;
  gaspi_rank_t nProc;

  SUCCESS_OR_DIE (gaspi_proc_rank, &iProc);
  SUCCESS_OR_DIE (gaspi_proc_num, &nProc);

  { 
      gaspi_segment_id_t unused_segment_id;
      SUCCESS_OR_DIE (gpi_cp_get_unused_segment_id, &unused_segment_id );
      
      ASSERT(unused_segment_id == 0);
  }

  SUCCESS_OR_DIE (gaspi_segment_create, 0, 1024, GASPI_GROUP_ALL,
					GASPI_BLOCK, GASPI_MEM_UNINITIALIZED);
  { // first segment allocated
      gaspi_segment_id_t unused_segment_id;
      SUCCESS_OR_DIE (gpi_cp_get_unused_segment_id, &unused_segment_id );
      
      ASSERT(unused_segment_id == 1);
  }

  SUCCESS_OR_DIE (gaspi_segment_create, 2, 1024, GASPI_GROUP_ALL,
					GASPI_BLOCK, GASPI_MEM_UNINITIALIZED);
  { // non-sequential segments allocated
      gaspi_segment_id_t unused_segment_id;
      SUCCESS_OR_DIE (gpi_cp_get_unused_segment_id, &unused_segment_id );
      
      ASSERT(unused_segment_id == 1);
  }

  SUCCESS_OR_DIE (gaspi_segment_create, 1, 1024, GASPI_GROUP_ALL,
					GASPI_BLOCK, GASPI_MEM_UNINITIALIZED);
  { // sequential segments allocated
      gaspi_segment_id_t unused_segment_id;
      SUCCESS_OR_DIE (gpi_cp_get_unused_segment_id, &unused_segment_id );
      
      ASSERT(unused_segment_id == 3);
  }

  SUCCESS_OR_DIE (gaspi_barrier, GASPI_GROUP_ALL, GASPI_BLOCK);
  SUCCESS_OR_DIE (gaspi_proc_term, GASPI_BLOCK);

  return EXIT_SUCCESS;
}
