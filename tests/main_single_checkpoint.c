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


#define SUCCESS_OR_DIE(r)                       \
  do						\
  {						\
    if (r != GASPI_SUCCESS)			\
    {						\
      ERROR (gaspi_error_str (r));		\
    }						\
  } while (0)

int
main(int argc, char *argv[])
{
  SUCCESS_OR_DIE (gaspi_proc_init( GASPI_BLOCK ));

  gaspi_rank_t iProc;
  gaspi_rank_t nProc;

  SUCCESS_OR_DIE (gaspi_proc_rank( &iProc ));
  SUCCESS_OR_DIE (gaspi_proc_num( &nProc ));

  const gaspi_rank_t spare = nProc-1;
  const gaspi_rank_t culprit = nProc-2;

  gaspi_segment_id_t segment_id_checkpoint = 1;
  gaspi_size_t const cp_data_size = 1024 * 1024;
  const int num_work_elems = cp_data_size / sizeof(int);

  SUCCESS_OR_DIE (gaspi_segment_create( segment_id_checkpoint, cp_data_size, GASPI_GROUP_ALL,
					GASPI_BLOCK, GASPI_MEM_INITIALIZED ));

  gaspi_pointer_t checkpoint_seg_ptr;
  SUCCESS_OR_DIE (gaspi_segment_ptr(segment_id_checkpoint, &checkpoint_seg_ptr) );
  volatile int* const work_array = (int *) checkpoint_seg_ptr;

  // init data
  for( int iwork = 0; iwork < num_work_elems; ++iwork)
  {
      work_array[iwork] = iProc + 1;
  }

  // create group
  gaspi_group_t g_active = GASPI_GROUP_ALL;
  if ( iProc != spare)
  {
      SUCCESS_OR_DIE (gaspi_group_create(&g_active));
      for(gaspi_rank_t i = 0; i < nProc; i++)
      {
          if(i != spare)
              SUCCESS_OR_DIE(gaspi_group_add(g_active, i));
      }
      SUCCESS_OR_DIE(gaspi_group_commit(g_active, GASPI_BLOCK));
  }

  gpi_cp_description_t checkpoint_description = GPI_CP_DESCRIPTION_INITIALIZER();


  if ( iProc != spare)
  {
      SUCCESS_OR_DIE ( gpi_cp_init ( segment_id_checkpoint
                                       , 0
                                       , cp_data_size
                                       , 4
                                       , GPI_CP_POLICY_RING
                                       , g_active
                                       , checkpoint_description
                                       , GASPI_BLOCK
                                    )
          );
      SUCCESS_OR_DIE ( gpi_cp_start (checkpoint_description, GASPI_BLOCK) );
  
      SUCCESS_OR_DIE ( gpi_cp_commit (checkpoint_description, GASPI_BLOCK));
  }

  // change data
  work_array[0] += nProc;

  if (iProc != culprit)
  {  
      if ( iProc != spare)
      {
          SUCCESS_OR_DIE(gaspi_group_delete(g_active));
      }

      SUCCESS_OR_DIE (gaspi_group_create(&g_active));
      for(gaspi_rank_t i = 0; i < nProc; i++)
      {
          if(i != culprit)
              SUCCESS_OR_DIE(gaspi_group_add(g_active, i));
      }
      SUCCESS_OR_DIE(gaspi_group_commit(g_active, GASPI_BLOCK));

      SUCCESS_OR_DIE(gpi_cp_restore( segment_id_checkpoint
                                       , 0
                                       , cp_data_size
                                       , 4
                                       , GPI_CP_POLICY_RING
                                       , g_active
                                       , checkpoint_description
                                       , GASPI_BLOCK
                         )
      );
  }

  // check restored data
  if (iProc == spare)
  {
      ASSERT(culprit + 1 == work_array[0] );
  }
  else
  { // unaffected nodes
      ASSERT(nProc + iProc + 1 == work_array[0] );
  }

  SUCCESS_OR_DIE ( gaspi_barrier(GASPI_GROUP_ALL, GASPI_BLOCK) );
  SUCCESS_OR_DIE ( gaspi_proc_term(GASPI_BLOCK) );

  return EXIT_SUCCESS;
}
